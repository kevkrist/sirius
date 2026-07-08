/*
 * Copyright 2025, Sirius Contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "telemetry/dynamic_filter_telemetry.hpp"

#include "log/logging.hpp"
#include "memory/sirius_memory_reservation_manager.hpp"

#include <cucascade/memory/fixed_size_host_memory_resource.hpp>
#include <cucascade/memory/memory_space.hpp>
#include <cucascade/memory/reservation_aware_resource_adaptor.hpp>

#include <algorithm>
#include <utility>

namespace sirius::op {

const char* to_string(dynamic_filter_publication_outcome outcome) noexcept
{
  switch (outcome) {
    case dynamic_filter_publication_outcome::PUBLISHED: return "PUBLISHED";
    case dynamic_filter_publication_outcome::NO_MATERIALIZATION: return "NO_MATERIALIZATION";
    case dynamic_filter_publication_outcome::FAILED: return "FAILED";
    case dynamic_filter_publication_outcome::CANCELLED: return "CANCELLED";
  }
  return "UNKNOWN";
}

const char* to_string(dynamic_filter_no_materialization_reason reason) noexcept
{
  switch (reason) {
    case dynamic_filter_no_materialization_reason::NONE: return "NONE";
    case dynamic_filter_no_materialization_reason::EMPTY_BUILD: return "EMPTY_BUILD";
    case dynamic_filter_no_materialization_reason::NO_BUILD_DELIVERY: return "NO_BUILD_DELIVERY";
    case dynamic_filter_no_materialization_reason::UNSUPPORTED_MODE: return "UNSUPPORTED_MODE";
    case dynamic_filter_no_materialization_reason::POLICY_SKIPPED: return "POLICY_SKIPPED";
    case dynamic_filter_no_materialization_reason::SOURCE_UNAVAILABLE: return "SOURCE_UNAVAILABLE";
    case dynamic_filter_no_materialization_reason::CONSUMER_CLOSED: return "CONSUMER_CLOSED";
  }
  return "UNKNOWN";
}

}  // namespace sirius::op

namespace sirius::telemetry {

dynamic_filter_query_stats::gpu_space_sample::gpu_space_sample(gpu_space_sample&& other) noexcept
  : device_id(other.device_id),
    resource(other.resource),
    baseline_allocated(other.baseline_allocated),
    baseline_peak(other.baseline_peak),
    sampled_max(other.sampled_max.load(std::memory_order_relaxed))
{
}

dynamic_filter_query_stats::gpu_space_sample&
dynamic_filter_query_stats::gpu_space_sample::operator=(gpu_space_sample&& other) noexcept
{
  device_id          = other.device_id;
  resource           = other.resource;
  baseline_allocated = other.baseline_allocated;
  baseline_peak      = other.baseline_peak;
  sampled_max.store(other.sampled_max.load(std::memory_order_relaxed), std::memory_order_relaxed);
  return *this;
}

dynamic_filter_query_stats& dynamic_filter_query_stats::instance()
{
  static dynamic_filter_query_stats stats;
  return stats;
}

void dynamic_filter_query_stats::update_hwm(std::atomic<std::size_t>& hwm,
                                            std::size_t value) noexcept
{
  auto current = hwm.load(std::memory_order_relaxed);
  while (current < value && !hwm.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
  }
}

void dynamic_filter_query_stats::saturating_sub(std::atomic<std::size_t>& value,
                                                std::size_t amount) noexcept
{
  auto current = value.load(std::memory_order_relaxed);
  while (!value.compare_exchange_weak(
    current, current > amount ? current - amount : 0, std::memory_order_relaxed)) {}
}

void dynamic_filter_query_stats::reset_for_testing()
{
  _next_publication_plan_id.store(1, std::memory_order_relaxed);
  _next_target_id.store(1, std::memory_order_relaxed);
  _next_channel_id.store(1, std::memory_order_relaxed);
  _next_filter_id.store(1, std::memory_order_relaxed);
  for (auto* counter :
       {&_published,         &_no_mat_empty,       &_no_mat_no_delivery,      &_no_mat_mode,
        &_no_mat_policy,     &_no_mat_source,      &_no_mat_closed,           &_failed,
        &_cancelled,         &_feeder_queued,      &_feeder_dispatched,       &_feeder_prioritized,
        &_feeder_running,    &_feeder_running_hwm, &_build_batches_delivered, &_pinned_builds,
        &_pinned_builds_hwm, &_hash_tables,        &_hash_tables_hwm,         &_replica_bytes,
        &_replica_bytes_hwm, &_replica_unavailable}) {
    counter->store(0, std::memory_order_relaxed);
  }
  {
    std::unique_lock lock(_feeders_mutex);
    _feeder_pipelines.clear();
  }
  {
    std::scoped_lock lock(_coverage_mutex);
    _coverage.clear();
  }
  _gpu_spaces.clear();
  _host_spaces.clear();
}

void dynamic_filter_query_stats::begin_query(
  sirius::memory::sirius_memory_reservation_manager* manager)
{
  reset_for_testing();
  if (!manager) { return; }
  for (auto* space : manager->get_memory_spaces_for_tier(cucascade::memory::Tier::GPU)) {
    auto* resource =
      space->get_memory_resource_as<cucascade::memory::reservation_aware_resource_adaptor>();
    if (!resource) { continue; }
    _gpu_spaces.emplace_back(space->get_device_id(),
                             resource,
                             resource->get_total_allocated_bytes(),
                             resource->get_peak_total_allocated_bytes());
  }
  for (auto* space : manager->get_memory_spaces_for_tier(cucascade::memory::Tier::HOST)) {
    auto* resource =
      space->get_memory_resource_as<cucascade::memory::fixed_size_host_memory_resource>();
    if (!resource) { continue; }
    _host_spaces.push_back({space->get_device_id(),
                            resource,
                            resource->get_total_allocated_bytes(),
                            resource->get_peak_total_allocated_bytes()});
  }
}

void dynamic_filter_query_stats::end_query()
{
  auto const total =
    _published.load(std::memory_order_relaxed) + _no_mat_empty.load(std::memory_order_relaxed) +
    _no_mat_no_delivery.load(std::memory_order_relaxed) +
    _no_mat_mode.load(std::memory_order_relaxed) + _no_mat_policy.load(std::memory_order_relaxed) +
    _no_mat_source.load(std::memory_order_relaxed) +
    _no_mat_closed.load(std::memory_order_relaxed) + _failed.load(std::memory_order_relaxed) +
    _cancelled.load(std::memory_order_relaxed);
  SIRIUS_LOG_INFO(
    "[dynf_summary] publications total={} published={} no_mat_empty={} no_mat_no_delivery={} "
    "no_mat_mode={} no_mat_policy={} no_mat_source={} no_mat_closed={} failed={} cancelled={}",
    total,
    _published.load(std::memory_order_relaxed),
    _no_mat_empty.load(std::memory_order_relaxed),
    _no_mat_no_delivery.load(std::memory_order_relaxed),
    _no_mat_mode.load(std::memory_order_relaxed),
    _no_mat_policy.load(std::memory_order_relaxed),
    _no_mat_source.load(std::memory_order_relaxed),
    _no_mat_closed.load(std::memory_order_relaxed),
    _failed.load(std::memory_order_relaxed),
    _cancelled.load(std::memory_order_relaxed));
  auto const running_end = _feeder_running.load(std::memory_order_relaxed);
  if (running_end != 0) {
    SIRIUS_LOG_WARN("[dynf_summary] feeder running_end={} (unbalanced feeder gauge)", running_end);
  }
  SIRIUS_LOG_INFO(
    "[dynf_summary] feeder queued={} dispatched={} prioritized={} running_hwm={} running_end={}",
    _feeder_queued.load(std::memory_order_relaxed),
    _feeder_dispatched.load(std::memory_order_relaxed),
    _feeder_prioritized.load(std::memory_order_relaxed),
    _feeder_running_hwm.load(std::memory_order_relaxed),
    running_end);
  SIRIUS_LOG_INFO(
    "[dynf_summary] builds delivered={} pinned_hwm={} pinned_end={} tables_hwm={} tables_end={}",
    _build_batches_delivered.load(std::memory_order_relaxed),
    _pinned_builds_hwm.load(std::memory_order_relaxed),
    _pinned_builds.load(std::memory_order_relaxed),
    _hash_tables_hwm.load(std::memory_order_relaxed),
    _hash_tables.load(std::memory_order_relaxed));
  SIRIUS_LOG_INFO("[dynf_summary] replicas bytes_hwm={} bytes_end={} unavailable={}",
                  _replica_bytes_hwm.load(std::memory_order_relaxed),
                  _replica_bytes.load(std::memory_order_relaxed),
                  _replica_unavailable.load(std::memory_order_relaxed));

  for (auto const& space : _gpu_spaces) {
    auto const end_peak = space.resource->get_peak_total_allocated_bytes();
    auto const exact    = end_peak > space.baseline_peak;
    auto const bytes    = std::max(space.sampled_max.load(std::memory_order_relaxed),
                                exact ? end_peak : std::size_t{0});
    SIRIUS_LOG_INFO(
      "[dynf_summary] high_water space=GPU:{} bytes={} baseline_allocated={} exact={}",
      space.device_id,
      bytes,
      space.baseline_allocated,
      exact ? 1 : 0);
  }
  for (auto const& space : _host_spaces) {
    auto const end_peak = space.resource->get_peak_total_allocated_bytes();
    auto const exact    = end_peak > space.baseline_peak;
    SIRIUS_LOG_INFO(
      "[dynf_summary] high_water space=HOST:{} bytes={} baseline_allocated={} exact={}",
      space.device_id,
      std::max(space.baseline_allocated, exact ? end_peak : std::size_t{0}),
      space.baseline_allocated,
      exact ? 1 : 0);
  }

  std::scoped_lock lock(_coverage_mutex);
  for (auto const& [channel, c] : _coverage) {
    SIRIUS_LOG_INFO(
      "[dynf_summary] channel_coverage channel={} batches_pre={} rows_in_pre={} rows_out_pre={} "
      "batches_post={} rows_in_post={} rows_out_post={} passthrough_batches={} "
      "passthrough_rows={} masks_applied={} masks_skipped={} replica_unavailable={}",
      channel,
      c.batches_pre,
      c.rows_in_pre,
      c.rows_out_pre,
      c.batches_post,
      c.rows_in_post,
      c.rows_out_post,
      c.passthrough_batches,
      c.passthrough_rows,
      c.masks_applied,
      c.masks_skipped,
      c.replica_unavailable);
  }
}

op::dynamic_filter_publication_plan_id dynamic_filter_query_stats::next_publication_plan_id()
{
  return _next_publication_plan_id.fetch_add(1, std::memory_order_relaxed);
}
op::dynamic_filter_target_id dynamic_filter_query_stats::next_target_id()
{
  return _next_target_id.fetch_add(1, std::memory_order_relaxed);
}
op::dynamic_filter_channel_id dynamic_filter_query_stats::next_channel_id()
{
  return _next_channel_id.fetch_add(1, std::memory_order_relaxed);
}
op::dynamic_filter_filter_id dynamic_filter_query_stats::next_filter_id()
{
  return _next_filter_id.fetch_add(1, std::memory_order_relaxed);
}

void dynamic_filter_query_stats::count_build_batch_delivered()
{
  _build_batches_delivered.fetch_add(1, std::memory_order_relaxed);
}
void dynamic_filter_query_stats::on_build_batch_pinned()
{
  auto const value = _pinned_builds.fetch_add(1, std::memory_order_relaxed) + 1;
  update_hwm(_pinned_builds_hwm, value);
}
void dynamic_filter_query_stats::on_build_state_released(std::size_t pinned, std::size_t tables)
{
  saturating_sub(_pinned_builds, pinned);
  saturating_sub(_hash_tables, tables);
}
void dynamic_filter_query_stats::on_hash_table_built()
{
  auto const value = _hash_tables.fetch_add(1, std::memory_order_relaxed) + 1;
  update_hwm(_hash_tables_hwm, value);
}
void dynamic_filter_query_stats::add_replica_bytes(std::size_t bytes)
{
  auto const value = _replica_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
  update_hwm(_replica_bytes_hwm, value);
}
void dynamic_filter_query_stats::sub_replica_bytes(std::size_t bytes)
{
  saturating_sub(_replica_bytes, bytes);
}
void dynamic_filter_query_stats::count_replica_unavailable()
{
  _replica_unavailable.fetch_add(1, std::memory_order_relaxed);
}

void dynamic_filter_query_stats::count_publication_outcome(
  op::dynamic_filter_publication_outcome outcome,
  op::dynamic_filter_no_materialization_reason reason)
{
  using outcome_t = op::dynamic_filter_publication_outcome;
  using reason_t  = op::dynamic_filter_no_materialization_reason;
  if (outcome == outcome_t::PUBLISHED) {
    _published.fetch_add(1, std::memory_order_relaxed);
  } else if (outcome == outcome_t::FAILED) {
    _failed.fetch_add(1, std::memory_order_relaxed);
  } else if (outcome == outcome_t::CANCELLED) {
    _cancelled.fetch_add(1, std::memory_order_relaxed);
  } else {
    switch (reason) {
      case reason_t::EMPTY_BUILD: _no_mat_empty.fetch_add(1, std::memory_order_relaxed); break;
      case reason_t::NO_BUILD_DELIVERY:
        _no_mat_no_delivery.fetch_add(1, std::memory_order_relaxed);
        break;
      case reason_t::UNSUPPORTED_MODE: _no_mat_mode.fetch_add(1, std::memory_order_relaxed); break;
      case reason_t::POLICY_SKIPPED: _no_mat_policy.fetch_add(1, std::memory_order_relaxed); break;
      case reason_t::SOURCE_UNAVAILABLE:
        _no_mat_source.fetch_add(1, std::memory_order_relaxed);
        break;
      case reason_t::CONSUMER_CLOSED: _no_mat_closed.fetch_add(1, std::memory_order_relaxed); break;
      case reason_t::NONE: _no_mat_policy.fetch_add(1, std::memory_order_relaxed); break;
    }
  }
}

void dynamic_filter_query_stats::set_feeder_pipelines(std::unordered_set<const void*> pipelines)
{
  std::unique_lock lock(_feeders_mutex);
  _feeder_pipelines = std::move(pipelines);
}
bool dynamic_filter_query_stats::is_feeder(const void* pipeline) const noexcept
{
  std::shared_lock lock(_feeders_mutex);
  return _feeder_pipelines.contains(pipeline);
}
void dynamic_filter_query_stats::count_feeder_queued()
{
  _feeder_queued.fetch_add(1, std::memory_order_relaxed);
}
void dynamic_filter_query_stats::count_feeder_dispatch(bool prioritized)
{
  _feeder_dispatched.fetch_add(1, std::memory_order_relaxed);
  if (prioritized) { _feeder_prioritized.fetch_add(1, std::memory_order_relaxed); }
}
void dynamic_filter_query_stats::feeder_running_inc()
{
  auto const value = _feeder_running.fetch_add(1, std::memory_order_relaxed) + 1;
  update_hwm(_feeder_running_hwm, value);
}
void dynamic_filter_query_stats::feeder_running_dec() { saturating_sub(_feeder_running, 1); }

void dynamic_filter_query_stats::record_channel_batch(op::dynamic_filter_channel_id channel,
                                                      std::int64_t rows_in,
                                                      std::int64_t rows_out,
                                                      bool post_publication,
                                                      std::uint32_t masks_applied,
                                                      std::uint32_t masks_skipped,
                                                      std::uint32_t replica_unavailable)
{
  std::scoped_lock lock(_coverage_mutex);
  auto& c = _coverage[channel];
  if (post_publication) {
    ++c.batches_post;
    c.rows_in_post += rows_in;
    c.rows_out_post += rows_out;
  } else {
    ++c.batches_pre;
    c.rows_in_pre += rows_in;
    c.rows_out_pre += rows_out;
  }
  c.masks_applied += masks_applied;
  c.masks_skipped += masks_skipped;
  c.replica_unavailable += replica_unavailable;
  _replica_unavailable.fetch_add(replica_unavailable, std::memory_order_relaxed);
}

void dynamic_filter_query_stats::record_channel_passthrough(op::dynamic_filter_channel_id channel,
                                                            std::size_t batches,
                                                            std::int64_t rows)
{
  std::scoped_lock lock(_coverage_mutex);
  auto& c = _coverage[channel];
  c.passthrough_batches += batches;
  if (rows >= 0) { c.passthrough_rows += rows; }
}

void dynamic_filter_query_stats::sample_gpu_allocated(int device_id,
                                                      std::size_t total_allocated_bytes)
{
  auto const it = std::find_if(_gpu_spaces.begin(), _gpu_spaces.end(), [device_id](auto const& s) {
    return s.device_id == device_id;
  });
  if (it != _gpu_spaces.end()) { update_hwm(it->sampled_max, total_allocated_bytes); }
}

dynamic_filter_query_stats::counters_snapshot dynamic_filter_query_stats::snapshot_for_testing()
  const noexcept
{
  auto const no_mat =
    _no_mat_empty.load(std::memory_order_relaxed) +
    _no_mat_no_delivery.load(std::memory_order_relaxed) +
    _no_mat_mode.load(std::memory_order_relaxed) + _no_mat_policy.load(std::memory_order_relaxed) +
    _no_mat_source.load(std::memory_order_relaxed) + _no_mat_closed.load(std::memory_order_relaxed);
  auto const published = _published.load(std::memory_order_relaxed);
  auto const failed    = _failed.load(std::memory_order_relaxed);
  auto const cancelled = _cancelled.load(std::memory_order_relaxed);
  return {published + no_mat + failed + cancelled,
          published,
          no_mat,
          failed,
          cancelled,
          _feeder_queued.load(std::memory_order_relaxed),
          _feeder_dispatched.load(std::memory_order_relaxed),
          _feeder_prioritized.load(std::memory_order_relaxed),
          _feeder_running_hwm.load(std::memory_order_relaxed),
          _feeder_running.load(std::memory_order_relaxed),
          _build_batches_delivered.load(std::memory_order_relaxed),
          _pinned_builds_hwm.load(std::memory_order_relaxed),
          _pinned_builds.load(std::memory_order_relaxed),
          _hash_tables_hwm.load(std::memory_order_relaxed),
          _hash_tables.load(std::memory_order_relaxed),
          _replica_bytes_hwm.load(std::memory_order_relaxed),
          _replica_bytes.load(std::memory_order_relaxed),
          _replica_unavailable.load(std::memory_order_relaxed)};
}

}  // namespace sirius::telemetry
