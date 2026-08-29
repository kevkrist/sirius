/*
 * Copyright 2025, Sirius Contributors.
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

#include "op/sirius_physical_grouped_aggregate.hpp"

#include "config.hpp"
#include "data/data_batch_utils.hpp"
#include "log/logging.hpp"
#include "op/aggregate/aggregate_op_util.hpp"
#include "op/aggregate/gpu_aggregate_impl.hpp"
#include "op/aggregate/tiny_domain_grouped_aggregate_impl.hpp"
#include "sirius/exception.hpp"

#include <nvtx3/nvtx3.hpp>

namespace sirius {
namespace op {

sirius_physical_grouped_aggregate::sirius_physical_grouped_aggregate(
  duckdb::vector<sirius::logical_type> types,
  duckdb::vector<std::unique_ptr<sirius::ast::node>> expressions,
  duckdb::vector<std::unique_ptr<sirius::ast::node>> groups_p,
  std::size_t estimated_cardinality,
  bool enable_tiny_domain_strategy)
  : sirius_physical_grouped_aggregate(std::move(types),
                                      std::move(expressions),
                                      std::move(groups_p),
                                      {},
                                      {},
                                      estimated_cardinality,
                                      duckdb::TupleDataValidityType::CAN_HAVE_NULL_VALUES,
                                      duckdb::TupleDataValidityType::CAN_HAVE_NULL_VALUES,
                                      enable_tiny_domain_strategy)
{
}

// expressions is the list of aggregates to be computed. Each aggregates has a bound_ref expression
// to a column groups_p is the list of group by columns. Each group by column is a bound_ref
// expression to a column grouping_sets_p is the list of grouping set. Each grouping set is a set of
// indexes to the group by columns. Seems like DuckDB group the groupby columns into several sets
// and for every grouping set there is one radix_table grouping_functions_p is a list of indexes to
// the groupby expressions (groups_p) for each grouping_sets. The first level of the vector is the
// grouping set and the second level is the indexes to the groupby expression for that set.
sirius_physical_grouped_aggregate::sirius_physical_grouped_aggregate(
  duckdb::vector<sirius::logical_type> types,
  duckdb::vector<std::unique_ptr<sirius::ast::node>> expressions,
  duckdb::vector<std::unique_ptr<sirius::ast::node>> groups_p,
  duckdb::vector<duckdb::GroupingSet> grouping_sets_p,
  duckdb::vector<duckdb::unsafe_vector<std::size_t>> grouping_functions_p,
  std::size_t estimated_cardinality,
  duckdb::TupleDataValidityType /*group_validity*/,
  duckdb::TupleDataValidityType /*distinct_validity*/,
  bool enable_tiny_domain_strategy)
  : sirius_physical_operator(
      SiriusPhysicalOperatorType::HASH_GROUP_BY, std::move(types), estimated_cardinality),
    grouping_sets(std::move(grouping_sets_p)),
    _enable_tiny_domain_strategy(enable_tiny_domain_strategy)
{
  auto cudf_defs                    = convert_duckdb_aggregates_to_cudf(groups_p, expressions);
  group_idx                         = std::move(cudf_defs.group_idx);
  cudf_aggregates                   = std::move(cudf_defs.cudf_aggregates);
  cudf_aggregate_idx                = std::move(cudf_defs.cudf_aggregate_idx);
  cudf_aggregate_struct_col_indices = std::move(cudf_defs.cudf_aggregate_struct_col_indices);
  aggregate_slots                   = std::move(cudf_defs.aggregate_slots);
  has_avg                           = cudf_defs.has_avg;
  has_count_distinct                = cudf_defs.has_count_distinct;
}

void sirius_physical_grouped_aggregate::set_pipeline(
  duckdb::shared_ptr<pipeline::sirius_pipeline> pipeline)
{
  for (auto const& stage : _tiny_domain_projection_fallback_stages) {
    stage->set_pipeline(pipeline);
  }
  sirius_physical_operator::set_pipeline(std::move(pipeline));
}

void sirius_physical_grouped_aggregate::install_tiny_domain_projection_fusion(
  tiny_domain_q1_projection_plan plan,
  std::vector<duckdb::unique_ptr<sirius_physical_operator>> fallback_stages)
{
  if (!_enable_tiny_domain_strategy) {
    throw sirius::invalid_input_exception(
      "tiny-domain projection fusion requires the tiny-domain aggregate strategy");
  }
  if (_tiny_domain_projection_plan || !_tiny_domain_projection_fallback_stages.empty()) {
    throw sirius::invalid_input_exception("tiny-domain projection fusion is already installed");
  }
  if (fallback_stages.empty()) {
    throw sirius::invalid_input_exception(
      "tiny-domain projection fusion requires at least one detached fallback stage");
  }
  for (auto const& stage : fallback_stages) {
    if (!stage || stage->type != SiriusPhysicalOperatorType::PROJECTION ||
        !stage->children.empty()) {
      throw sirius::invalid_input_exception(
        "tiny-domain projection fusion fallback stages must be detached PROJECTION operators");
    }
  }

  _tiny_domain_projection_plan.emplace(std::move(plan));
  _tiny_domain_projection_fallback_stages = std::move(fallback_stages);
}

duckdb::vector<sirius::logical_type>
sirius_physical_grouped_aggregate::get_count_distinct_local_output_types() const
{
  auto const aggregate_offset = group_idx.size();
  if (!has_count_distinct || has_avg || types.size() != aggregate_offset + aggregate_slots.size()) {
    throw std::runtime_error(
      "COUNT(DISTINCT) local schema requires a non-AVG one-slot-per-aggregate layout");
  }

  auto local_types = types;
  for (size_t slot_idx = 0; slot_idx < aggregate_slots.size(); ++slot_idx) {
    if (aggregate_slots[slot_idx].is_count_distinct) {
      local_types[aggregate_offset + slot_idx] = sirius::logical_type::make(sirius::type_id::LIST);
    }
  }
  return local_types;
}

std::unique_ptr<operator_data> sirius_physical_grouped_aggregate::execute(
  const operator_data& input_data, rmm::cuda_stream_view stream)
{
  nvtx3::scoped_range nvtx_range{"sirius_physical_grouped_aggregate::execute"};
  if (!_tiny_domain_projection_plan) { return execute_projected(input_data, stream); }

  auto& input        = dynamic_cast<const pipelineable_operator_data&>(input_data);
  auto input_batches = input.get_read_only_batches();
  std::vector<std::shared_ptr<::cucascade::data_batch>> results;
  results.reserve(input_batches.size());

  for (auto const& input_batch : input_batches) {
    auto* space = input_batch.get_memory_space();
    if (!space) { continue; }

    auto attempt = try_tiny_domain_q1_projection_aggregate(get_cudf_table_view(input_batch),
                                                           *_tiny_domain_projection_plan,
                                                           cudf_aggregates,
                                                           stream,
                                                           space->get_default_allocator());
    if (attempt) {
      auto const activation =
        _tiny_domain_projection_activations.fetch_add(1, std::memory_order_relaxed) + 1;
      SIRIUS_LOG_DEBUG(
        "[tiny_domain_q1_projection_aggregate] activated: groups={} states={} activation={}",
        attempt.num_groups,
        cudf_aggregates.size(),
        activation);
      results.push_back(
        make_data_batch(std::move(attempt.table), *space, stream, batch_telemetry()));
      continue;
    }

    auto const fallback =
      _tiny_domain_projection_fallbacks.fetch_add(1, std::memory_order_relaxed) + 1;
    SIRIUS_LOG_DEBUG("[tiny_domain_q1_projection_aggregate] fallback: reason='{}' fallback={}",
                     attempt.fallback_reason,
                     fallback);

    std::unique_ptr<operator_data> fallback_data = std::make_unique<pipelineable_operator_data>(
      std::vector<::cucascade::read_only_data_batch>{input_batch});
    for (auto const& stage : _tiny_domain_projection_fallback_stages) {
      fallback_data = stage->execute(*fallback_data, stream);
    }

    auto aggregate_data = execute_projected(*fallback_data, stream);
    auto const& aggregate_batches =
      dynamic_cast<const pipelineable_operator_data&>(*aggregate_data).get_data_batches();
    results.insert(results.end(), aggregate_batches.begin(), aggregate_batches.end());
  }

  return std::make_unique<pipelineable_operator_data>(std::move(results));
}

std::unique_ptr<operator_data> sirius_physical_grouped_aggregate::execute_projected(
  const operator_data& input_data, rmm::cuda_stream_view stream)
{
  auto& input               = dynamic_cast<const pipelineable_operator_data&>(input_data);
  const auto& input_batches = input.get_read_only_batches();
  std::vector<std::shared_ptr<::cucascade::data_batch>> results;
  for (auto const& input_batch : input_batches) {
    auto* space = input_batch.get_memory_space();
    if (!space) { continue; }
    if (_enable_tiny_domain_strategy) {
      auto attempt = try_tiny_domain_grouped_aggregate(get_cudf_table_view(input_batch),
                                                       group_idx,
                                                       cudf_aggregates,
                                                       cudf_aggregate_idx,
                                                       stream,
                                                       space->get_default_allocator());
      if (attempt) {
        auto const activation =
          _tiny_domain_activations.fetch_add(1, std::memory_order_relaxed) + 1;
        SIRIUS_LOG_DEBUG(
          "[tiny_domain_grouped_aggregate] activated: groups={} states={} strategy={} "
          "preflight={} register_fallback={} activation={}",
          attempt.num_groups,
          cudf_aggregates.size(),
          attempt.used_register_private ? "q1_register_private" : "wide",
          attempt.used_sampled_preflight ? "sampled" : "full",
          attempt.register_private_attempted && !attempt.used_register_private,
          activation);
        results.push_back(
          make_data_batch(std::move(attempt.table), *space, stream, batch_telemetry()));
        continue;
      }
      auto const fallback = _tiny_domain_fallbacks.fetch_add(1, std::memory_order_relaxed) + 1;
      SIRIUS_LOG_DEBUG("[tiny_domain_grouped_aggregate] fallback: reason='{}' fallback={}",
                       attempt.fallback_reason,
                       fallback);
    }
    auto result = gpu_aggregate_impl::local_grouped_aggregate(input_batch,
                                                              group_idx,
                                                              cudf_aggregates,
                                                              cudf_aggregate_idx,
                                                              cudf_aggregate_struct_col_indices,
                                                              stream,
                                                              *space,
                                                              batch_telemetry());
    results.push_back(std::move(result));
  }
  return std::make_unique<pipelineable_operator_data>(std::move(results));
}
}  // namespace op
}  // namespace sirius
