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

#pragma once

#include "op/sirius_physical_group_join.hpp"

#include <duckdb/main/client_context.hpp>
#include <duckdb/planner/logical_operator.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace sirius::scan_manager {
class sirius_scan_manager;
}

namespace sirius::planner {

/**
 * @file
 * @brief Plan-time admission proofs of the GROUP_JOIN streamed (BUILD_STREAM) schedule.
 *
 * A streamed INNER groupjoin cannot inspect counted batches before allocating its dense state, so
 * the two facts the one-shot schedule reads off harvested batches move to plan time: the
 * argument column carries no NULLs (every argument-taking op), and the int64 SUM/AVG accumulation
 * cannot overflow (`counted rows x max(|min|, |max|)` on the argument's unscaled representation).
 * Both proofs consume hard bounds only, never cardinality estimates: the counted side is a linear
 * GET/FILTER/PROJECTION chain (row-non-increasing), so the base scan's exact row count bounds the
 * stream, and column min/max metadata bounds every value. Facts resolve through the chain to the
 * base scan column and come from DuckDB column statistics plus the exact `seq_scan` cardinality
 * for native tables, or -- because DuckDB's multi-file parquet binding surfaces neither
 * statistics nor an exactness-flagged cardinality -- from the parquet footers themselves through
 * `sirius_scan_manager::describe_parquet_metadata` (exact `num_rows`, schema-level REQUIRED
 * repetition, INT32/INT64 column-chunk statistics). Anything unresolvable is inconclusive and the
 * caller keeps the one-shot admission, whose counted-byte gate then declines the shape.
 */

/// Outcome of one streamed-admission evaluation.
struct group_join_stream_admission {
  bool admitted = false;
  /// Decline reason for the fusion log; empty when admitted.
  std::string reason;
  /// Row bound consumed by a SUM/AVG overflow proof (belt-checked at runtime); 0 otherwise.
  uint64_t counted_row_bound = 0;
};

/**
 * @brief Prove (or decline) streamed admission of an over-gate GROUP_JOIN INNER shape.
 *
 * @param context Client context of the query being planned.
 * @param scan_manager Source of parquet footer facts; nullptr makes parquet-backed proofs
 * inconclusive.
 * @param counted_child The counted-side logical subtree (a linear GET/FILTER/PROJECTION chain,
 * already screened by rung P1).
 * @param argument_ordinal The aggregate argument's ordinal in the counted child's output;
 * std::nullopt for COUNT(*).
 * @param op The slot's aggregate operation.
 */
group_join_stream_admission admit_group_join_inner_stream(
  duckdb::ClientContext& context,
  scan_manager::sirius_scan_manager* scan_manager,
  duckdb::LogicalOperator const& counted_child,
  std::optional<std::size_t> argument_ordinal,
  sirius::op::groupjoin::agg_op op);

}  // namespace sirius::planner
