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

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace duckdb {
class ClientContext;
class LogicalComparisonJoin;
class LogicalGet;
class LogicalOperator;
}  // namespace duckdb

namespace sirius::scan_manager {
class sirius_scan_manager;
struct pinned_entry;
}  // namespace sirius::scan_manager

namespace sirius::planner {

/**
 * @brief Callable returning an exact unfiltered row count, a true upper bound, or `std::nullopt`
 *
 * Production converts callback failures to `std::nullopt`; custom-source exceptions propagate.
 * Implementations must not answer from `LogicalGet::estimated_cardinality`, which may reflect
 * filters and understate the domain.
 */
template <class Source>
concept base_table_cardinality_source =
  std::invocable<Source const&, duckdb::LogicalGet const&> &&
  std::same_as<std::invoke_result_t<Source const&, duckdb::LogicalGet const&>,
               std::optional<std::size_t>>;

/**
 * @brief Callable answering whether scan-local output column @p scan_ordinal of a base scan holds
 * distinct values over the whole unfiltered relation
 *
 * The ordinal indexes the scan's projected output (after `projection_ids`), the coordinate the
 * pass-through walk delivers. Implementations answer false for anything they cannot vouch for.
 */
template <class Source>
concept base_table_uniqueness_source =
  std::invocable<Source const&, duckdb::LogicalGet const&, std::size_t> &&
  std::same_as<std::invoke_result_t<Source const&, duckdb::LogicalGet const&, std::size_t>, bool>;

namespace detail {

/**
 * @brief A build-key column traced to its base scan and the scan-local output ordinal it reads
 */
struct resolved_scan_column {
  duckdb::LogicalGet const* scan = nullptr;
  std::size_t scan_ordinal       = 0;
};

/**
 * @brief Traces a column through value-preserving row subsets to its base scan, or returns
 * `std::nullopt` when unresolved
 *
 * Every pass-through step keeps each surviving row's value and never multiplies rows, so a
 * column that is unique in the base scan is unique at @p output_ordinal, and the base scan's row
 * count bounds the traced relation's key domain.
 */
[[nodiscard]] std::optional<resolved_scan_column> resolve_pass_through_scan_column(
  duckdb::LogicalOperator const& subtree, std::size_t output_ordinal) noexcept;

/**
 * @brief Traces a column through value-preserving row subsets, or returns null when unresolved
 */
[[nodiscard]] duckdb::LogicalGet const* resolve_pass_through_scan(
  duckdb::LogicalOperator const& subtree, std::size_t output_ordinal) noexcept;

/**
 * @brief Returns one traced column per original condition; `scan` is null for an untraceable key
 *
 * Call after type and binding resolution and before `create_plan` moves the children.
 */
[[nodiscard]] std::vector<resolved_scan_column> resolve_build_key_scan_columns(
  duckdb::LogicalComparisonJoin const& join);

/**
 * @brief Returns one base scan per original condition, or null for an untraceable build key
 *
 * Call after type and binding resolution and before `create_plan` moves the children.
 */
[[nodiscard]] std::vector<duckdb::LogicalGet const*> resolve_build_key_scans(
  duckdb::LogicalComparisonJoin const& join);

}  // namespace detail

/**
 * @brief Returns one domain bound per original condition; 0 means unknown
 *
 * Each distinct scan is queried once.
 *
 * @pre @p join still owns both logical children; call before `create_plan`
 */
template <base_table_cardinality_source Source>
[[nodiscard]] std::vector<std::size_t> build_key_domain_cardinalities(
  duckdb::LogicalComparisonJoin const& join, Source const& evidence_for)
{
  auto const scans = detail::resolve_build_key_scans(join);
  std::vector<std::size_t> domains(scans.size(), 0);
  std::vector<std::pair<duckdb::LogicalGet const*, std::size_t>> memo;
  for (std::size_t condition_index = 0; condition_index < scans.size(); ++condition_index) {
    if (scans[condition_index] == nullptr) { continue; }
    auto const hit =
      std::ranges::find(memo, scans[condition_index], &decltype(memo)::value_type::first);
    if (hit != memo.end()) {
      domains[condition_index] = hit->second;
      continue;
    }
    domains[condition_index] = evidence_for(*scans[condition_index]).value_or(std::size_t{0});
    memo.emplace_back(scans[condition_index], domains[condition_index]);
  }
  return domains;
}

/**
 * @brief Returns one per-condition flag telling whether the build key is unique in its base scan
 *
 * Untraceable keys are false. The flag feeds dynamic-filter key admission only: with a domain
 * bound it lets the coverage gate read the build's row count as key-domain coverage.
 *
 * @pre @p join still owns both logical children; call before `create_plan`
 */
template <base_table_uniqueness_source Source>
[[nodiscard]] std::vector<bool> build_key_unique_flags(duckdb::LogicalComparisonJoin const& join,
                                                       Source const& unique_for)
{
  auto const columns = detail::resolve_build_key_scan_columns(join);
  std::vector<bool> flags(columns.size(), false);
  for (std::size_t condition_index = 0; condition_index < columns.size(); ++condition_index) {
    auto const& column = columns[condition_index];
    if (column.scan == nullptr) { continue; }
    flags[condition_index] = unique_for(*column.scan, column.scan_ordinal);
  }
  return flags;
}

/**
 * @brief Domain and uniqueness evidence from the DuckDB catalog and the pinned-table registry
 *
 * Domain: `seq_scan` answers from `NodeStatistics::max_cardinality`; a parquet-family scan whose
 * resolved file set is pinned answers the pinned entry's exact row count when a registry was
 * given. Uniqueness: the pinned entry matching the scan (by file set or by catalog identity)
 * declared the column unique through `pin_table(..., unique_cols => [...])`. Everything else,
 * including callback failures, is `std::nullopt` / false. Cardinality estimates are never used.
 *
 * Pinned-entry pointers are held only for this object's lifetime, which must stay inside one
 * planning window (no pin or unpin may interleave).
 */
class duckdb_base_table_evidence {
 public:
  /**
   * @param[in] context Planning connection, for the catalog cardinality callback
   * @param[in] pinned_registry Registry to consult, or null to restrict evidence to the catalog
   */
  duckdb_base_table_evidence(duckdb::ClientContext& context,
                             scan_manager::sirius_scan_manager const* pinned_registry) noexcept;

  [[nodiscard]] std::optional<std::size_t> operator()(duckdb::LogicalGet const& get) const noexcept;

  [[nodiscard]] bool operator()(duckdb::LogicalGet const& get,
                                std::size_t scan_ordinal) const noexcept;

 private:
  [[nodiscard]] scan_manager::pinned_entry const* pinned_entry_for(
    duckdb::LogicalGet const& get) const noexcept;

  duckdb::ClientContext* _context;
  scan_manager::sirius_scan_manager const* _pinned_registry;
  mutable std::vector<std::pair<duckdb::LogicalGet const*, scan_manager::pinned_entry const*>>
    _pinned_memo;
};

}  // namespace sirius::planner
