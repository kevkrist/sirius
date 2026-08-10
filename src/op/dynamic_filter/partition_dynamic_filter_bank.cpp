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

#include "op/dynamic_filter/partition_dynamic_filter_bank.hpp"

#include "log/logging.hpp"
#include "op/dynamic_filter/dynamic_filter_source_policy.hpp"
#include "op/dynamic_filter/dynamic_filter_stats.hpp"
#include "op/dynamic_filter/sirius_dynamic_filter.hpp"

#include <rmm/cuda_device.hpp>

#include <cucascade/memory/common.hpp>
#include <cucascade/memory/memory_space.hpp>

#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace sirius::op {

namespace {

[[nodiscard]] std::size_t ceil_div(std::size_t numerator, std::size_t denominator) noexcept
{
  return numerator / denominator + static_cast<std::size_t>(numerator % denominator != 0);
}

[[nodiscard]] bool checked_multiply(std::size_t lhs, std::size_t rhs, std::size_t& product) noexcept
{
  if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) { return false; }
  product = lhs * rhs;
  return true;
}

void saturating_add(std::atomic<std::uint64_t>& value, std::uint64_t increment) noexcept
{
  auto current = value.load(std::memory_order_relaxed);
  while (true) {
    auto const maximum = std::numeric_limits<std::uint64_t>::max();
    auto const next    = increment > maximum - current ? maximum : current + increment;
    if (value.compare_exchange_weak(
          current, next, std::memory_order_relaxed, std::memory_order_relaxed)) {
      return;
    }
  }
}

}  // namespace

struct partition_dynamic_filter_bank::impl {
  struct partition_entry {
    std::mutex mutex;
    int device_id            = -1;
    std::uint64_t build_rows = 0;
    bool failed              = false;
    std::vector<std::shared_ptr<sirius_dynamic_bloom_filter>> mutable_filters;
    std::shared_ptr<sirius_dynamic_filter_set const> sealed_filters;
  };

  dynamic_filter_publish_plan const& plan;
  dynamic_filter_stats* stats;
  mutable std::shared_mutex lifecycle_mutex;
  std::atomic<state> lifecycle{state::pending_arm};
  std::vector<char> active_keys;
  std::vector<std::unique_ptr<partition_entry>> partitions;
  std::size_t rows_per_partition = 0;
  std::atomic<bool> bank_failure{false};

  explicit impl(dynamic_filter_publish_plan const& plan, dynamic_filter_stats* stats)
    : plan(plan), stats(stats), active_keys(plan.admitted_keys().size(), 0)
  {
    for (std::size_t key_index = 0; key_index < plan.admitted_keys().size(); ++key_index) {
      auto const& key = plan.admitted_keys()[key_index];
      active_keys[key_index] =
        static_cast<char>(key.storage_type == key.probe_storage_type &&
                          sirius_dynamic_bloom_filter::supports(key.storage_type));
    }
  }

  void record_failure() noexcept
  {
    if (stats != nullptr) {
      stats->partition_dynamic_filter_failures.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void fail_entry(partition_entry& entry,
                  std::size_t partition_idx,
                  std::string_view reason) noexcept
  {
    if (entry.failed) { return; }
    entry.failed = true;
    entry.mutable_filters.clear();
    record_failure();
    SIRIUS_LOG_WARN(
      "[partition_dynamic_filter_bank] partition {} disabled: {}", partition_idx, reason);
  }

  [[nodiscard]] bool arm(std::uint64_t global_build_rows,
                         std::size_t num_partitions,
                         std::size_t num_gpus) noexcept
  {
    std::unique_lock lifecycle_lock(lifecycle_mutex);
    if (lifecycle.load(std::memory_order_relaxed) != state::pending_arm) { return false; }

    auto disable_bank = [this] {
      lifecycle.store(state::disabled, std::memory_order_release);
      return false;
    };
    if (global_build_rows == 0 || num_partitions <= 1 || num_gpus == 0 ||
        global_build_rows > std::numeric_limits<std::size_t>::max()) {
      return disable_bank();
    }

    auto const build_rows        = static_cast<std::size_t>(global_build_rows);
    std::size_t active_key_count = 0;
    for (std::size_t key_index = 0; key_index < active_keys.size(); ++key_index) {
      if (active_keys[key_index] == 0) { continue; }

      auto const& key       = plan.admitted_keys()[key_index];
      auto const key_domain = key.build_key_domain_cardinality;
      if (stats != nullptr) {
        stats->partition_dynamic_filter_keys_considered.fetch_add(1, std::memory_order_relaxed);
        if (key_domain > 0) {
          stats->partition_dynamic_filter_keys_with_known_domain.fetch_add(
            1, std::memory_order_relaxed);
          if (build_rows > key_domain) {
            stats->partition_dynamic_filter_keys_build_exceeded_domain.fetch_add(
              1, std::memory_order_relaxed);
          }
        }
      }
      if (domain_coverage_gate_fires(build_rows,
                                     key_domain,
                                     key.build_key_proven_unique,
                                     plan.domain_coverage_threshold())) {
        active_keys[key_index] = 0;
        if (stats != nullptr) {
          stats->partition_dynamic_filter_keys_skipped_domain_gate.fetch_add(
            1, std::memory_order_relaxed);
        }
        SIRIUS_LOG_DEBUG(
          "[partition_dynamic_filter_bank] domain gate skipped key {}: {} build rows cover "
          "{:.2f} of domain {}.",
          key_index,
          build_rows,
          static_cast<double>(build_rows) / static_cast<double>(key_domain),
          key_domain);
        continue;
      }
      ++active_key_count;
    }
    if (active_key_count == 0) { return disable_bank(); }

    rows_per_partition     = ceil_div(static_cast<std::size_t>(global_build_rows), num_partitions);
    auto const bloom_bytes = sirius_dynamic_bloom_filter::estimated_bytes(rows_per_partition);
    auto const worst_partitions_per_gpu = ceil_div(num_partitions, num_gpus);
    std::size_t bytes_for_all_keys      = 0;
    std::size_t worst_bytes_per_gpu     = 0;
    if (bloom_bytes == std::numeric_limits<std::size_t>::max() ||
        !checked_multiply(active_key_count, bloom_bytes, bytes_for_all_keys) ||
        !checked_multiply(bytes_for_all_keys, worst_partitions_per_gpu, worst_bytes_per_gpu) ||
        worst_bytes_per_gpu > plan.max_bloom_bytes_per_gpu()) {
      if (stats != nullptr) {
        stats->partition_dynamic_filter_budget_skips.fetch_add(active_key_count,
                                                               std::memory_order_relaxed);
      }
      SIRIUS_LOG_INFO(
        "[partition_dynamic_filter_bank] disabled by policy: {} key(s), {} rows/partition, {} "
        "bytes/filter, {} worst partitions/GPU, {}-byte cap.",
        active_key_count,
        rows_per_partition,
        bloom_bytes,
        worst_partitions_per_gpu,
        plan.max_bloom_bytes_per_gpu());
      return disable_bank();
    }

    try {
      partitions.reserve(num_partitions);
      for (std::size_t partition_idx = 0; partition_idx < num_partitions; ++partition_idx) {
        partitions.push_back(std::make_unique<partition_entry>());
      }
    } catch (...) {
      partitions.clear();
      record_failure();
      lifecycle.store(state::failed, std::memory_order_release);
      return false;
    }

    SIRIUS_LOG_INFO(
      "[partition_dynamic_filter_bank] armed {} partition(s), {} active key(s), geometry={} "
      "rows/partition, worst allocator-accounted footprint={} bytes/GPU.",
      num_partitions,
      active_key_count,
      rows_per_partition,
      worst_bytes_per_gpu);
    lifecycle.store(state::accumulating, std::memory_order_release);
    return true;
  }

  void abandon_partition(std::size_t partition_idx, std::string_view reason) noexcept
  {
    std::unique_lock lifecycle_lock(lifecycle_mutex);
    auto const observed = lifecycle.load(std::memory_order_acquire);
    if (observed == state::pending_arm) {
      bank_failure.store(true, std::memory_order_release);
      record_failure();
      SIRIUS_LOG_WARN("[partition_dynamic_filter_bank] disabled before arming partition {}: {}",
                      partition_idx,
                      reason);
      lifecycle.store(state::failed, std::memory_order_release);
      return;
    }
    if (observed != state::accumulating) { return; }
    if (partition_idx >= partitions.size()) {
      bank_failure.store(true, std::memory_order_release);
      record_failure();
      SIRIUS_LOG_WARN(
        "[partition_dynamic_filter_bank] abandonment used out-of-range partition {} of {}: {}",
        partition_idx,
        partitions.size(),
        reason);
      return;
    }

    auto& entry = *partitions[partition_idx];
    std::scoped_lock partition_lock(entry.mutex);
    fail_entry(entry, partition_idx, reason);
  }

  void contribute(std::size_t partition_idx,
                  cudf::table_view const& build_view,
                  int device_id,
                  cucascade::memory::memory_space& space,
                  rmm::cuda_stream_view stream) noexcept
  {
    std::shared_lock lifecycle_lock(lifecycle_mutex);
    if (lifecycle.load(std::memory_order_acquire) != state::accumulating) { return; }
    if (partition_idx >= partitions.size()) {
      bank_failure.store(true, std::memory_order_release);
      record_failure();
      SIRIUS_LOG_WARN(
        "[partition_dynamic_filter_bank] contribution used out-of-range partition {} of {}.",
        partition_idx,
        partitions.size());
      return;
    }

    if (build_view.num_rows() == 0) { return; }

    auto& entry = *partitions[partition_idx];
    std::scoped_lock partition_lock(entry.mutex);
    if (entry.failed) { return; }

    auto fail_partition = [&](std::string_view reason) {
      fail_entry(entry, partition_idx, reason);
    };

    auto drain_stream = [&]() noexcept {
      try {
        rmm::cuda_set_device_raii device_guard{rmm::cuda_device_id{device_id}};
        stream.synchronize();
      } catch (...) {
      }
    };

    if (space.get_tier() != cucascade::memory::Tier::GPU || space.get_device_id() != device_id) {
      fail_partition("contribution memory space does not match its actual GPU");
      return;
    }
    if (entry.device_id >= 0 && entry.device_id != device_id) {
      if (stats != nullptr) {
        stats->partition_dynamic_filter_device_mismatches.fetch_add(1, std::memory_order_relaxed);
      }
      fail_partition("multiple build GPUs contributed to one hash partition");
      return;
    }

    try {
      rmm::cuda_set_device_raii device_guard{rmm::cuda_device_id{device_id}};

      for (std::size_t key_index = 0; key_index < active_keys.size(); ++key_index) {
        if (active_keys[key_index] == 0) { continue; }
        auto const& key = plan.admitted_keys()[key_index];
        if (key.build_key_ordinal >= build_view.num_columns() ||
            build_view.column(key.build_key_ordinal).type() != key.storage_type) {
          fail_partition("runtime build key does not match the admitted ordinal and type");
          return;
        }
      }

      entry.device_id = device_id;
      entry.mutable_filters.resize(active_keys.size());
      for (std::size_t key_index = 0; key_index < active_keys.size(); ++key_index) {
        if (active_keys[key_index] == 0) { continue; }
        auto const& key = plan.admitted_keys()[key_index];
        if (!entry.mutable_filters[key_index]) {
          entry.mutable_filters[key_index] = std::make_shared<sirius_dynamic_bloom_filter>(
            key.storage_type, rows_per_partition, stream, space.get_default_allocator());
        }
        entry.mutable_filters[key_index]->add(build_view.column(key.build_key_ordinal), stream);
      }
      stream.synchronize();

      auto const rows    = static_cast<std::uint64_t>(build_view.num_rows());
      auto const maximum = std::numeric_limits<std::uint64_t>::max();
      entry.build_rows   = rows > maximum - entry.build_rows ? maximum : entry.build_rows + rows;
      if (stats != nullptr) {
        stats->partition_dynamic_filter_build_fragments.fetch_add(1, std::memory_order_relaxed);
        saturating_add(stats->partition_dynamic_filter_build_rows, rows);
      }
    } catch (std::exception const& error) {
      drain_stream();
      fail_partition(error.what());
    } catch (...) {
      drain_stream();
      fail_partition("unknown Bloom construction or insertion failure");
    }
  }

  void seal() noexcept
  {
    std::unique_lock lifecycle_lock(lifecycle_mutex);
    auto const observed = lifecycle.load(std::memory_order_acquire);
    if (observed != state::accumulating) {
      if (observed == state::pending_arm) {
        lifecycle.store(state::disabled, std::memory_order_release);
      }
      return;
    }
    if (bank_failure.load(std::memory_order_acquire)) {
      for (auto& entry : partitions) {
        entry->mutable_filters.clear();
      }
      lifecycle.store(state::failed, std::memory_order_release);
      return;
    }

    std::uint64_t built_partitions = 0;
    std::uint64_t built_filters    = 0;
    std::uint64_t skewed           = 0;
    try {
      for (std::size_t partition_idx = 0; partition_idx < partitions.size(); ++partition_idx) {
        auto& entry = *partitions[partition_idx];
        if (entry.build_rows > rows_per_partition) {
          ++skewed;
          SIRIUS_LOG_INFO(
            "[partition_dynamic_filter_bank] partition {} contributed {} rows above geometry {} "
            "(Bloom false-positive rate may rise; membership remains no-false-negative).",
            partition_idx,
            entry.build_rows,
            rows_per_partition);
        }
        if (entry.failed || entry.mutable_filters.empty()) {
          entry.mutable_filters.clear();
          continue;
        }

        auto filters                       = std::make_shared<sirius_dynamic_filter_set>();
        std::uint64_t filters_in_partition = 0;
        for (std::size_t key_index = 0; key_index < entry.mutable_filters.size(); ++key_index) {
          auto& filter = entry.mutable_filters[key_index];
          if (!filter) { continue; }
          auto const ordinal =
            static_cast<std::size_t>(plan.admitted_keys()[key_index].probe_key_ordinal);
          if (filters->push_filter(ordinal, filter)) { ++filters_in_partition; }
        }
        filters->close_for_new_filters();
        if (filters_in_partition > 0) {
          entry.sealed_filters = std::move(filters);
          ++built_partitions;
          built_filters += filters_in_partition;
        }
        entry.mutable_filters.clear();
      }
    } catch (...) {
      for (auto& entry : partitions) {
        entry->sealed_filters.reset();
        entry->mutable_filters.clear();
      }
      record_failure();
      lifecycle.store(state::failed, std::memory_order_release);
      return;
    }

    if (stats != nullptr) {
      stats->partition_dynamic_filter_partitions_built.fetch_add(built_partitions,
                                                                 std::memory_order_relaxed);
      stats->partition_dynamic_filter_filters_built.fetch_add(built_filters,
                                                              std::memory_order_relaxed);
      stats->partition_dynamic_filter_skewed_partitions.fetch_add(skewed,
                                                                  std::memory_order_relaxed);
    }
    SIRIUS_LOG_INFO(
      "[partition_dynamic_filter_bank] sealed {} filter(s) across {} of {} partition(s), geometry "
      "{} rows, {} skewed partition(s).",
      built_filters,
      built_partitions,
      partitions.size(),
      rows_per_partition,
      skewed);
    lifecycle.store(state::sealed, std::memory_order_release);
  }

  void disable() noexcept
  {
    std::unique_lock lifecycle_lock(lifecycle_mutex);
    auto const observed = lifecycle.load(std::memory_order_acquire);
    if (observed == state::pending_arm || observed == state::accumulating) {
      for (auto& entry : partitions) {
        entry->mutable_filters.clear();
      }
      lifecycle.store(state::disabled, std::memory_order_release);
    }
  }

  [[nodiscard]] selection select(std::size_t partition_idx, int device_id) const noexcept
  {
    auto const observed = lifecycle.load(std::memory_order_acquire);
    if (observed == state::pending_arm || observed == state::accumulating) {
      return {.result = selection::status::not_ready};
    }
    if (observed == state::failed) { return {.result = selection::status::failed}; }
    if (observed != state::sealed || partition_idx >= partitions.size()) {
      return {.result = selection::status::missing};
    }

    auto const& entry = *partitions[partition_idx];
    if (entry.failed) { return {.result = selection::status::failed}; }
    if (entry.device_id >= 0 && entry.device_id != device_id) {
      if (stats != nullptr) {
        stats->partition_dynamic_filter_device_mismatches.fetch_add(1, std::memory_order_relaxed);
      }
      return {.result = selection::status::device_mismatch};
    }
    if (!entry.sealed_filters) { return {.result = selection::status::missing}; }
    return {.result = selection::status::available, .filters = entry.sealed_filters};
  }
};

partition_dynamic_filter_bank::partition_dynamic_filter_bank(
  dynamic_filter_publish_plan const& plan, dynamic_filter_stats* stats)
  : _impl(std::make_unique<impl>(plan, stats))
{
}

partition_dynamic_filter_bank::~partition_dynamic_filter_bank() = default;

bool partition_dynamic_filter_bank::arm(std::uint64_t global_build_rows,
                                        std::size_t num_partitions,
                                        std::size_t num_gpus) noexcept
{
  return _impl->arm(global_build_rows, num_partitions, num_gpus);
}

void partition_dynamic_filter_bank::contribute(std::size_t partition_idx,
                                               cudf::table_view const& build_view,
                                               int device_id,
                                               cucascade::memory::memory_space& space,
                                               rmm::cuda_stream_view stream) noexcept
{
  _impl->contribute(partition_idx, build_view, device_id, space, stream);
}

void partition_dynamic_filter_bank::abandon_partition(std::size_t partition_idx,
                                                      std::string_view reason) noexcept
{
  _impl->abandon_partition(partition_idx, reason);
}

void partition_dynamic_filter_bank::seal() noexcept { _impl->seal(); }

void partition_dynamic_filter_bank::disable() noexcept { _impl->disable(); }

partition_dynamic_filter_bank::state partition_dynamic_filter_bank::current_state() const noexcept
{
  return _impl->lifecycle.load(std::memory_order_acquire);
}

bool partition_dynamic_filter_bank::probe_may_proceed() const noexcept
{
  auto const observed = current_state();
  return observed != state::pending_arm && observed != state::accumulating;
}

partition_dynamic_filter_bank::selection partition_dynamic_filter_bank::select(
  std::size_t partition_idx, int device_id) const noexcept
{
  return _impl->select(partition_idx, device_id);
}

void partition_dynamic_filter_bank::record_readiness_wait() noexcept
{
  if (_impl->stats != nullptr) {
    _impl->stats->partition_dynamic_filter_readiness_waits.fetch_add(1, std::memory_order_relaxed);
  }
}

void partition_dynamic_filter_bank::record_probe(std::uint64_t rows_in,
                                                 std::uint64_t rows_out) noexcept
{
  if (_impl->stats == nullptr) { return; }
  _impl->stats->partition_dynamic_filter_probe_batches.fetch_add(1, std::memory_order_relaxed);
  saturating_add(_impl->stats->partition_dynamic_filter_probe_rows_in, rows_in);
  saturating_add(_impl->stats->partition_dynamic_filter_probe_rows_out, rows_out);
}

void partition_dynamic_filter_bank::record_probe_failure() noexcept { _impl->record_failure(); }

std::size_t partition_dynamic_filter_bank::rows_per_partition_geometry() const noexcept
{
  std::shared_lock lock(_impl->lifecycle_mutex);
  return _impl->rows_per_partition;
}

}  // namespace sirius::op
