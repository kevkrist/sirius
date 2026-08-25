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

#include <duckdb/common/types.hpp>

#include <unordered_set>

namespace duckdb {
class LogicalOperator;
}  // namespace duckdb

namespace sirius::planner {

/**
 * @brief Returns a set of output column indices proven to form a unique key for the given logical
 * operator subtree, or an empty set if uniqueness cannot be proven.
 *
 * The proof is conservative: only PRIMARY KEY constraints (which also guarantee NOT NULL), single
 * grouping sets, and operators whose column mapping is fully traceable contribute. Shared by
 * `plan_comparison_join` (distinct-build eligibility and dynamic-filter key admission) and the
 * GROUPJOIN detection ladder in `sirius_plan_aggregate.cpp` (preserved-side uniqueness for the
 * INNER groupjoin rung). Defined in `sirius_plan_comparison_join.cpp`.
 */
[[nodiscard]] std::unordered_set<duckdb::idx_t> prove_unique_columns(duckdb::LogicalOperator& op);

}  // namespace sirius::planner
