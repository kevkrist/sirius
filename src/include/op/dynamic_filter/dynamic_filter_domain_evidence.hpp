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
#include <optional>
#include <string_view>

namespace sirius::op {

/**
 * @brief Where the planner may take build-key domain and uniqueness evidence for the
 * domain-coverage gate
 *
 * The gate skips a proven-unique key whose complete build covers most of the key's base-table
 * domain (see `domain_coverage_gate_fires`). Both sources are exact; neither answers from
 * cardinality estimates. Missing evidence disables the gate for that key (fail-open: the filter is
 * built).
 */
enum class dynamic_filter_domain_evidence : std::uint8_t {
  /// DuckDB catalog only: `seq_scan` row bounds and PRIMARY KEY constraints.
  catalog_only,
  /// Catalog plus the pinned-table registry: the exact row count of a pinned parquet file set and
  /// the columns `pin_table(..., unique_cols => [...])` declared unique.
  catalog_and_pinned,
};

[[nodiscard]] constexpr std::string_view to_string(dynamic_filter_domain_evidence evidence) noexcept
{
  switch (evidence) {
    case dynamic_filter_domain_evidence::catalog_only: return "catalog_only";
    case dynamic_filter_domain_evidence::catalog_and_pinned: return "catalog_and_pinned";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::optional<dynamic_filter_domain_evidence>
parse_dynamic_filter_domain_evidence(std::string_view text) noexcept
{
  if (text == "catalog_only") { return dynamic_filter_domain_evidence::catalog_only; }
  if (text == "catalog_and_pinned") { return dynamic_filter_domain_evidence::catalog_and_pinned; }
  return std::nullopt;
}

}  // namespace sirius::op
