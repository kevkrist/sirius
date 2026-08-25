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

#include "planner/dynamic_filter/dynamic_filter_publication_planning.hpp"

#include "helper/type_conversions.hpp"
#include "log/logging.hpp"
#include "op/dynamic_filter/sirius_dynamic_filter.hpp"
#include "op/scan/sirius_physical_dynamic_filter.hpp"
#include "op/sirius_physical_table_scan.hpp"
#include "planner/dynamic_filter/dynamic_filter_key_admission.hpp"
#include "planner/dynamic_filter/dynamic_filter_target_discovery.hpp"
#include "sirius_config.hpp"
#include "sirius_context.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sirius::planner {

namespace {

/// One replica-space pair per GPU, preferring the NUMA-local HOST staging space and falling back
/// to the first HOST space; mirrors the resolution `plan_comparison_join` performs for hash joins.
template <typename GpuSpaces, typename HostSpaces>
std::vector<op::dynamic_filter_replica_space> collect_replica_spaces(
  duckdb::SiriusContext& sirius_context, GpuSpaces const& gpu_spaces, HostSpaces const& host_spaces)
{
  auto& memory_manager = sirius_context.get_memory_manager();
  auto const& topology = sirius_context.get_config().get_hw_topology();
  std::vector<op::dynamic_filter_replica_space> replica_spaces;
  for (auto const* gpu_space : gpu_spaces) {
    auto const gpu_id = gpu_space->get_device_id();
    auto const gpu_info =
      std::find_if(topology.gpus.begin(), topology.gpus.end(), [gpu_id](auto const& gpu) {
        return static_cast<int>(gpu.id) == gpu_id;
      });
    auto const numa_node = gpu_info == topology.gpus.end() ? -1 : gpu_info->numa_node;
    auto const local_host =
      std::find_if(host_spaces.begin(), host_spaces.end(), [numa_node](auto const* host) {
        return numa_node >= 0 && host->get_device_id() == numa_node;
      });
    auto const* host_space  = local_host == host_spaces.end() ? host_spaces.front() : *local_host;
    auto* mutable_gpu_space = memory_manager.get_memory_space(cucascade::memory::Tier::GPU, gpu_id);
    if (mutable_gpu_space == nullptr) {
      throw std::logic_error(
        "[dynamic_filter_publication_planning] Dynamic-filter GPU space disappeared during plan "
        "construction");
    }
    replica_spaces.emplace_back(*mutable_gpu_space, *host_space);
  }
  return replica_spaces;
}

}  // namespace

op::dynamic_filter_publish_plan plan_single_key_membership_publication(
  duckdb::SiriusContext& sirius_context,
  sirius::operator_params const& op_params,
  membership_publication_request request,
  duckdb::unique_ptr<op::sirius_physical_operator>& probe_subtree,
  std::string_view log_context)
{
  auto& memory_manager   = sirius_context.get_memory_manager();
  auto const gpu_spaces  = memory_manager.get_memory_spaces_for_tier(cucascade::memory::Tier::GPU);
  auto const host_spaces = memory_manager.get_memory_spaces_for_tier(cucascade::memory::Tier::HOST);

  duckdb::vector<sirius::join_condition> conditions;
  conditions.push_back(std::move(request.condition));
  auto admitted_keys = admit_dynamic_filter_keys(conditions,
                                                 {request.shape},
                                                 {request.build_key_domain_cardinality},
                                                 request.build_side_unique_column);

  if (gpu_spaces.empty() || host_spaces.empty()) {
    if (!admitted_keys.empty()) {
      SIRIUS_LOG_INFO(
        "[{}] Not wiring dynamic filter(s): a GPU and HOST memory space are required for "
        "device-local replicas.",
        log_context);
    }
    return {};
  }

  // Prefer scan binding; the key uses one route.
  std::vector<op::dynamic_filter_publish_plan::probe_target> targets;
  std::size_t scan_target_count = 0;
  bool const scan_bind_armed    = scan_route_join_type_admissible(request.join_type);
  descent_policy const policy{.descend_build_blocks = true};
  for (std::size_t key_index = 0; key_index < admitted_keys.size(); ++key_index) {
    auto const& key       = admitted_keys[key_index];
    auto const& condition = conditions[key.planner_condition_index];
    // Terminal ordinals are local to each terminal, not the probe entry schema.
    auto const terminals =
      trace_probe_key(*probe_subtree, static_cast<std::size_t>(key.probe_key_ordinal), policy);
    bool scan_bound = false;
    for (auto const& terminal : terminals) {
      if (!scan_bind_armed ||
          terminal.node->type != sirius::op::SiriusPhysicalOperatorType::TABLE_SCAN) {
        continue;
      }
      auto& scan = terminal.node->Cast<sirius::op::sirius_physical_table_scan>();
      if (terminal.ordinal >= scan.types.size()) {
        SIRIUS_LOG_WARN(
          "[{}] dynamic filter key {}: scan-route terminal at scan '{}' carries exit ordinal {} "
          "outside the scan's {} output columns; skipping this binding.",
          log_context,
          key_index,
          scan.function.name,
          terminal.ordinal,
          scan.types.size());
        continue;
      }
      // Reuse the scan channel so multiple producers share the consumer.
      if (!scan.sirius_dynamic_filters) {
        scan.sirius_dynamic_filters = std::make_shared<sirius::op::sirius_dynamic_filter_set>();
      }
      // One key: every scan terminal opens its own target.
      targets.push_back({.filter_set               = scan.sirius_dynamic_filters,
                         .route_class              = sirius::op::dynamic_filter_route_class::scan,
                         .accepts_zone_map_filters = true,
                         .key_bindings             = {{.admitted_key_index   = key_index,
                                                       .channel_push_ordinal = terminal.ordinal,
                                                       .probe_storage_type =
                                                         sirius::try_get_cudf_type(scan.types[terminal.ordinal])
                                                           .value_or(cudf::data_type{cudf::type_id::EMPTY})}}});
      ++scan_target_count;
      scan_bound = true;
    }
    if (scan_bound) { continue; }
    // Admission proves build-block safety; placement preserves the reported site ordinal.
    if (!direct_route_admissible(request.join_type,
                                 condition.comparison,
                                 key.key_shape,
                                 key.probe_storage_type,
                                 key.storage_type)) {
      continue;
    }
    std::vector<std::shared_ptr<sirius::op::sirius_dynamic_filter_set>> site_channels;
    auto placed = place_endpoint(
      std::move(probe_subtree),
      static_cast<std::size_t>(key.probe_key_ordinal),
      policy,
      [&site_channels, &op_params](sirius::op::sirius_physical_operator const& site)
        -> duckdb::unique_ptr<sirius::op::sirius_physical_operator> {
        auto channel  = std::make_shared<sirius::op::sirius_dynamic_filter_set>();
        auto endpoint = duckdb::make_uniq<sirius::op::scan::sirius_physical_dynamic_filter>(
          site.types,
          site.estimated_cardinality,
          channel,
          op_params.dynamic_filter_keep_threshold,
          sirius::op::scan::dynamic_filter_apply_mode::membership_masks_only);
        site_channels.push_back(std::move(channel));
        return endpoint;
      });
    probe_subtree = std::move(placed.subtree);
    if (site_channels.size() != placed.site_ordinals.size()) {
      SIRIUS_LOG_WARN(
        "[{}] dynamic filter key {}: direct-route walk placed {} endpoints but recorded {} site "
        "ordinals; skipping this key's bindings.",
        log_context,
        key_index,
        site_channels.size(),
        placed.site_ordinals.size());
      continue;
    }
    for (std::size_t site = 0; site < site_channels.size(); ++site) {
      targets.push_back({.filter_set               = std::move(site_channels[site]),
                         .route_class              = sirius::op::dynamic_filter_route_class::direct,
                         .accepts_zone_map_filters = false,
                         .key_bindings             = {{.admitted_key_index   = key_index,
                                                       .channel_push_ordinal = placed.site_ordinals[site],
                                                       .probe_storage_type   = key.probe_storage_type}}});
    }
  }
  if (targets.empty()) { return {}; }

  // Register after all keys bind so each declaration covers every planned push.
  for (auto const& target : targets) {
    std::vector<std::size_t> planned_columns;
    planned_columns.reserve(target.key_bindings.size());
    for (auto const& binding : target.key_bindings) {
      planned_columns.push_back(binding.channel_push_ordinal);
    }
    target.filter_set->register_producer(std::move(planned_columns));
  }

  SIRIUS_LOG_INFO(
    "[{}] Wired GROUP_JOIN with {} dynamic-filter probe target(s) ({} scan-bound, {} join-edge; "
    "build est {} rows).",
    log_context,
    targets.size(),
    scan_target_count,
    targets.size() - scan_target_count,
    request.build_estimated_rows);

  return op::dynamic_filter_publish_plan{
    std::move(admitted_keys),
    std::move(targets),
    collect_replica_spaces(sirius_context, gpu_spaces, host_spaces),
    {.emit_zone_map_filters     = op_params.enable_dynamic_zone_map_filter,
     .domain_coverage_threshold = op_params.dynamic_filter_domain_coverage_threshold,
     .inlist_max_l2_fraction    = op_params.dynamic_filter_inlist_max_l2_fraction}};
}

}  // namespace sirius::planner
