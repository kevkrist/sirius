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

#pragma once

// Host-side parquet row-group pruning by column-chunk min/max statistics.
//
// This replaces cuDF's GPU `filter_row_groups_with_stats`: instead of translating the DuckDB
// filter into a cuDF AST, lifting per-row-group min/max into GPU columns, and launching a
// `cudf::compute_column` kernel per file, we evaluate the original DuckDB `TableFilter`s against
// per-row-group statistics on the host using DuckDB's own `TableFilter::CheckStatistics`. The
// metadata/planning stage therefore needs no cuDF AST and no GPU work, and it prunes column types
// (e.g. FIXED_LEN_BYTE_ARRAY decimals) that the GPU stats filter could not handle.
//
// Correctness contract: a row group is dropped only when a filter is provably unsatisfiable over
// that row group's statistics (`FilterPropagateResult::FILTER_ALWAYS_FALSE`). Whenever an outcome
// cannot be determined — a filter type we don't model, a column without modern `min_value`/
// `max_value` statistics, a nested column, or a stat-byte encoding we don't decode — the row group
// is kept. The pruner can only ever match or under-prune relative to a full scan, never over-prune.

#include <cudf/io/parquet_schema.hpp>
#include <cudf/types.hpp>

#include <duckdb/common/types.hpp>
#include <duckdb/planner/table_filter.hpp>

#include <string>
#include <vector>

namespace sirius::op::scan {

/// One conjunct of the scan's pushed-down predicate, bound to a single column.
/// `column_name` is the parquet (D-space) column name used to resolve the leaf schema index per
/// file; `column_type` is the DuckDB logical type used to decode the stat bytes and build the
/// `BaseStatistics` the filter is checked against; `filter` is borrowed (it must outlive every
/// `prune_row_groups_by_stats` call — the provider keeps the owning `TableFilterSet` alive).
struct stats_prune_filter {
  std::string column_name;
  duckdb::LogicalType column_type;
  duckdb::TableFilter const* filter;
};

/// Return the subset of `input_row_groups` (ordinals into `metadata.row_groups`) that may contain
/// rows satisfying every `filter`. Pure host-side; no GPU, no cuDF AST. See the file header for the
/// conservative correctness contract. With `filters` empty, returns `input_row_groups` unchanged.
[[nodiscard]] std::vector<cudf::size_type> prune_row_groups_by_stats(
  cudf::io::parquet::FileMetaData const& metadata,
  std::vector<cudf::size_type> const& input_row_groups,
  std::vector<stats_prune_filter> const& filters);

}  // namespace sirius::op::scan
