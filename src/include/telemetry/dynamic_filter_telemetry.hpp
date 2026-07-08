/*
 * Copyright 2025, Sirius Contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include "op/dynamic_filter_ids.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cucascade::memory {
class fixed_size_host_memory_resource;
class reservation_aware_resource_adaptor;
}  // namespace cucascade::memory

namespace sirius::memory {
class sirius_memory_reservation_manager;
}  // namespace sirius::memory

namespace sirius::telemetry {

class dynamic_filter_query_stats {
 public:
  struct counters_snapshot {
    std::size_t publications_total{};
    std::size_t published{};
    std::size_t no_materialization{};
    std::size_t failed{};
    std::size_t cancelled{};
    std::size_t feeder_queued{};
    std::size_t feeder_dispatched{};
    std::size_t feeder_prioritized{};
    std::size_t feeder_running_hwm{};
    std::size_t feeder_running{};
    std::size_t build_batches_delivered{};
    std::size_t pinned_build_hwm{};
    std::size_t pinned_build_end{};
    std::size_t hash_tables_hwm{};
    std::size_t hash_tables_end{};
    std::size_t replica_bytes_hwm{};
    std::size_t replica_bytes_end{};
    std::size_t replica_unavailable{};
  };

  static dynamic_filter_query_stats& instance();

  void begin_query(sirius::memory::sirius_memory_reservation_manager* manager);
  void end_query();
  void reset_for_testing();

  [[nodiscard]] op::dynamic_filter_publication_plan_id next_publication_plan_id();
  [[nodiscard]] op::dynamic_filter_target_id next_target_id();
  [[nodiscard]] op::dynamic_filter_channel_id next_channel_id();
  [[nodiscard]] op::dynamic_filter_filter_id next_filter_id();

  void count_build_batch_delivered();
  void on_build_batch_pinned();
  void on_build_state_released(std::size_t pinned, std::size_t tables);
  void on_hash_table_built();
  void add_replica_bytes(std::size_t bytes);
  void sub_replica_bytes(std::size_t bytes);
  void count_replica_unavailable();
  void count_publication_outcome(op::dynamic_filter_publication_outcome outcome,
                                 op::dynamic_filter_no_materialization_reason reason);

  void set_feeder_pipelines(std::unordered_set<const void*> pipelines);
  [[nodiscard]] bool is_feeder(const void* pipeline) const noexcept;
  void count_feeder_queued();
  void count_feeder_dispatch(bool prioritized);
  void feeder_running_inc();
  void feeder_running_dec();

  void record_channel_batch(op::dynamic_filter_channel_id channel,
                            std::int64_t rows_in,
                            std::int64_t rows_out,
                            bool post_publication,
                            std::uint32_t masks_applied,
                            std::uint32_t masks_skipped,
                            std::uint32_t replica_unavailable);
  void record_channel_passthrough(op::dynamic_filter_channel_id channel,
                                  std::size_t batches,
                                  std::int64_t rows);

  void sample_gpu_allocated(int device_id, std::size_t total_allocated_bytes);

  [[nodiscard]] counters_snapshot snapshot_for_testing() const noexcept;

 private:
  struct channel_coverage {
    std::size_t batches_pre{};
    std::int64_t rows_in_pre{};
    std::int64_t rows_out_pre{};
    std::size_t batches_post{};
    std::int64_t rows_in_post{};
    std::int64_t rows_out_post{};
    std::size_t passthrough_batches{};
    std::int64_t passthrough_rows{};
    std::size_t masks_applied{};
    std::size_t masks_skipped{};
    std::size_t replica_unavailable{};
  };

  struct gpu_space_sample {
    int device_id{};
    cucascade::memory::reservation_aware_resource_adaptor* resource{};
    std::size_t baseline_allocated{};
    std::size_t baseline_peak{};
    std::atomic<std::size_t> sampled_max{};

    gpu_space_sample() = default;
    gpu_space_sample(int id,
                     cucascade::memory::reservation_aware_resource_adaptor* r,
                     std::size_t allocated,
                     std::size_t peak)
      : device_id(id),
        resource(r),
        baseline_allocated(allocated),
        baseline_peak(peak),
        sampled_max(allocated)
    {
    }
    gpu_space_sample(gpu_space_sample&& other) noexcept;
    gpu_space_sample& operator=(gpu_space_sample&& other) noexcept;
  };

  struct host_space_sample {
    int device_id{};
    cucascade::memory::fixed_size_host_memory_resource* resource{};
    std::size_t baseline_allocated{};
    std::size_t baseline_peak{};
  };

  static void update_hwm(std::atomic<std::size_t>& hwm, std::size_t value) noexcept;
  static void saturating_sub(std::atomic<std::size_t>& value, std::size_t amount) noexcept;

  std::atomic<std::uint32_t> _next_publication_plan_id{1};
  std::atomic<std::uint32_t> _next_target_id{1};
  std::atomic<std::uint32_t> _next_channel_id{1};
  std::atomic<std::uint32_t> _next_filter_id{1};

  std::atomic<std::size_t> _published{};
  std::atomic<std::size_t> _no_mat_empty{};
  std::atomic<std::size_t> _no_mat_no_delivery{};
  std::atomic<std::size_t> _no_mat_mode{};
  std::atomic<std::size_t> _no_mat_policy{};
  std::atomic<std::size_t> _no_mat_source{};
  std::atomic<std::size_t> _no_mat_closed{};
  std::atomic<std::size_t> _failed{};
  std::atomic<std::size_t> _cancelled{};

  std::atomic<std::size_t> _feeder_queued{};
  std::atomic<std::size_t> _feeder_dispatched{};
  std::atomic<std::size_t> _feeder_prioritized{};
  std::atomic<std::size_t> _feeder_running{};
  std::atomic<std::size_t> _feeder_running_hwm{};

  std::atomic<std::size_t> _build_batches_delivered{};
  std::atomic<std::size_t> _pinned_builds{};
  std::atomic<std::size_t> _pinned_builds_hwm{};
  std::atomic<std::size_t> _hash_tables{};
  std::atomic<std::size_t> _hash_tables_hwm{};
  std::atomic<std::size_t> _replica_bytes{};
  std::atomic<std::size_t> _replica_bytes_hwm{};
  std::atomic<std::size_t> _replica_unavailable{};

  mutable std::shared_mutex _feeders_mutex;
  std::unordered_set<const void*> _feeder_pipelines;
  mutable std::mutex _coverage_mutex;
  std::unordered_map<op::dynamic_filter_channel_id, channel_coverage> _coverage;
  std::vector<gpu_space_sample> _gpu_spaces;
  std::vector<host_space_sample> _host_spaces;
};

}  // namespace sirius::telemetry
