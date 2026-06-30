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

#include <cstdint>
#include <string>
#include <vector>

namespace sirius::transparent {

/// One bucket of a lag histogram: over dimension dates in [dim_lo_day, dim_hi_day]
/// (inclusive, expressed as date_t day numbers), every joining fact row satisfies
/// `fact_date - dim_date in [lag_lo_days, lag_hi_days]`.
struct lag_bucket {
  std::int32_t dim_lo_day;
  std::int32_t dim_hi_day;
  std::int32_t lag_lo_days;
  std::int32_t lag_hi_days;
};

/// A measured correlation between a dimension date column and a fact date column.
///
/// Across the FK equi-join `fact.fact_key_col = dim.dim_key_col`, every joining fact
/// row satisfies `fact_date_col - dim_date_col in [lag_lo_days, lag_hi_days]` (days).
///
/// Measured by `CALL sirius_measure_date_correlation(...)`, cached on SiriusContext,
/// and consumed by the date-DIP optimizer pass (`apply_date_dips`) to derive a
/// prune-only predicate on `fact_date_col` from a filter on `dim_date_col`. One
/// direction only: a dimension-side date filter yields a fact-side predicate.
///
/// `lag_lo_days`/`lag_hi_days` are the GLOBAL lag (the bound over all joining rows). When
/// `buckets` is non-empty it refines that global bound into a per-dim-date lag histogram;
/// an empty `buckets` is the 1-bucket case (use the global lag), which is optimal for a
/// stationary correlation and what a plain `CALL` (no bucketing) produces.
///
/// A dependency-free value type so both the runtime context (which caches it) and the
/// optimizer pass (which consumes it) share one definition without a layering cycle.
struct date_correlation {
  std::string dim_table;
  std::string dim_date_col;
  std::string dim_key_col;
  std::string fact_table;
  std::string fact_date_col;
  std::string fact_key_col;
  std::int32_t lag_lo_days = 0;
  std::int32_t lag_hi_days = 0;
  std::vector<lag_bucket> buckets;

  // Staleness gate (see date-dip-staleness-detection). Write-activity token of the owning
  // database captured at measurement: (checkpoint iteration, WAL size). The date-DIP pass
  // injects only if the table's database still reports this exact token — i.e. nothing was
  // committed since measurement. Left 0/0 when the token could not be captured (e.g. an
  // in-memory DB); the gate then fails closed and no DIP is injected.
  std::uint64_t snapshot_checkpoint_iteration = 0;
  std::uint64_t snapshot_wal_size             = 0;
};

}  // namespace sirius::transparent
