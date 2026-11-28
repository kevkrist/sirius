/*
 * Copyright 2026, Sirius Contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file union_transfer.hpp
 * @brief Makes equally shaped byte arrays on several GPUs equal to their bitwise OR.
 *
 * `accumulated_bloom_builder::finish` is the only caller: its arrays are the per-GPU Bloom
 * partials. `unite` knows nothing about filters. Using the vocabulary of `union_plan.hpp`, it moves
 * every array chunk by chunk through up to three stages:
 *
 * 1. Merge: inside an island, each contributing member's chunk is copied device to device into the
 *    leader's scratch and ORed into the leader's chunk on the GPU.
 * 2. Cross: between islands, every contributing leader copies its chunk into pinned host memory,
 *    the host ORs those copies (`sirius::memory::host_or`), and every leader copies the result
 *    back. With a single contributing island the host ORs nothing and the other leaders copy that
 *    island's chunk.
 * 3. Fan-out: inside an island, every other member copies the leader's chunk device to device.
 *
 * A stage with nothing to do is skipped: GPUs that all reach each other never touch the host, and
 * GPUs that cannot reach each other at all use only the cross stage.
 */

#pragma once

#include "memory/host_or.hpp"

#include <rmm/resource_ref.hpp>

#include <cuda/stream>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace cucascade::memory {
class fixed_size_host_memory_resource;
class memory_space;
}  // namespace cucascade::memory

namespace sirius::op::detail {

/**
 * @brief One GPU's copy of the arrays.
 */
struct union_member {
  cucascade::memory::memory_space* space;  ///< The GPU's space; scratch and streams come from it
  ::cuda::stream_ref stream;           ///< Orders every use and every free of this member's arrays
  std::span<std::byte* const> arrays;  ///< One pointer per array; null for an array of no bytes
  bool contributed;                    ///< False promises that the arrays are all zero
};

/**
 * @brief The size of one array, identical on every member, and the size of its transfer chunks.
 */
struct union_shape {
  std::size_t bytes;        ///< A multiple of 16
  std::size_t chunk_bytes;  ///< A nonzero multiple of 16
};

/**
 * @brief Everything one `unite` call works on. All spans are borrowed for the call.
 */
struct union_request {
  std::span<union_member const> members;
  /// One island id per member. Members with equal ids must have pool access to each other
  /// (`sirius::memory::peer_islands`, or any finer grouping).
  std::span<std::uint8_t const> island_ids;
  std::span<union_shape const> shapes;          ///< One per array
  std::size_t root;                             ///< The member on the calling thread's current GPU
  ::cuda::stream_ref root_stream;               ///< A stream of the root GPU owned by the caller
  rmm::device_async_resource_ref root_scratch;  ///< Allocates the root's merge scratch
  /// Pinned fixed-size blocks for the cross stage; may be null when the islands never cross
  cucascade::memory::fixed_size_host_memory_resource* host;
};

/**
 * @brief What one `unite` call did and the workspace it held.
 */
struct union_report {
  std::uint8_t islands          = 0;  ///< Islands after any split
  std::uint8_t host_inputs      = 0;  ///< Islands that copied to the host; 0 without a cross stage
  std::uint8_t window_chunks    = 0;  ///< Chunks in flight per host input; 0 without a cross stage
  bool peer_copies              = false;  ///< Whether any merge or fan-out stage ran
  bool degraded                 = false;  ///< An island was split or the window was halved
  std::size_t host_window_bytes = 0;      ///< Pinned bytes held
  double host_or_ms             = 0;      ///< Time the calling thread spent in host ORs
  double host_wait_ms           = 0;      ///< Time it waited for the transfers the host ORs read
  std::vector<std::size_t> device_scratch_bytes;  ///< Per member: its tracked scratch charge
};

/**
 * @brief A borrowed `host_or_executor` with its type erased, so that `unite` compiles once.
 */
struct union_or_runner {
  using part_function = void (*)(void* context, unsigned part) noexcept;

  void* executor;      ///< The borrowed executor
  unsigned max_parts;  ///< Its `max_parts()`
  /// Calls the executor's `run` with a body that calls `part(context, index)`
  void (*run)(void* executor, unsigned parts, part_function part, void* context) noexcept;

  /// Borrows @p executor, which must outlive every use of the result.
  template <sirius::memory::host_or_executor Executor>
  [[nodiscard]] static union_or_runner of(Executor& executor) noexcept
  {
    return {&executor,
            static_cast<unsigned>(executor.max_parts()),
            [](void* erased, unsigned parts, part_function part, void* context) noexcept {
              static_cast<Executor*>(erased)->run(
                parts, [&](unsigned index) noexcept { part(context, index); });
            }};
  }
};

/**
 * @brief Makes every member's arrays equal to the bitwise OR of all members' arrays and returns
 * once every array is complete.
 *
 * Workspace is acquired before anything is submitted, and what is refused is replaced rather than
 * failing the call. An island whose merge scratch is refused is split into single-member islands,
 * which use the host instead. A pinned window refused at two chunks in flight is retried at one.
 * Each replacement is logged and reported as `degraded`. When nothing was contributed, or there is
 * one member, nothing is copied.
 *
 * On return, and on every exception but `unjoined_gpu_work`, nothing the call submitted is pending
 * and its workspace is released.
 *
 * @pre The root's GPU is current; every member's stream already orders all earlier writes of its
 * arrays; no other thread uses the arrays or streams
 * @throw accumulation_cuda_error if a CUDA call fails; submitted work is joined first
 * @throw unjoined_gpu_work if that join fails. The call has then abandoned its workspace without
 * freeing it, and the caller must keep the arrays allocated
 * @throw accumulation_invariant_error if the request is inconsistent, or the cross stage is needed
 * and `request.host` has no fixed-size blocks
 * @param request The arrays and where they live
 * @param or_executor Splits each host OR over threads; see `union_or_runner::of`
 * @return The report, or nullopt, with nothing submitted and nothing held, iff the smallest pinned
 * window or a host allocation during preparation was refused
 */
[[nodiscard]] std::optional<union_report> unite(union_request const& request,
                                                union_or_runner or_executor);

}  // namespace sirius::op::detail
