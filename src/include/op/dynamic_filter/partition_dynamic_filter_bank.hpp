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
 * @file partition_dynamic_filter_bank.hpp
 * @brief Private hash-join storage for partition-specific Bloom filters
 *
 * `partition_dynamic_filter_bank` accumulates each post-partition build fragment into its exact
 * hash partition. Mutable filters never enter a `sirius_dynamic_filter_set`; sealing stops
 * contributions before exposing closed, immutable per-partition sets.
 */

#pragma once

#include "op/dynamic_filter/dynamic_filter_publish_plan.hpp"

#include <cudf/table/table_view.hpp>

#include <rmm/cuda_stream_view.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
namespace cucascade::memory {
class memory_space;
}

namespace sirius::op {

struct dynamic_filter_stats;
class sirius_dynamic_filter_set;

/**
 * @brief Hash-join-owned bank of sealed, partition-specific Bloom filters
 *
 * The bank starts in `pending_arm`. Contributions for one partition are serialized, while different
 * partitions can accumulate concurrently. Any optional failure leaves the authoritative join
 * unchanged and makes the affected probe partition, or the whole bank for a bank-level failure,
 * pass through.
 *
 * Filter storage uses the contributing batch's GPU memory space and reservation-aware default
 * allocator. The policy cap is a checked pre-admission bound, not an explicit detached reservation.
 */
class partition_dynamic_filter_bank final {
 public:
  /**
   * @brief Bank lifecycle visible to task creation
   */
  enum class state : std::uint8_t {
    pending_arm,   ///< Exact build sizing has not completed
    accumulating,  ///< Geometry is fixed and build contributions are accepted
    sealed,        ///< Immutable per-partition snapshots are available
    failed,        ///< A bank-level failure disabled filtering
    disabled       ///< Runtime shape or policy admission disabled filtering
  };

  /**
   * @brief Result of selecting one sealed partition
   */
  struct selection {
    enum class status : std::uint8_t {
      available,       ///< A sealed filter set matches the partition and device
      not_ready,       ///< The bank is pending or accumulating
      missing,         ///< The sealed partition has no usable filter
      failed,          ///< The bank or partition failed
      device_mismatch  ///< Probe and recorded build devices differ
    };

    status result = status::missing;
    std::shared_ptr<sirius_dynamic_filter_set const> filters;
  };

  /**
   * @brief Construct an unarmed bank from immutable join metadata
   *
   * @p plan may have no ordinary probe targets or replica placements. Local construction uses each
   * contribution's actual GPU memory space.
   *
   * @param[in] plan Immutable key and policy metadata that outlives the bank
   * @param[in] stats Optional connection-lifetime local-filter counter sink
   */
  explicit partition_dynamic_filter_bank(dynamic_filter_publish_plan const& plan,
                                         dynamic_filter_stats* stats = nullptr);
  ~partition_dynamic_filter_bank();

  partition_dynamic_filter_bank(partition_dynamic_filter_bank const&)            = delete;
  partition_dynamic_filter_bank& operator=(partition_dynamic_filter_bank const&) = delete;

  /**
   * @brief Fix geometry and admit the aggregate worst-case per-GPU footprint
   *
   * Geometry is `ceil(global_build_rows / num_partitions)`. Admission checks `active_keys *
   * estimated_bytes(geometry) * ceil(num_partitions / num_gpus)` without overflow against the plan
   * policy cap.
   *
   * @param[in] global_build_rows Exact pre-scatter build row count
   * @param[in] num_partitions Exact non-broadcast hash partition count
   * @param[in] num_gpus Number of active GPUs
   * @return True only when the bank transitioned to `accumulating`
   */
  [[nodiscard]] bool arm(std::uint64_t global_build_rows,
                         std::size_t num_partitions,
                         std::size_t num_gpus) noexcept;

  /**
   * @brief Union one post-partition, post-CONCAT build fragment into partition @p partition_idx
   *
   * The caller orders @p stream after the fragment's writer event. This function synchronizes @p
   * stream before returning. Failures are contained and recorded.
   *
   * @param[in] partition_idx Exact hash partition index
   * @param[in] build_view Build fragment indexed by admitted build-key ordinals
   * @param[in] device_id Actual GPU that owns @p build_view
   * @param[in] space GPU memory space that owns @p build_view and @p stream
   * @param[in] stream Durable stream acquired from @p space
   */
  void contribute(std::size_t partition_idx,
                  cudf::table_view const& build_view,
                  int device_id,
                  cucascade::memory::memory_space& space,
                  rmm::cuda_stream_view stream) noexcept;

  /**
   * @brief Make one partition permanently unavailable before sealing
   *
   * Call this when a build fragment cannot be contributed. Existing mutable filters for the
   * partition are discarded so an incomplete Bloom filter can never reach the probe.
   *
   * @param[in] partition_idx Exact hash partition index
   * @param[in] reason Diagnostic reason for abandoning the partition
   */
  void abandon_partition(std::size_t partition_idx, std::string_view reason) noexcept;
  /**
   * @brief Wait for accepted contributions and expose complete immutable snapshots
   */
  void seal() noexcept;

  /**
   * @brief Disable a bank that cannot be armed or completed
   */
  void disable() noexcept;

  /**
   * @brief Current lifecycle state
   */
  [[nodiscard]] state current_state() const noexcept;

  /**
   * @brief Whether task creation may release probe CONCAT work
   *
   * Only `pending_arm` and `accumulating` hold probe task creation; terminal states fail open.
   */
  [[nodiscard]] bool probe_may_proceed() const noexcept;

  /**
   * @brief Select one immutable partition set for an actual probe GPU
   */
  [[nodiscard]] selection select(std::size_t partition_idx, int device_id) const noexcept;

  /**
   * @brief Record one task-creation readiness wait
   */
  void record_readiness_wait() noexcept;

  /**
   * @brief Record rows for one probe batch after optional filtering
   */
  void record_probe(std::uint64_t rows_in, std::uint64_t rows_out) noexcept;

  /**
   * @brief Record a contained probe-apply failure
   */
  void record_probe_failure() noexcept;

  /**
   * @brief Per-partition geometry, or zero before successful arming
   */
  [[nodiscard]] std::size_t rows_per_partition_geometry() const noexcept;

 private:
  struct impl;
  std::unique_ptr<impl> _impl;
};

}  // namespace sirius::op
