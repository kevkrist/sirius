/*
 * Copyright 2025, Sirius Contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License. You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied. See the License for the specific language governing permissions and limitations under
 * the License.
 */

#pragma once

// PROTOTYPE [xfer-PR] (proto/xfer-host): switches and host helpers of the rebuilt HOST-staged
// transfers. Throwaway: every switch is an environment variable read at each use, so one binary
// gives the A/B.
//
//   SIRIUS_PROTO_XFER_HOST       unset|0|off = tip loop; gpuor = star, OR on the root GPU, every
//                                leg pipelined through pinned HOST slots; hostor = OR on the CPU;
//                                auto = hostor with two or more sources, else gpuor
//   SIRIUS_PROTO_XFER_WINDOW     HOST slots per array in flight (default 4, at least 1)
//   SIRIUS_PROTO_XFER_OR_THREADS CPU threads of the host OR (default 16)
//   SIRIUS_PROTO_XFER_REPLICA    0|1: stage-once replica fan-out and batched exact-key gather
//                                (default: on iff SIRIUS_PROTO_XFER_HOST is on)
//   SIRIUS_PROTO_XFER_FAIL_HOST  1 = pretend the pinned HOST window cannot be allocated
//   SIRIUS_PROTO_XFER_CHECKSUM   1 = checksum every replica array after a Bloom finish
//   SIRIUS_PROTO_XFER_DUMP       1 = also copy every replica array to the host (byte compare)

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sirius::memory::proto_xfer {

enum class bloom_mode : std::uint8_t { off, gpuor, hostor, automatic };

[[nodiscard]] bloom_mode bloom_host_mode() noexcept;
[[nodiscard]] std::size_t window_slots() noexcept;
[[nodiscard]] unsigned or_threads() noexcept;
[[nodiscard]] bool replica_enabled() noexcept;
[[nodiscard]] bool fail_host_allocation() noexcept;
[[nodiscard]] bool checksum_enabled() noexcept;
[[nodiscard]] bool dump_enabled() noexcept;

/**
 * @brief ORs `inputs.size()` arrays held in equally sized HOST blocks into @p out, in parallel.
 *
 * Array `a` occupies blocks `inputs[a][0..]`, the result blocks `out[0..]`; every block has
 * @p block_bytes bytes and only the first @p bytes of each array are reduced.
 */
void host_or(std::span<std::byte* const> out,
             std::span<std::span<std::byte* const> const> inputs,
             std::size_t block_bytes,
             std::size_t bytes,
             unsigned threads);

/**
 * @brief What the last Bloom finish of this process did (benchmark and test observation only).
 */
struct finish_observation {
  int schedule{-1};  ///< -1 tip loop or PEER; 0 fan-out; 1 OR on the root GPU; 2 OR on the host
  bool fell_back{false};
  std::size_t host_bytes{0};
  std::size_t device_scratch_bytes{0};
  std::size_t host_waits{0};
  std::vector<std::uint64_t> checksums;  ///< Per partial, per key; with SIRIUS_PROTO_XFER_CHECKSUM
  /// Per partial, per key: the array's bytes; with SIRIUS_PROTO_XFER_DUMP (tests only)
  std::vector<std::vector<std::byte>> arrays;
};

void set_last_finish(finish_observation observation);
[[nodiscard]] finish_observation last_finish();

/// Counters over the process (replica and exact-key transfers)
struct counters {
  std::atomic<std::uint64_t> staged_once_hits{0};
  std::atomic<std::uint64_t> staged_sources{0};
  std::atomic<std::uint64_t> gather_batches{0};
};
[[nodiscard]] counters& stats() noexcept;

}  // namespace sirius::memory::proto_xfer
