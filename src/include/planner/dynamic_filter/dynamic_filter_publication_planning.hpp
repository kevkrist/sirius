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

#include "expression/join_condition.hpp"
#include "op/dynamic_filter/dynamic_filter_publish_plan.hpp"

#include <duckdb/common/enums/join_type.hpp>
#include <duckdb/common/unique_ptr.hpp>

#include <cstddef>
#include <optional>
#include <string_view>

namespace duckdb {
class SiriusContext;
}  // namespace duckdb

namespace sirius {
struct operator_params;
namespace op {
class sirius_physical_operator;
}  // namespace op
}  // namespace sirius

namespace sirius::planner {

/**
 * @brief One equality key whose build-side membership should be published to consumers reached
 * through the probe subtree.
 *
 * `condition` follows the hash-join orientation: the probe (filter-consuming) side on the left and
 * the build (filter-producing) side on the right. `shape` must be captured from the original
 * DuckDB expressions before any key materialization rewrites them into bare references.
 */
struct membership_publication_request {
  duckdb::JoinType join_type = duckdb::JoinType::INNER;
  sirius::join_condition condition;
  op::dynamic_filter_condition_shape shape{};
  /// Exact base-table row bound of the build key's domain; 0 disables coverage gating.
  std::size_t build_key_domain_cardinality = 0;
  /// Build-side output column proven unique, when a single-column proof exists.
  std::optional<std::size_t> build_side_unique_column;
  /// Build-side estimated rows, used only for logging.
  std::size_t build_estimated_rows = 0;
};

/**
 * @brief Builds the dynamic-filter publication plan for one membership key, mirroring the target
 * discovery `plan_comparison_join` performs for an eligible hash join.
 *
 * Admits the key via `admit_dynamic_filter_keys`, traces it through @p probe_subtree with
 * `trace_probe_key` (binding every reachable GPU scan), falls back to splicing membership-only
 * `sirius_physical_dynamic_filter` endpoints into @p probe_subtree when no scan binds and the
 * direct route is admissible, registers each target channel's producer declaration, and resolves
 * the per-GPU replica spaces (NUMA-local HOST staging preferred). Returns a disabled plan (no
 * targets) when the key is inadmissible, nothing binds, or no GPU+HOST space pair exists --
 * callers treat a disabled plan as "publication unavailable".
 *
 * The caller owns the evidence decision (whether the build side justifies filters at all) and
 * attaches the returned plan to the publishing operator.
 *
 * @param sirius_context Registered Sirius state supplying the memory manager, hardware topology,
 * and dynamic-filter statistics.
 * @param op_params Operator parameters supplying the publication policy knobs.
 * @param request The key to publish; consumed.
 * @param probe_subtree The physical subtree the filter's consumers live under; may be re-rooted
 * with membership-only endpoint operators.
 * @param log_context Tag prefixed to log lines, e.g. "sirius_plan_aggregate".
 */
[[nodiscard]] op::dynamic_filter_publish_plan plan_single_key_membership_publication(
  duckdb::SiriusContext& sirius_context,
  sirius::operator_params const& op_params,
  membership_publication_request request,
  duckdb::unique_ptr<op::sirius_physical_operator>& probe_subtree,
  std::string_view log_context);

}  // namespace sirius::planner
