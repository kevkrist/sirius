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

#include "planner/group_join_stream_admission.hpp"

#include "duckdb/common/multi_file/multi_file_states.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "log/logging.hpp"
#include "scan_manager/sirius_scan_manager.hpp"

#include <cudf/io/parquet_schema.hpp>

#include <cstring>
#include <limits>
#include <vector>

namespace sirius::planner {

namespace {

/// Facts about the argument's base scan column, from whichever provider resolved them.
struct column_facts {
  bool not_null    = false;
  bool have_bounds = false;
  int64_t unscaled_min{};
  int64_t unscaled_max{};
  /// Exact upper bound on the base scan's row count; 0 = unknown.
  uint64_t row_bound = 0;
};

/// Walk a linear GET/FILTER/PROJECTION chain from @p ordinal at @p node down to the base scan.
/// Projections must select the ordinal as a bare reference (a computed argument has no base
/// column). Returns the GET plus the ordinal in its output, or nullopt when unresolvable.
std::optional<std::pair<duckdb::LogicalGet const*, std::size_t>> resolve_base_scan_column(
  duckdb::LogicalOperator const& node, std::size_t ordinal)
{
  duckdb::LogicalOperator const* current = &node;
  while (true) {
    switch (current->type) {
      case duckdb::LogicalOperatorType::LOGICAL_GET: {
        auto const& get = current->Cast<duckdb::LogicalGet>();
        if (!get.children.empty() || !get.projected_input.empty()) { return std::nullopt; }
        return std::pair{&get, ordinal};
      }
      case duckdb::LogicalOperatorType::LOGICAL_FILTER: {
        auto const& filter = current->Cast<duckdb::LogicalFilter>();
        if (filter.HasProjectionMap()) {
          if (ordinal >= filter.projection_map.size()) { return std::nullopt; }
          ordinal = static_cast<std::size_t>(filter.projection_map[ordinal]);
        }
        break;
      }
      case duckdb::LogicalOperatorType::LOGICAL_PROJECTION: {
        if (ordinal >= current->expressions.size()) { return std::nullopt; }
        auto const& expr = *current->expressions[ordinal];
        if (expr.GetExpressionClass() != duckdb::ExpressionClass::BOUND_REF) {
          return std::nullopt;
        }
        ordinal = static_cast<std::size_t>(expr.Cast<duckdb::BoundReferenceExpression>().index);
        break;
      }
      default: return std::nullopt;
    }
    if (current->children.size() != 1) { return std::nullopt; }
    current = current->children[0].get();
  }
}

/// Map a GET output ordinal onto its position in `GetColumnIds()`; the base column index in
/// storage order is that entry's primary index.
std::optional<std::size_t> base_column_ids_position(duckdb::LogicalGet const& get,
                                                    std::size_t ordinal)
{
  auto const& column_ids = get.GetColumnIds();
  auto const scan_width =
    get.projection_ids.empty() ? column_ids.size() : get.projection_ids.size();
  if (ordinal >= scan_width) { return std::nullopt; }
  auto const column_pos = get.projection_ids.empty() ? ordinal : get.projection_ids[ordinal];
  if (column_pos >= column_ids.size()) { return std::nullopt; }
  auto const primary = column_ids[column_pos].GetPrimaryIndex();
  // Virtual columns (rowid and friends) carry out-of-range identifiers.
  if (primary >= get.names.size()) { return std::nullopt; }
  return static_cast<std::size_t>(column_pos);
}

/// The unscaled integer payload of a DuckDB statistics Value (DECIMALs store their scaled
/// integer internally).
std::optional<int64_t> unscaled_value(duckdb::Value const& value)
{
  if (value.IsNull()) { return std::nullopt; }
  switch (value.type().InternalType()) {
    case duckdb::PhysicalType::INT8: return value.GetValueUnsafe<int8_t>();
    case duckdb::PhysicalType::INT16: return value.GetValueUnsafe<int16_t>();
    case duckdb::PhysicalType::INT32: return value.GetValueUnsafe<int32_t>();
    case duckdb::PhysicalType::INT64: return value.GetValueUnsafe<int64_t>();
    default: return std::nullopt;
  }
}

/// Facts from DuckDB's own interfaces: column statistics plus the exactness-flagged `seq_scan`
/// cardinality. Best-effort; missing pieces stay unresolved.
void gather_duckdb_facts(duckdb::ClientContext& context,
                         duckdb::LogicalGet const& get,
                         std::size_t column_ids_position,
                         column_facts& facts)
{
  auto const& column_index = get.GetColumnIds()[column_ids_position];
  if ((get.function.statistics != nullptr || get.function.statistics_extended != nullptr) &&
      get.bind_data != nullptr) {
    try {
      // The two-callback dispatch mirrors the statistics propagator (`propagate_get.cpp`).
      duckdb::unique_ptr<duckdb::BaseStatistics> stats;
      if (get.function.statistics_extended != nullptr) {
        duckdb::TableFunctionGetStatisticsInput input(get.bind_data.get(), column_index);
        stats = get.function.statistics_extended(context, input);
      } else {
        stats =
          get.function.statistics(context, get.bind_data.get(), column_index.GetPrimaryIndex());
      }
      if (stats) {
        facts.not_null = !stats->CanHaveNull();
        if (duckdb::NumericStats::HasMinMax(*stats)) {
          auto const min = unscaled_value(duckdb::NumericStats::Min(*stats));
          auto const max = unscaled_value(duckdb::NumericStats::Max(*stats));
          if (min && max) {
            facts.have_bounds  = true;
            facts.unscaled_min = *min;
            facts.unscaled_max = *max;
          }
        }
      }
    } catch (...) {
      // Optional evidence must not fail query planning.
    }
  }
  // Other table functions report estimates, which are never admissible as bounds.
  if (get.function.name != "seq_scan" || get.function.cardinality == nullptr ||
      get.bind_data == nullptr) {
    return;
  }
  try {
    auto const node_stats = get.function.cardinality(context, get.bind_data.get());
    if (node_stats && node_stats->has_max_cardinality) {
      facts.row_bound = static_cast<uint64_t>(node_stats->max_cardinality);
    }
  } catch (...) {
  }
}

/// Facts from the parquet footers of a multi-file scan, which DuckDB's binding surfaces no
/// statistics for. Every file must agree: schema-level REQUIRED repetition proves NOT NULL,
/// column-chunk statistics on an INT32/INT64 physical column bound the unscaled values, and the
/// summed `num_rows` is the exact row bound. Any missing or undecodable piece leaves the
/// corresponding fact unresolved.
void gather_parquet_facts(scan_manager::sirius_scan_manager& scan_manager,
                          duckdb::LogicalGet const& get,
                          std::size_t column_ids_position,
                          column_facts& facts)
{
  namespace pq = cudf::io::parquet;

  auto const* multi_file_bind = dynamic_cast<duckdb::MultiFileBindData const*>(get.bind_data.get());
  if (multi_file_bind == nullptr || !multi_file_bind->file_list ||
      multi_file_bind->file_list->IsEmpty()) {
    return;
  }
  auto const& column_name = get.names[get.GetColumnIds()[column_ids_position].GetPrimaryIndex()];

  auto const decode_stat = [](std::optional<std::vector<uint8_t>> const& bytes,
                              pq::Type type) -> std::optional<int64_t> {
    if (!bytes) { return std::nullopt; }
    if (type == pq::Type::INT32 && bytes->size() == sizeof(int32_t)) {
      int32_t value{};
      std::memcpy(&value, bytes->data(), sizeof(value));
      return value;
    }
    if (type == pq::Type::INT64 && bytes->size() == sizeof(int64_t)) {
      int64_t value{};
      std::memcpy(&value, bytes->data(), sizeof(value));
      return value;
    }
    return std::nullopt;
  };

  // Two independent NOT-NULL evidence channels, both hard metadata: schema-level REQUIRED
  // repetition, and a zero null_count on every column chunk.
  bool required_all     = true;
  bool null_counts_zero = true;
  bool have_bounds      = true;
  int64_t global_min{std::numeric_limits<int64_t>::max()};
  int64_t global_max{std::numeric_limits<int64_t>::min()};
  uint64_t total_rows = 0;
  bool any_values     = false;

  try {
    for (auto const& file : multi_file_bind->file_list->GetAllFiles()) {
      auto const metadata = scan_manager.describe_parquet_metadata(file.path);
      if (metadata == nullptr || metadata->num_rows < 0) {
        SIRIUS_LOG_INFO("[group_join_stream_admission] no parquet footer metadata for {}",
                        file.path);
        return;
      }
      if (total_rows >
          std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(metadata->num_rows)) {
        return;
      }
      total_rows += static_cast<uint64_t>(metadata->num_rows);

      // Locate the column's leaf schema element by name; ambiguity fails closed.
      pq::SchemaElement const* leaf = nullptr;
      for (auto const& element : metadata->schema) {
        if (element.num_children != 0 || element.name != column_name) { continue; }
        if (leaf != nullptr) { return; }
        leaf = &element;
      }
      if (leaf == nullptr) {
        SIRIUS_LOG_DEBUG(
          "[group_join_stream_admission] column {} has no unique leaf schema element in {}",
          column_name,
          file.path);
        return;
      }
      required_all &= leaf->repetition_type == pq::FieldRepetitionType::REQUIRED;
      if (leaf->type != pq::Type::INT32 && leaf->type != pq::Type::INT64) { have_bounds = false; }

      for (auto const& row_group : metadata->row_groups) {
        pq::ColumnChunk const* chunk = nullptr;
        for (auto const& candidate : row_group.columns) {
          auto const& path = candidate.meta_data.path_in_schema;
          if (path.size() == 1 && path[0] == column_name) {
            chunk = &candidate;
            break;
          }
        }
        if (chunk == nullptr) { return; }
        if (row_group.num_rows == 0) { continue; }
        auto const& stats = chunk->meta_data.statistics;
        null_counts_zero &= stats.null_count.has_value() && *stats.null_count == 0;
        if (!have_bounds) { continue; }
        // Prefer the ColumnOrder min_value/max_value; the deprecated signed-order min/max
        // coincide with it for two's-complement INT32/INT64.
        auto const chunk_min =
          decode_stat(stats.min_value ? stats.min_value : stats.min, chunk->meta_data.type);
        auto const chunk_max =
          decode_stat(stats.max_value ? stats.max_value : stats.max, chunk->meta_data.type);
        if (!chunk_min || !chunk_max) {
          have_bounds = false;
          continue;
        }
        any_values = true;
        global_min = std::min(global_min, *chunk_min);
        global_max = std::max(global_max, *chunk_max);
      }
    }
  } catch (std::exception const& error) {
    // A failed footer read is inconclusive, never a planner error.
    SIRIUS_LOG_INFO("[group_join_stream_admission] parquet footer facts unavailable for {}: {}",
                    column_name,
                    error.what());
    return;
  } catch (...) {
    SIRIUS_LOG_INFO("[group_join_stream_admission] parquet footer facts unavailable for {}",
                    column_name);
    return;
  }

  facts.row_bound = total_rows;
  facts.not_null  = required_all || null_counts_zero;
  if (have_bounds && any_values) {
    facts.have_bounds  = true;
    facts.unscaled_min = global_min;
    facts.unscaled_max = global_max;
  } else if (have_bounds && total_rows == 0) {
    // A provably empty scan bounds every value trivially.
    facts.have_bounds  = true;
    facts.unscaled_min = 0;
    facts.unscaled_max = 0;
  }
}

/// True when rows x max(|min|, |max|) provably fits in int64 (the streamed SUM/AVG bound).
[[nodiscard]] bool accumulation_bound_safe(uint64_t rows, int64_t min_value, int64_t max_value)
{
  auto const unsigned_abs = [](int64_t v) {
    return v < 0 ? ~static_cast<uint64_t>(v) + 1 : static_cast<uint64_t>(v);
  };
  auto const magnitude = std::max(unsigned_abs(min_value), unsigned_abs(max_value));
  if (magnitude == 0 || rows == 0) { return true; }
  auto const bigint_max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  return rows <= bigint_max / magnitude;
}

[[nodiscard]] group_join_stream_admission declined(std::string reason)
{
  group_join_stream_admission result;
  result.reason = std::move(reason);
  return result;
}

}  // namespace

group_join_stream_admission admit_group_join_inner_stream(
  duckdb::ClientContext& context,
  scan_manager::sirius_scan_manager* scan_manager,
  duckdb::LogicalOperator const& counted_child,
  std::optional<std::size_t> argument_ordinal,
  sirius::op::groupjoin::agg_op op)
{
  using sirius::op::groupjoin::agg_op;

  // COUNT(*) reads no argument and its BIGINT product is validated at emit from runtime totals.
  if (op == agg_op::COUNT_STAR) {
    group_join_stream_admission result;
    result.admitted = true;
    return result;
  }
  if (!argument_ordinal) { return declined("argument-taking op without an argument ordinal"); }

  auto const resolved = resolve_base_scan_column(counted_child, *argument_ordinal);
  if (!resolved) {
    return declined("argument does not resolve to a base scan column (computed or opaque)");
  }
  auto const& [get, scan_ordinal] = *resolved;
  auto const column_pos           = base_column_ids_position(*get, scan_ordinal);
  if (!column_pos) { return declined("argument maps to no base table column"); }

  column_facts facts;
  gather_duckdb_facts(context, *get, *column_pos, facts);
  bool const is_parquet =
    get->function.name == "parquet_scan" || get->function.name == "read_parquet";
  if (is_parquet && scan_manager != nullptr &&
      (!facts.not_null || !facts.have_bounds || facts.row_bound == 0)) {
    gather_parquet_facts(*scan_manager, *get, *column_pos, facts);
  }
  SIRIUS_LOG_INFO(
    "[group_join_stream_admission] facts for {}.{}: not_null={} bounds={} [{}, {}] row_bound={}",
    get->function.name,
    get->names[get->GetColumnIds()[*column_pos].GetPrimaryIndex()],
    facts.not_null,
    facts.have_bounds,
    facts.unscaled_min,
    facts.unscaled_max,
    facts.row_bound);

  if (!facts.not_null) {
    return declined(
      "argument NOT-NULL evidence inconclusive (no catalog constraint or "
      "statistics)");
  }
  if (op == agg_op::SUM || op == agg_op::AVG) {
    if (facts.row_bound == 0) {
      return declined("no exact base-scan row bound for the SUM/AVG accumulation proof");
    }
    if (!facts.have_bounds) {
      return declined("no argument value bounds for the SUM/AVG accumulation proof");
    }
    if (!accumulation_bound_safe(facts.row_bound, facts.unscaled_min, facts.unscaled_max)) {
      return declined(
        "SUM/AVG int64 accumulation bound not provable (rows x magnitude exceeds "
        "BIGINT)");
    }
  }

  group_join_stream_admission result;
  result.admitted = true;
  if (op == agg_op::SUM || op == agg_op::AVG) { result.counted_row_bound = facts.row_bound; }
  return result;
}

}  // namespace sirius::planner
