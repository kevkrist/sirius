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

// "Date DIPs" (data-induced / derived date predicates).
//
// When a query filters a dimension date column and FK-joins an un-filtered fact table that is
// clustered on a correlated date column, this pass derives a date-range predicate on the fact date
// column from the dimension filter and the measured correlation lag, then injects it onto the
// fact-table scan. The native row-group pruner (mark_row_groups_pruned_by_filter_stats) then skips
// fact row groups before decode.
//
// The derived predicate is wrapped in a duckdb::OptionalFilter, which is
//  (a) honored by the row-group pruner via CheckStatistics, yet
//  (b) skipped row-wise by convert_table_filters_to_expression (src/op/scan/scan_utils.cpp)
// so it can only PRUNE row groups, never drop a decoded row. The derived window is an
// over-approximation: given a sound lag, every joining fact row satisfies it, so no qualifying row
// group is ever pruned.
//
// The lag is supplied by `apply_date_dips`' caller as a `date_correlation`. It is measured by `CALL
// sirius_measure_date_correlation(...)`, which the optimizer hook reads from the cache; with no
// measurement, no DIP fires. The lag is a cross-column property DuckDB's single-column statistics
// cannot supply, which is why it is measured explicitly. The feature is gated off by default behind
// the `enable_date_dips` setting.

#include "transparent/date_correlation.hpp"

#include <duckdb/common/types/date.hpp>
#include <duckdb/planner/logical_operator.hpp>
#include <duckdb/planner/table_filter.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace duckdb {
class TableCatalogEntry;
}

namespace sirius::transparent {

/// A closed date interval [lo, hi]; either bound may be absent (one-sided).
struct derived_date_window {
  std::optional<duckdb::date_t> lo;
  std::optional<duckdb::date_t> hi;
  [[nodiscard]] bool empty() const { return !lo.has_value() && !hi.has_value(); }
};

//===----------------------------------------------------------------------===//
// Pure, unit-testable core (no plan / no catalog required).
//===----------------------------------------------------------------------===//

/// @brief Read whatever date bounds a pushed-down filter encodes. Recurses through
/// CONJUNCTION_AND and OPTIONAL_FILTER, collecting CONSTANT_COMPARISON bounds on
/// a DATE column. Returns an empty window if nothing usable.
derived_date_window extract_date_window(const duckdb::TableFilter& filter);

/// @brief Apply a correlation's lag to a dimension-side window: dim [D1, D2] -> fact
/// [D1 + lag_lo, D2 + lag_hi]. With a two-sided window and a lag histogram (`correlation.buckets`),
/// `lag_lo`/`lag_hi` are taken over only the buckets the window overlaps; otherwise the global lag
/// is used. Absent input bounds stay absent. The result is always a single interval (same
/// injection).
derived_date_window derive_forward_window(const derived_date_window& dim_window,
                                          const date_correlation& correlation);

/// @brief Build a prune-only TableFilter for [window.lo, window.hi]: an OptionalFilter wrapping a
/// ConjunctionAndFilter of two ConstantFilters (or a single ConstantFilter for a one-sided window).
/// The OptionalFilter wrapper makes it a row-level no-op while still pruning row groups via
/// CheckStatistics. Returns nullptr if the window is empty or degenerate (lo > hi).
duckdb::unique_ptr<duckdb::TableFilter> make_prune_only_date_filter(
  const derived_date_window& window);

//===----------------------------------------------------------------------===//
// The pass.
//===----------------------------------------------------------------------===//

/// A database's write-activity token: (checkpoint iteration, WAL size). Both move only on
/// committed writes — reads never touch the WAL (gated by ChangesMade()) — and the
/// iteration is durable + monotone across checkpoints. Equal token at two times means no
/// committed write in between.
struct db_write_token {
  std::uint64_t checkpoint_iteration = 0;
  std::uint64_t wal_size             = 0;
};

/// Current write-activity token of @p table's database. Returns nullopt when the database
/// is not file-backed (in-memory, no SingleFileBlockManager) — the caller then skips the
/// DIP (fail-safe). Used to detect a stale correlation (data changed since measurement).
std::optional<db_write_token> read_db_write_token(duckdb::TableCatalogEntry& table);

/// Inject a prune-only date predicate onto every fact-table scan that a measured
/// @p correlation connects to a date-filtered dimension scan, mutating @p plan in
/// place. Returns the number of predicates injected.
///
/// A correlation fires on a (dim GET, fact GET) pair only when ALL hold:
///   - the dim GET carries a pushed-down filter on its date column (the seed),
///   - the fact GET has NO filter on its date column (so the injected OptionalFilter
///     stays the top-level filter, hence a row-level no-op),
///   - they are linked by an INNER or SEMI equi-join on (dim_key_col, fact_key_col).
///     OUTER/ANTI joins are excluded: an unmatched fact row could survive the join,
///     so pruning its row group could drop a result row.
std::size_t apply_date_dips(duckdb::LogicalOperator& plan,
                            const std::vector<date_correlation>& correlations);

}  // namespace sirius::transparent
