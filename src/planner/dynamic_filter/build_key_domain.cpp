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

#include "planner/dynamic_filter/build_key_domain.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "planner/sirius_physical_plan_generator.hpp"
#include "scan_manager/sirius_scan_manager.hpp"

namespace sirius::planner {

namespace {

struct ordinal_origin {
  std::size_t child_index;
  std::size_t child_ordinal;
};

std::optional<ordinal_origin> pass_through_origin(duckdb::LogicalOperator const& op,
                                                  std::size_t output_ordinal) noexcept
{
  switch (op.type) {
    case duckdb::LogicalOperatorType::LOGICAL_PROJECTION: {
      auto const& projection = op.Cast<duckdb::LogicalProjection>();
      if (output_ordinal >= projection.expressions.size()) { return std::nullopt; }
      auto const& expression = *projection.expressions[output_ordinal];
      if (expression.GetExpressionClass() != duckdb::ExpressionClass::BOUND_REF) {
        return std::nullopt;
      }
      return ordinal_origin{
        0, static_cast<std::size_t>(expression.Cast<duckdb::BoundReferenceExpression>().index)};
    }
    case duckdb::LogicalOperatorType::LOGICAL_FILTER: {
      auto const& filter = op.Cast<duckdb::LogicalFilter>();
      if (!filter.HasProjectionMap()) { return ordinal_origin{0, output_ordinal}; }
      if (output_ordinal >= filter.projection_map.size()) { return std::nullopt; }
      return ordinal_origin{0, static_cast<std::size_t>(filter.projection_map[output_ordinal])};
    }
    case duckdb::LogicalOperatorType::LOGICAL_ORDER_BY: {
      auto const& order = op.Cast<duckdb::LogicalOrder>();
      if (!order.HasProjectionMap()) { return ordinal_origin{0, output_ordinal}; }
      if (output_ordinal >= order.projection_map.size()) { return std::nullopt; }
      return ordinal_origin{0, static_cast<std::size_t>(order.projection_map[output_ordinal])};
    }
    case duckdb::LogicalOperatorType::LOGICAL_LIMIT:
    case duckdb::LogicalOperatorType::LOGICAL_TOP_N:
    case duckdb::LogicalOperatorType::LOGICAL_DISTINCT: return ordinal_origin{0, output_ordinal};
    case duckdb::LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
      auto const& aggregate = op.Cast<duckdb::LogicalAggregate>();
      // Multiple grouping sets can repeat rows and overstate coverage.
      if (aggregate.grouping_sets.size() > 1) { return std::nullopt; }
      if (output_ordinal >= aggregate.groups.size()) { return std::nullopt; }
      auto const& group = *aggregate.groups[output_ordinal];
      if (group.GetExpressionClass() != duckdb::ExpressionClass::BOUND_REF) { return std::nullopt; }
      return ordinal_origin{
        0, static_cast<std::size_t>(group.Cast<duckdb::BoundReferenceExpression>().index)};
    }
    case duckdb::LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
      auto const& join = op.Cast<duckdb::LogicalComparisonJoin>();
      if (join.children.size() != 2) { return std::nullopt; }
      // ResolveTypes emits projected left then right blocks. Trace only a side the join cannot
      // duplicate; otherwise coverage could be overstated.
      auto const left_width  = join.left_projection_map.empty() ? join.children[0]->types.size()
                                                                : join.left_projection_map.size();
      auto const left_origin = [&join](std::size_t ordinal) {
        return ordinal_origin{0,
                              join.left_projection_map.empty()
                                ? ordinal
                                : static_cast<std::size_t>(join.left_projection_map[ordinal])};
      };
      switch (join.join_type) {
        case duckdb::JoinType::SEMI:
        case duckdb::JoinType::ANTI:
          if (output_ordinal >= left_width) { return std::nullopt; }
          return std::optional{left_origin(output_ordinal)};
        case duckdb::JoinType::RIGHT_SEMI:
        case duckdb::JoinType::RIGHT_ANTI: {
          auto const right_width = join.right_projection_map.empty()
                                     ? join.children[1]->types.size()
                                     : join.right_projection_map.size();
          if (output_ordinal >= right_width) { return std::nullopt; }
          return std::optional{ordinal_origin{
            1,
            join.right_projection_map.empty()
              ? output_ordinal
              : static_cast<std::size_t>(join.right_projection_map[output_ordinal])}};
        }
        case duckdb::JoinType::MARK:
        case duckdb::JoinType::SINGLE:
          if (output_ordinal >= left_width) { return std::nullopt; }
          return std::optional{left_origin(output_ordinal)};
        default:
          // The opposite side may multiply rows and overstate coverage.
          return std::nullopt;
      }
    }
    case duckdb::LogicalOperatorType::LOGICAL_DELIM_JOIN: return std::nullopt;
    default: return std::nullopt;
  }
}

// Table-in/out functions have no single base-table row domain.
bool admissible_base_scan(duckdb::LogicalGet const& get, std::size_t ordinal) noexcept
{
  if (!get.children.empty() || !get.projected_input.empty()) { return false; }
  auto const scan_width =
    get.projection_ids.empty() ? get.GetColumnIds().size() : get.projection_ids.size();
  return ordinal < scan_width;
}

bool is_parquet_scan(std::string const& function_name) noexcept
{
  return function_name == "read_parquet" || function_name == "parquet_scan" ||
         function_name == "sirius_read_parquet";
}

}  // namespace

namespace detail {

std::optional<resolved_scan_column> resolve_pass_through_scan_column(
  duckdb::LogicalOperator const& subtree, std::size_t output_ordinal) noexcept
{
  auto const* node = &subtree;
  auto ordinal     = output_ordinal;
  while (node->type != duckdb::LogicalOperatorType::LOGICAL_GET) {
    auto const step = pass_through_origin(*node, ordinal);
    if (!step || step->child_index >= node->children.size()) { return std::nullopt; }
    node    = node->children[step->child_index].get();
    ordinal = step->child_ordinal;
  }
  auto const& get = node->Cast<duckdb::LogicalGet>();
  if (!admissible_base_scan(get, ordinal)) { return std::nullopt; }
  return resolved_scan_column{.scan = &get, .scan_ordinal = ordinal};
}

duckdb::LogicalGet const* resolve_pass_through_scan(duckdb::LogicalOperator const& subtree,
                                                    std::size_t output_ordinal) noexcept
{
  auto const column = resolve_pass_through_scan_column(subtree, output_ordinal);
  return column ? column->scan : nullptr;
}

std::vector<resolved_scan_column> resolve_build_key_scan_columns(
  duckdb::LogicalComparisonJoin const& join)
{
  std::vector<resolved_scan_column> columns(join.conditions.size());
  if (join.children.size() != 2) { return columns; }
  for (std::size_t condition_index = 0; condition_index < join.conditions.size();
       ++condition_index) {
    auto const& build_side = *join.conditions[condition_index].right;
    if (build_side.GetExpressionClass() != duckdb::ExpressionClass::BOUND_REF) { continue; }
    auto const ordinal =
      static_cast<std::size_t>(build_side.Cast<duckdb::BoundReferenceExpression>().index);
    if (auto const column = resolve_pass_through_scan_column(*join.children[1], ordinal)) {
      columns[condition_index] = *column;
    }
  }
  return columns;
}

std::vector<duckdb::LogicalGet const*> resolve_build_key_scans(
  duckdb::LogicalComparisonJoin const& join)
{
  auto const columns = resolve_build_key_scan_columns(join);
  std::vector<duckdb::LogicalGet const*> scans(columns.size(), nullptr);
  for (std::size_t condition_index = 0; condition_index < columns.size(); ++condition_index) {
    scans[condition_index] = columns[condition_index].scan;
  }
  return scans;
}

}  // namespace detail

duckdb_base_table_evidence::duckdb_base_table_evidence(
  duckdb::ClientContext& context, scan_manager::sirius_scan_manager const* pinned_registry) noexcept
  : _context{&context}, _pinned_registry{pinned_registry}
{
}

std::optional<std::size_t> duckdb_base_table_evidence::operator()(
  duckdb::LogicalGet const& get) const noexcept
{
  try {
    if (get.function.name == "seq_scan") {
      if (!get.function.cardinality || !get.bind_data) { return std::nullopt; }
      auto const stats = get.function.cardinality(*_context, get.bind_data.get());
      if (!stats || !stats->has_max_cardinality) { return std::nullopt; }
      return static_cast<std::size_t>(stats->max_cardinality);
    }
    // Parquet cardinality callbacks report estimates (never `has_max_cardinality`); the pinned
    // entry's row count is the exact size of the very file set this scan reads.
    if (is_parquet_scan(get.function.name)) {
      auto const* entry = pinned_entry_for(get);
      if (entry != nullptr && entry->num_rows > 0) { return entry->num_rows; }
    }
    return std::nullopt;
  } catch (...) {
    // Optional evidence must not fail query planning.
    return std::nullopt;
  }
}

bool duckdb_base_table_evidence::operator()(duckdb::LogicalGet const& get,
                                            std::size_t scan_ordinal) const noexcept
{
  try {
    auto const* entry = pinned_entry_for(get);
    if (entry == nullptr || entry->declared_unique_columns.empty()) { return false; }
    // Scan-local output ordinal -> column_ids position -> table column -> name.
    auto const& column_ids      = get.GetColumnIds();
    std::size_t column_position = scan_ordinal;
    if (!get.projection_ids.empty()) {
      if (scan_ordinal >= get.projection_ids.size()) { return false; }
      column_position = static_cast<std::size_t>(get.projection_ids[scan_ordinal]);
    }
    if (column_position >= column_ids.size()) { return false; }
    auto const& column_index = column_ids[column_position];
    if (!column_index.HasPrimaryIndex() || column_index.IsRowIdColumn() ||
        column_index.IsVirtualColumn() || column_index.IsEmptyColumn()) {
      return false;
    }
    auto const primary = static_cast<std::size_t>(column_index.GetPrimaryIndex());
    if (primary >= get.names.size()) { return false; }
    return entry->is_declared_unique(get.names[primary]);
  } catch (...) {
    return false;
  }
}

scan_manager::pinned_entry const* duckdb_base_table_evidence::pinned_entry_for(
  duckdb::LogicalGet const& get) const noexcept
{
  if (_pinned_registry == nullptr) { return nullptr; }
  auto const hit =
    std::ranges::find(_pinned_memo, &get, &decltype(_pinned_memo)::value_type::first);
  if (hit != _pinned_memo.end()) { return hit->second; }

  scan_manager::pinned_entry const* entry = nullptr;
  try {
    if (get.function.name == "seq_scan") {
      auto const* bind = dynamic_cast<duckdb::TableScanBindData const*>(get.bind_data.get());
      if (bind != nullptr && bind->table.IsDuckTable()) {
        auto const& table = bind->table.Cast<duckdb::DuckTableEntry>();
        entry             = _pinned_registry->find_pinned_entry_for_duckdb_table(
          table.ParentCatalog().GetName(), table.ParentSchema().name, table.name);
      }
    } else if (is_parquet_scan(get.function.name)) {
      auto const files =
        resolve_parquet_scan_file_paths(get.function.name, get.bind_data.get(), get.parameters);
      if (!files.empty()) { entry = _pinned_registry->find_pinned_entry_for_parquet_files(files); }
    }
  } catch (...) {
    entry = nullptr;
  }
  _pinned_memo.emplace_back(&get, entry);
  return entry;
}

}  // namespace sirius::planner
