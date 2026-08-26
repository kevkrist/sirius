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

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/execution/operator/aggregate/physical_perfecthash_aggregate.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "expression/aggregate_id.hpp"
#include "expression/ast/from_duckdb.hpp"
#include "expression/ast/node.hpp"
#include "expression/ast/reference.hpp"
#include "expression/join_condition.hpp"
#include "helper/type_conversions.hpp"
#include "log/logging.hpp"
#include "memory/size_arithmetic.hpp"
#include "op/sirius_physical_group_join.hpp"
#include "op/sirius_physical_grouped_aggregate.hpp"
#include "op/sirius_physical_projection.hpp"
#include "op/sirius_physical_table_scan.hpp"
#include "op/sirius_physical_ungrouped_aggregate.hpp"
#include "planner/dynamic_filter/build_filter_evidence.hpp"
#include "planner/dynamic_filter/build_key_domain.hpp"
#include "planner/dynamic_filter/dynamic_filter_key_admission.hpp"
#include "planner/dynamic_filter/dynamic_filter_publication_planning.hpp"
#include "planner/group_join_stream_admission.hpp"
#include "planner/sirius_physical_plan_generator.hpp"
#include "planner/sirius_plan_projection_utils.hpp"
#include "planner/sirius_plan_unique_columns.hpp"
#include "sirius/exception.hpp"
#include "sirius_context.hpp"

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sirius::planner {

namespace {

// Translate a vector of DuckDB expressions into Sirius AST nodes at the planner
// boundary. The source vector is drained; size and order are preserved. If
// from_duckdb declines an unsupported shape (e.g. an ORDER BY aggregate rewritten
// to arg_min_null / create_sort_key), it returns null — which the downstream
// aggregate/projection operators cannot represent. Rather than build a GPU plan
// containing null nodes (which crashes at execution time), throw here so the
// query falls back to DuckDB's CPU execution.
duckdb::vector<std::unique_ptr<sirius::ast::node>> translate_expressions(
  duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> exprs)
{
  duckdb::vector<std::unique_ptr<sirius::ast::node>> out;
  out.reserve(exprs.size());
  for (auto& e : exprs) {
    auto translated = e ? sirius::ast::from_duckdb(*e) : nullptr;
    if (e && translated == nullptr) {
      throw duckdb::NotImplementedException(
        "Unsupported expression in aggregate (falling back to CPU): " + e->ToString());
    }
    out.push_back(std::move(translated));
  }
  return out;
}

// File-local helper (formerly sirius_physical_plan_generator::extract_aggregate_expressions).
// Pulls aggregate child / filter sub-expressions out of the aggregate list and groups into a
// projection fed upstream of the aggregate. Operates on raw DuckDB expressions so the hoist
// is straightforward; the caller translates the resulting groups/aggregates into Sirius AST
// nodes when constructing the aggregate operator.
duckdb::unique_ptr<sirius::op::sirius_physical_operator> extract_aggregate_expressions(
  duckdb::ClientContext& context,
  duckdb::unique_ptr<sirius::op::sirius_physical_operator> child,
  duckdb::vector<duckdb::unique_ptr<duckdb::Expression>>& aggregates,
  duckdb::vector<duckdb::unique_ptr<duckdb::Expression>>& groups,
  duckdb::optional_ptr<duckdb::vector<duckdb::GroupingSet>> grouping_sets)
{
  duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> expressions;
  duckdb::vector<duckdb::LogicalType> types;

  // bind sorted aggregates
  for (auto& aggr : aggregates) {
    auto& bound_aggr = aggr->Cast<duckdb::BoundAggregateExpression>();
    if (bound_aggr.order_bys) {
      duckdb::FunctionBinder::BindSortedAggregate(context, bound_aggr, groups, grouping_sets);
    }
  }
  for (auto& group : groups) {
    auto ref =
      duckdb::make_uniq<duckdb::BoundReferenceExpression>(group->return_type, expressions.size());
    types.push_back(group->return_type);
    expressions.push_back(std::move(group));
    group = std::move(ref);
  }
  for (auto& aggr : aggregates) {
    auto& bound_aggr = aggr->Cast<duckdb::BoundAggregateExpression>();
    for (auto& child_expr : bound_aggr.children) {
      auto ref = duckdb::make_uniq<duckdb::BoundReferenceExpression>(child_expr->return_type,
                                                                     expressions.size());
      types.push_back(child_expr->return_type);
      expressions.push_back(std::move(child_expr));
      child_expr = std::move(ref);
    }
    if (bound_aggr.filter) {
      auto& filter = bound_aggr.filter;
      auto ref     = duckdb::make_uniq<duckdb::BoundReferenceExpression>(filter->return_type,
                                                                     expressions.size());
      types.push_back(filter->return_type);
      expressions.push_back(std::move(filter));
      bound_aggr.filter = std::move(ref);
    }
  }
  if (expressions.empty()) { return child; }
  auto const estimated_cardinality = child->estimated_cardinality;
  return push_projection(std::move(child),
                         sirius::from_duckdb_vec(types),
                         translate_expressions(std::move(expressions)),
                         estimated_cardinality);
}

// Shared detection infrastructure for the GROUPJOIN ladder: projection-map validation, the
// exact-builtin catalog identity check, and the linear-scan-chain screen. Every rung reuses these
// and consults no statistics.

struct join_projection_layout {
  std::size_t left_output_count;
  std::size_t right_output_count;
};

static std::optional<join_projection_layout> validate_join_projection_layout(
  const duckdb::LogicalComparisonJoin& join)
{
  if (join.children.size() != 2) { return std::nullopt; }
  const std::size_t left_width  = join.children[0]->types.size();
  const std::size_t right_width = join.children[1]->types.size();
  for (auto const index : join.left_projection_map) {
    if (index >= left_width) { return std::nullopt; }
  }
  for (auto const index : join.right_projection_map) {
    if (index >= right_width) { return std::nullopt; }
  }
  return join_projection_layout{
    join.left_projection_map.empty() ? left_width : join.left_projection_map.size(),
    join.right_projection_map.empty() ? right_width : join.right_projection_map.size()};
}

static std::optional<std::pair<std::size_t, std::size_t>> resolve_join_output_column(
  const duckdb::LogicalComparisonJoin& join,
  const join_projection_layout& layout,
  std::size_t position)
{
  if (position < layout.left_output_count) {
    return std::pair<std::size_t, std::size_t>{
      0, join.left_projection_map.empty() ? position : join.left_projection_map[position]};
  }
  const std::size_t right_pos = position - layout.left_output_count;
  if (right_pos >= layout.right_output_count) { return std::nullopt; }
  return std::pair<std::size_t, std::size_t>{
    1, join.right_projection_map.empty() ? right_pos : join.right_projection_map[right_pos]};
}

// Identity projections may be elided, so require the entire child to be a unary scan chain.
static bool is_linear_scan_chain(const duckdb::LogicalOperator& node)
{
  const duckdb::LogicalOperator* current = &node;
  while (true) {
    switch (current->type) {
      case duckdb::LogicalOperatorType::LOGICAL_GET:
        return current->children.empty();  // table-in-out GETs carry children — refuse
      case duckdb::LogicalOperatorType::LOGICAL_FILTER:
      case duckdb::LogicalOperatorType::LOGICAL_PROJECTION:
        if (current->children.size() != 1) { return false; }
        current = current->children[0].get();
        break;
      default: return false;
    }
  }
}

// The extension's hidden DuckDB copy has different callback addresses from bound host functions.
// Compare host-owned catalog functions while rejecting user aggregates that reuse COUNT's name.
static std::optional<sirius::aggregate_id> exact_builtin_count_id(
  duckdb::ClientContext& context, const duckdb::BoundAggregateExpression& aggr)
{
  auto const aggregate_id = sirius::from_duckdb_aggregate_name(aggr.function.name);
  if (!aggregate_id || (*aggregate_id != sirius::aggregate_id::count &&
                        *aggregate_id != sirius::aggregate_id::count_star)) {
    return std::nullopt;
  }

  const bool is_count              = *aggregate_id == sirius::aggregate_id::count;
  const std::size_t expected_arity = is_count ? 1 : 0;
  auto const canonical_name        = sirius::to_duckdb_aggregate_name(*aggregate_id);
  auto const& function             = aggr.function;

  if (function.name != canonical_name || aggr.children.size() != expected_arity ||
      function.arguments.size() != expected_arity ||
      function.GetReturnType() != duckdb::LogicalType::BIGINT ||
      aggr.return_type != duckdb::LogicalType::BIGINT || function.HasVarArgs() ||
      function.GetNullHandling() != duckdb::FunctionNullHandling::SPECIAL_HANDLING ||
      function.GetOrderDependent() != duckdb::AggregateOrderDependent::NOT_ORDER_DEPENDENT ||
      aggr.bind_info != nullptr) {
    return std::nullopt;
  }
  if ((!function.catalog_name.empty() && function.catalog_name != SYSTEM_CATALOG) ||
      (!function.schema_name.empty() && function.schema_name != DEFAULT_SCHEMA)) {
    return std::nullopt;
  }

  auto& entry =
    duckdb::Catalog::GetSystemCatalog(context).GetEntry<duckdb::AggregateFunctionCatalogEntry>(
      context, DEFAULT_SCHEMA, "count");

  const duckdb::AggregateFunction* canonical = nullptr;
  for (auto const& candidate : entry.functions.functions) {
    const bool signature_matches =
      candidate.name == "count" && candidate.arguments.size() == expected_arity &&
      candidate.GetReturnType() == duckdb::LogicalType::BIGINT && !candidate.HasVarArgs() &&
      (!is_count || candidate.arguments[0].id() == duckdb::LogicalTypeId::ANY);
    if (!signature_matches) { continue; }
    // A duplicate canonical signature would make identity ambiguous; fail closed.
    if (canonical != nullptr) { return std::nullopt; }
    canonical = &candidate;
  }
  if (canonical == nullptr || function != *canonical) { return std::nullopt; }
  return aggregate_id;
}

// Ladder rung P0: grouped COUNT over a preserved-side outer equi-join (the q13 shape). Emits an
// OUTER_PRESERVING count spec; every check below is fail-closed.
struct count_pathway_detection {
  std::size_t preserved_child   = 0;
  std::size_t preserved_key_idx = 0;
  std::size_t counted_key_idx   = 0;
  std::optional<std::size_t> counted_value_idx;
};

static std::optional<count_pathway_detection> detect_count_group_join(
  duckdb::ClientContext& context, const duckdb::LogicalAggregate& op)
{
  if (op.groups.size() != 1 || op.grouping_sets.size() != 1 || op.grouping_sets[0].size() != 1 ||
      op.grouping_sets[0].count(0) != 1 || !op.grouping_functions.empty() ||
      op.expressions.size() != 1) {
    return std::nullopt;
  }
  if (op.groups[0]->GetExpressionClass() != duckdb::ExpressionClass::BOUND_REF) {
    return std::nullopt;
  }
  if (op.expressions[0]->GetExpressionClass() != duckdb::ExpressionClass::BOUND_AGGREGATE) {
    return std::nullopt;
  }
  auto const& aggr        = op.expressions[0]->Cast<duckdb::BoundAggregateExpression>();
  auto const aggregate_id = exact_builtin_count_id(context, aggr);
  if (!aggregate_id) { return std::nullopt; }
  const bool is_count      = *aggregate_id == sirius::aggregate_id::count;
  const bool is_count_star = *aggregate_id == sirius::aggregate_id::count_star;
  if (aggr.IsDistinct() || aggr.filter || aggr.order_bys) { return std::nullopt; }
  if (is_count && (aggr.children.size() != 1 ||
                   aggr.children[0]->GetExpressionClass() != duckdb::ExpressionClass::BOUND_REF)) {
    return std::nullopt;
  }
  if (is_count_star && !aggr.children.empty()) { return std::nullopt; }

  if (op.children.size() != 1 ||
      op.children[0]->type != duckdb::LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
    return std::nullopt;
  }
  auto const& join = op.children[0]->Cast<duckdb::LogicalComparisonJoin>();
  if (join.join_type != duckdb::JoinType::LEFT && join.join_type != duckdb::JoinType::RIGHT) {
    return std::nullopt;
  }
  if (join.children.size() != 2 || join.conditions.size() != 1 || join.predicate) {
    return std::nullopt;
  }
  auto const layout = validate_join_projection_layout(join);
  if (!layout) { return std::nullopt; }
  if (layout->left_output_count >
      std::numeric_limits<std::size_t>::max() - layout->right_output_count) {
    return std::nullopt;
  }
  const auto join_output_width = layout->left_output_count + layout->right_output_count;
  if (join.types.size() != join_output_width) { return std::nullopt; }
  for (std::size_t output_idx = 0; output_idx < layout->left_output_count; ++output_idx) {
    auto const child_idx =
      join.left_projection_map.empty() ? output_idx : join.left_projection_map[output_idx];
    if (join.types[output_idx] != join.children[0]->types[child_idx]) { return std::nullopt; }
  }
  for (std::size_t right_idx = 0; right_idx < layout->right_output_count; ++right_idx) {
    auto const child_idx =
      join.right_projection_map.empty() ? right_idx : join.right_projection_map[right_idx];
    auto const output_idx = layout->left_output_count + right_idx;
    if (join.types[output_idx] != join.children[1]->types[child_idx]) { return std::nullopt; }
  }

  auto const& cond = join.conditions[0];
  if (cond.comparison != duckdb::ExpressionType::COMPARE_EQUAL ||
      cond.left->GetExpressionClass() != duckdb::ExpressionClass::BOUND_REF ||
      cond.right->GetExpressionClass() != duckdb::ExpressionClass::BOUND_REF) {
    return std::nullopt;
  }
  auto const& left_ref  = cond.left->Cast<duckdb::BoundReferenceExpression>();
  auto const& right_ref = cond.right->Cast<duckdb::BoundReferenceExpression>();
  if (left_ref.index >= join.children[0]->types.size() ||
      right_ref.index >= join.children[1]->types.size() ||
      left_ref.return_type != join.children[0]->types[left_ref.index] ||
      right_ref.return_type != join.children[1]->types[right_ref.index]) {
    return std::nullopt;
  }
  const auto key_type_id = cond.left->return_type.id();
  if (key_type_id != cond.right->return_type.id() ||
      (key_type_id != duckdb::LogicalTypeId::INTEGER &&
       key_type_id != duckdb::LogicalTypeId::BIGINT)) {
    return std::nullopt;
  }

  count_pathway_detection det;
  det.preserved_child             = join.join_type == duckdb::JoinType::LEFT ? 0 : 1;
  const std::size_t counted_child = 1 - det.preserved_child;

  for (auto const& child : join.children) {
    if (!is_linear_scan_chain(*child)) { return std::nullopt; }
  }

  auto const& group_ref = op.groups[0]->Cast<duckdb::BoundReferenceExpression>();
  if (group_ref.index >= join_output_width ||
      group_ref.return_type != join.types[group_ref.index]) {
    return std::nullopt;
  }
  const auto group_target = resolve_join_output_column(join, *layout, group_ref.index);
  if (!group_target || group_target->first != det.preserved_child) { return std::nullopt; }
  const auto& preserved_cond = det.preserved_child == 0 ? cond.left : cond.right;
  det.preserved_key_idx      = preserved_cond->Cast<duckdb::BoundReferenceExpression>().index;
  if (group_target->second != det.preserved_key_idx) { return std::nullopt; }

  const auto& counted_cond = det.preserved_child == 0 ? cond.right : cond.left;
  det.counted_key_idx      = counted_cond->Cast<duckdb::BoundReferenceExpression>().index;
  if (det.preserved_key_idx >= join.children[det.preserved_child]->types.size() ||
      det.counted_key_idx >= join.children[counted_child]->types.size()) {
    return std::nullopt;
  }

  // COUNT(col): the argument must come from the counted side (a preserved-side or computed
  // argument has different NULL semantics under the outer join).
  if (is_count) {
    auto const& count_ref = aggr.children[0]->Cast<duckdb::BoundReferenceExpression>();
    if (count_ref.index >= join_output_width ||
        count_ref.return_type != join.types[count_ref.index]) {
      return std::nullopt;
    }
    const auto count_target = resolve_join_output_column(join, *layout, count_ref.index);
    if (!count_target || count_target->first != counted_child) { return std::nullopt; }
    if (count_target->second >= join.children[counted_child]->types.size()) { return std::nullopt; }
    det.counted_value_idx = count_target->second;
  }
  return det;
}

// The value rungs cannot authenticate min/max/avg/sum by signature comparison alone: DuckDB's
// binder rewrites those entries per argument type (BindMinMax specializes the ANY entry, and the
// DECIMAL avg/sum entries are retargeted to the matching integer kernels, avg attaching a scale
// bind datum). Authenticate by reproducing the binder's work instead: re-bind the system
// catalog's own entry over the same argument type and require the query's bound function --
// callbacks, signature, flags, and bind data -- to match the reproduction exactly.
// sum_no_overflow (the optimizer's stats rewrite of sum) is compared against its catalog member
// directly because its bind callback is intentionally uncallable.
static std::optional<sirius::aggregate_id> exact_builtin_value_aggregate_id(
  duckdb::ClientContext& context, const duckdb::BoundAggregateExpression& aggr)
{
  auto const aggregate_id = sirius::from_duckdb_aggregate_name(aggr.function.name);
  if (!aggregate_id) { return std::nullopt; }
  switch (*aggregate_id) {
    case sirius::aggregate_id::sum:
    case sirius::aggregate_id::sum_no_overflow:
    case sirius::aggregate_id::min:
    case sirius::aggregate_id::max:
    case sirius::aggregate_id::avg: break;
    default: return std::nullopt;
  }
  auto const canonical_name = sirius::to_duckdb_aggregate_name(*aggregate_id);
  auto const& function      = aggr.function;
  if (function.name != canonical_name || aggr.children.size() != 1 ||
      function.arguments.size() != 1 || function.HasVarArgs()) {
    return std::nullopt;
  }
  auto const& argument_type = aggr.children[0]->return_type;
  if (function.arguments[0] != argument_type) { return std::nullopt; }
  // The expression-level result must be the function's declared result, modulo the plan-level
  // HUGEINT -> BIGINT downcast that runs before detection.
  if (aggr.return_type != function.GetReturnType() &&
      !(function.GetReturnType() == duckdb::LogicalType::HUGEINT &&
        aggr.return_type == duckdb::LogicalType::BIGINT)) {
    return std::nullopt;
  }
  if ((!function.catalog_name.empty() && function.catalog_name != SYSTEM_CATALOG) ||
      (!function.schema_name.empty() && function.schema_name != DEFAULT_SCHEMA)) {
    return std::nullopt;
  }

  try {
    auto& entry =
      duckdb::Catalog::GetSystemCatalog(context).GetEntry<duckdb::AggregateFunctionCatalogEntry>(
        context, DEFAULT_SCHEMA, std::string(canonical_name));
    duckdb::FunctionBinder binder(context);
    duckdb::vector<duckdb::LogicalType> argument_types{argument_type};
    duckdb::ErrorData error;
    auto const best = binder.BindFunction(entry.name, entry.functions, argument_types, error);
    if (!best.IsValid()) { return std::nullopt; }
    auto canonical = entry.functions.GetFunctionByOffset(best.GetIndex());
    duckdb::unique_ptr<duckdb::FunctionData> canonical_bind_info;
    if (*aggregate_id != sirius::aggregate_id::sum_no_overflow && canonical.HasBindCallback()) {
      duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> synthetic_children;
      synthetic_children.push_back(
        duckdb::make_uniq<duckdb::BoundConstantExpression>(duckdb::Value(argument_type)));
      canonical_bind_info = canonical.GetBindCallback()(context, canonical, synthetic_children);
    }
    if (function != canonical || canonical.name != canonical_name ||
        canonical.arguments.size() != 1 || canonical.arguments[0] != argument_type ||
        canonical.HasVarArgs() || canonical.GetReturnType() != function.GetReturnType() ||
        canonical.GetNullHandling() != function.GetNullHandling() ||
        canonical.GetOrderDependent() != function.GetOrderDependent()) {
      return std::nullopt;
    }
    if ((aggr.bind_info == nullptr) != (canonical_bind_info == nullptr)) { return std::nullopt; }
    if (aggr.bind_info != nullptr && !aggr.bind_info->Equals(*canonical_bind_info)) {
      return std::nullopt;
    }
  } catch (...) {
    // A missing catalog entry or a throwing bind is a fallback signal, never a planner error.
    return std::nullopt;
  }
  return aggregate_id;
}

// Shared screens for the value rungs (P1 and P2). Rung P0 keeps its own inline checks verbatim.

// Maps a recognized builtin aggregate name onto its GROUPJOIN slot op; nullopt outside the
// supported set.
static std::optional<sirius::op::groupjoin::agg_op> supported_slot_op(sirius::aggregate_id id)
{
  switch (id) {
    case sirius::aggregate_id::count_star: return sirius::op::groupjoin::agg_op::COUNT_STAR;
    case sirius::aggregate_id::count: return sirius::op::groupjoin::agg_op::COUNT_VALID;
    case sirius::aggregate_id::sum:
    case sirius::aggregate_id::sum_no_overflow: return sirius::op::groupjoin::agg_op::SUM;
    case sirius::aggregate_id::min: return sirius::op::groupjoin::agg_op::MIN;
    case sirius::aggregate_id::max: return sirius::op::groupjoin::agg_op::MAX;
    case sirius::aggregate_id::avg: return sirius::op::groupjoin::agg_op::AVG;
    default: return std::nullopt;
  }
}

static bool is_count_slot_op(sirius::op::groupjoin::agg_op op)
{
  return op == sirius::op::groupjoin::agg_op::COUNT_STAR ||
         op == sirius::op::groupjoin::agg_op::COUNT_VALID;
}

// The declared output must be one the fused operator can emit (mirrors the operator's spec
// validation, which throws -- and throwing at plan time would fall the whole query back to CPU
// instead of to generic GPU planning).
static bool declared_output_type_admissible(sirius::op::groupjoin::agg_op op,
                                            duckdb::LogicalTypeId output_id)
{
  switch (op) {
    case sirius::op::groupjoin::agg_op::COUNT_STAR:
    case sirius::op::groupjoin::agg_op::COUNT_VALID:
      return output_id == duckdb::LogicalTypeId::BIGINT;
    case sirius::op::groupjoin::agg_op::SUM:
      return output_id == duckdb::LogicalTypeId::BIGINT ||
             output_id == duckdb::LogicalTypeId::DECIMAL;
    case sirius::op::groupjoin::agg_op::MIN:
    case sirius::op::groupjoin::agg_op::MAX:
      return output_id == duckdb::LogicalTypeId::INTEGER ||
             output_id == duckdb::LogicalTypeId::BIGINT ||
             output_id == duckdb::LogicalTypeId::DECIMAL;
    case sirius::op::groupjoin::agg_op::AVG:
      return output_id == duckdb::LogicalTypeId::DOUBLE ||
             output_id == duckdb::LogicalTypeId::DECIMAL;
  }
  return false;
}

// v1 argument carriers: INT32/INT64, or DECIMAL stored as one of those.
static bool supported_argument_carrier(duckdb::LogicalType const& type)
{
  switch (type.id()) {
    case duckdb::LogicalTypeId::INTEGER:
    case duckdb::LogicalTypeId::BIGINT: return true;
    case duckdb::LogicalTypeId::DECIMAL:
      return type.InternalType() == duckdb::PhysicalType::INT32 ||
             type.InternalType() == duckdb::PhysicalType::INT64;
    default: return false;
  }
}

// Catalog identity, the one check that does catalog work: COUNT ops authenticate through the
// exact-builtin COUNT check, value ops by re-binding the system entry. Fail-closed either way.
static bool aggregate_catalog_identity_matches(duckdb::ClientContext& context,
                                               const duckdb::BoundAggregateExpression& aggr,
                                               sirius::aggregate_id name_id,
                                               sirius::op::groupjoin::agg_op slot_op)
{
  if (is_count_slot_op(slot_op)) { return exact_builtin_count_id(context, aggr).has_value(); }
  auto const identity = exact_builtin_value_aggregate_id(context, aggr);
  return identity && *identity == name_id;
}

// The value rungs screen dozens of properties and a miss is silent by design (every generic
// grouped aggregate takes this path); the reason is recorded at DEBUG so a query that
// unexpectedly fails to fuse can be diagnosed from its log.
static std::nullopt_t inner_rung_miss(std::string_view reason)
{
  SIRIUS_LOG_DEBUG("[sirius_plan_aggregate] GROUP_JOIN rung P1 miss: {}", reason);
  return std::nullopt;
}

static std::nullopt_t direct_rung_miss(std::string_view reason)
{
  SIRIUS_LOG_DEBUG("[sirius_plan_aggregate] GROUP_JOIN rung P2 miss: {}", reason);
  return std::nullopt;
}

// Ladder rung P1: a single value or count aggregate grouped by the preserved-side key of an INNER
// equi-join (the q17 correlated-AVG shape). Emits an INNER value spec; every check is fail-closed.
struct inner_value_detection {
  std::size_t preserved_child   = 0;
  std::size_t preserved_key_idx = 0;
  std::size_t counted_key_idx   = 0;
  std::optional<std::size_t> counted_value_idx;
  sirius::op::groupjoin::agg_op slot_op = sirius::op::groupjoin::agg_op::COUNT_STAR;
  bool preserved_is_delim               = false;
};

static std::optional<inner_value_detection> detect_inner_value_group_join(
  duckdb::ClientContext& context, const duckdb::LogicalAggregate& op)
{
  // The group/aggregate shape screens mirror rung P0 -- one BOUND_REF group and one bound
  // aggregate without DISTINCT/FILTER/ORDER BY -- with one widening: subquery flattening builds
  // the delim aggregate by appending the correlation group to an originally ungrouped aggregate,
  // which leaves `grouping_sets` empty; empty-with-one-group is plain GROUP BY semantics.
  bool const plain_single_grouping_set =
    op.grouping_sets.empty() || (op.grouping_sets.size() == 1 && op.grouping_sets[0].size() == 1 &&
                                 op.grouping_sets[0].count(0) == 1);
  if (op.groups.size() != 1 || !plain_single_grouping_set || !op.grouping_functions.empty() ||
      op.expressions.size() != 1) {
    return inner_rung_miss(
      "group/aggregate shape (one BOUND_REF group, one aggregate, plain grouping set)");
  }
  if (op.groups[0]->GetExpressionClass() != duckdb::ExpressionClass::BOUND_REF) {
    return inner_rung_miss("group is not a bound reference");
  }
  if (op.expressions[0]->GetExpressionClass() != duckdb::ExpressionClass::BOUND_AGGREGATE) {
    return inner_rung_miss("aggregate expression is not a bound aggregate");
  }
  auto const& aggr = op.expressions[0]->Cast<duckdb::BoundAggregateExpression>();
  if (aggr.IsDistinct() || aggr.filter || aggr.order_bys) {
    return inner_rung_miss("aggregate has DISTINCT, FILTER, or ORDER BY");
  }

  auto const name_id = sirius::from_duckdb_aggregate_name(aggr.function.name);
  if (!name_id) { return inner_rung_miss("aggregate name is not a recognized builtin"); }
  auto const slot_op = supported_slot_op(*name_id);
  if (!slot_op) { return inner_rung_miss("aggregate id outside the supported set"); }
  inner_value_detection det;
  det.slot_op               = *slot_op;
  bool const needs_argument = det.slot_op != sirius::op::groupjoin::agg_op::COUNT_STAR;
  if (needs_argument && (aggr.children.size() != 1 || aggr.children[0]->GetExpressionClass() !=
                                                        duckdb::ExpressionClass::BOUND_REF)) {
    return inner_rung_miss("aggregate argument is not a single bound reference");
  }
  if (!needs_argument && !aggr.children.empty()) {
    return inner_rung_miss("COUNT(*) carries arguments");
  }

  // The child must be an INNER equi-join with exactly one plain-equality condition, no residual
  // predicate, and validated projection maps -- the same join screens as rung P0 apart from the
  // join type. RemoveUnusedColumns may leave bare column-selection projections between the
  // aggregate and the join (it does for q17 at larger scales); compose them into a remap so the
  // group and argument references resolve through to the join output, and refuse any projection
  // that computes.
  if (op.children.size() != 1) { return inner_rung_miss("child is not a comparison join"); }
  const duckdb::LogicalOperator* join_node = op.children[0].get();
  std::optional<std::vector<std::size_t>> projection_remap;
  while (join_node->type == duckdb::LogicalOperatorType::LOGICAL_PROJECTION) {
    if (join_node->children.size() != 1) {
      return inner_rung_miss("intervening projection is not unary");
    }
    auto const& child_types = join_node->children[0]->types;
    std::vector<std::size_t> layer;
    layer.reserve(join_node->expressions.size());
    for (std::size_t expr_idx = 0; expr_idx < join_node->expressions.size(); ++expr_idx) {
      auto const& expr = join_node->expressions[expr_idx];
      if (expr->GetExpressionClass() != duckdb::ExpressionClass::BOUND_REF) {
        return inner_rung_miss("intervening projection computes expressions");
      }
      auto const& ref = expr->Cast<duckdb::BoundReferenceExpression>();
      if (ref.index >= child_types.size() || ref.return_type != child_types[ref.index] ||
          expr_idx >= join_node->types.size() || join_node->types[expr_idx] != ref.return_type) {
        return inner_rung_miss("intervening projection reference is invalid");
      }
      layer.push_back(ref.index);
    }
    if (projection_remap) {
      for (auto& mapped : *projection_remap) {
        if (mapped >= layer.size()) {
          return inner_rung_miss("intervening projection reference is invalid");
        }
        mapped = layer[mapped];
      }
    } else {
      projection_remap = std::move(layer);
    }
    join_node = join_node->children[0].get();
  }
  if (join_node->type != duckdb::LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
    return inner_rung_miss("child is not a comparison join");
  }
  auto const& join = join_node->Cast<duckdb::LogicalComparisonJoin>();
  if (join.join_type != duckdb::JoinType::INNER) {
    return inner_rung_miss("join type is not INNER");
  }
  if (join.children.size() != 2 || join.conditions.size() != 1 || join.predicate) {
    return inner_rung_miss("join arity, condition count, or residual predicate");
  }
  auto const layout = validate_join_projection_layout(join);
  if (!layout) { return inner_rung_miss("join projection maps are invalid"); }
  if (layout->left_output_count >
      std::numeric_limits<std::size_t>::max() - layout->right_output_count) {
    return inner_rung_miss("join output width overflows");
  }
  const auto join_output_width = layout->left_output_count + layout->right_output_count;
  if (join.types.size() != join_output_width) {
    return inner_rung_miss("join output width does not match the projection maps");
  }
  for (std::size_t output_idx = 0; output_idx < layout->left_output_count; ++output_idx) {
    auto const child_idx =
      join.left_projection_map.empty() ? output_idx : join.left_projection_map[output_idx];
    if (join.types[output_idx] != join.children[0]->types[child_idx]) {
      return inner_rung_miss("left projection output type mismatch");
    }
  }
  for (std::size_t right_idx = 0; right_idx < layout->right_output_count; ++right_idx) {
    auto const child_idx =
      join.right_projection_map.empty() ? right_idx : join.right_projection_map[right_idx];
    auto const output_idx = layout->left_output_count + right_idx;
    if (join.types[output_idx] != join.children[1]->types[child_idx]) {
      return inner_rung_miss("right projection output type mismatch");
    }
  }

  auto const& cond = join.conditions[0];
  if (cond.comparison != duckdb::ExpressionType::COMPARE_EQUAL ||
      cond.left->GetExpressionClass() != duckdb::ExpressionClass::BOUND_REF ||
      cond.right->GetExpressionClass() != duckdb::ExpressionClass::BOUND_REF) {
    return inner_rung_miss("condition is not a plain equality of bound references");
  }
  auto const& left_ref  = cond.left->Cast<duckdb::BoundReferenceExpression>();
  auto const& right_ref = cond.right->Cast<duckdb::BoundReferenceExpression>();
  if (left_ref.index >= join.children[0]->types.size() ||
      right_ref.index >= join.children[1]->types.size() ||
      left_ref.return_type != join.children[0]->types[left_ref.index] ||
      right_ref.return_type != join.children[1]->types[right_ref.index]) {
    return inner_rung_miss("condition reference indices or types are invalid");
  }
  const auto key_type_id = cond.left->return_type.id();
  if (key_type_id != cond.right->return_type.id() ||
      (key_type_id != duckdb::LogicalTypeId::INTEGER &&
       key_type_id != duckdb::LogicalTypeId::BIGINT)) {
    return inner_rung_miss("join keys are not both INT32 or both INT64");
  }

  // Resolve an aggregate-input reference through the intervening projections to its join output
  // ordinal; the hop types were validated when the remap was composed.
  auto const resolve_to_join_output = [&](std::size_t index) -> std::optional<std::size_t> {
    if (projection_remap) {
      if (index >= projection_remap->size()) { return std::nullopt; }
      index = (*projection_remap)[index];
    }
    if (index >= join_output_width) { return std::nullopt; }
    return index;
  };

  // The group must resolve to one side's join key.
  auto const& group_ref  = op.groups[0]->Cast<duckdb::BoundReferenceExpression>();
  auto const group_index = resolve_to_join_output(group_ref.index);
  if (!group_index || group_ref.return_type != join.types[*group_index]) {
    return inner_rung_miss("group reference index or type is invalid");
  }
  const auto group_target = resolve_join_output_column(join, *layout, *group_index);
  if (!group_target) { return inner_rung_miss("group does not resolve to a join output column"); }
  auto const key_idx_of = [&](std::size_t side) {
    return (side == 0 ? cond.left : cond.right)->Cast<duckdb::BoundReferenceExpression>().index;
  };
  if (group_target->second != key_idx_of(group_target->first)) {
    return inner_rung_miss("group is not its side's join key");
  }

  // Pick the preserved side: a childless DELIM_GET (delim provenance, the primary q17 target) or
  // a linear scan chain whose key is proven unique -- the sigma-asymmetry evidence behind this
  // form's relaxed runtime gate table, not a correctness requirement. The group's own side is
  // preferred, but a plain INNER equality makes the two key columns pairwise equal on every
  // emitted row, so the optimizer is free to bind the group to either side (and does); when the
  // group's side lacks preserved provenance the group is re-homed onto the other side's key,
  // which emits identical group values.
  auto const side_qualifies = [&](std::size_t side, bool& is_delim) {
    auto& node = *join.children[side];
    if (node.type == duckdb::LogicalOperatorType::LOGICAL_DELIM_GET && node.children.empty()) {
      is_delim = true;
      return true;
    }
    is_delim = false;
    if (!is_linear_scan_chain(node)) { return false; }
    auto const unique_columns = prove_unique_columns(node);
    return unique_columns.size() == 1 &&
           *unique_columns.begin() == static_cast<duckdb::idx_t>(key_idx_of(side));
  };
  if (side_qualifies(group_target->first, det.preserved_is_delim)) {
    det.preserved_child = group_target->first;
  } else if (side_qualifies(1 - group_target->first, det.preserved_is_delim)) {
    det.preserved_child = 1 - group_target->first;
  } else {
    return inner_rung_miss(
      "neither join side is a childless DELIM_GET or a unique-keyed linear scan chain");
  }
  const std::size_t counted_child = 1 - det.preserved_child;
  det.preserved_key_idx           = key_idx_of(det.preserved_child);
  det.counted_key_idx             = key_idx_of(counted_child);

  if (!is_linear_scan_chain(*join.children[counted_child])) {
    return inner_rung_miss("counted side is not a linear scan chain");
  }

  // The aggregate argument must come from the counted side, with a v1-supported carrier
  // (INT32/INT64, or DECIMAL stored as one of those).
  if (needs_argument) {
    auto const& arg_ref  = aggr.children[0]->Cast<duckdb::BoundReferenceExpression>();
    auto const arg_index = resolve_to_join_output(arg_ref.index);
    if (!arg_index || arg_ref.return_type != join.types[*arg_index]) {
      return inner_rung_miss("argument reference index or type is invalid");
    }
    const auto arg_target = resolve_join_output_column(join, *layout, *arg_index);
    if (!arg_target || arg_target->first != counted_child) {
      return inner_rung_miss("argument does not resolve to the counted side");
    }
    if (arg_target->second >= join.children[counted_child]->types.size()) {
      return inner_rung_miss("argument column index is out of range");
    }
    det.counted_value_idx = arg_target->second;
    if (!supported_argument_carrier(arg_ref.return_type)) {
      return inner_rung_miss("argument type outside INT32/INT64/DECIMAL32/DECIMAL64");
    }
  }

  if (!declared_output_type_admissible(det.slot_op, aggr.return_type.id())) {
    return inner_rung_miss("declared output type unsupported for this aggregate");
  }

  // Catalog identity last: it is the only check that does catalog work.
  if (!aggregate_catalog_identity_matches(context, aggr, *name_id, det.slot_op)) {
    return inner_rung_miss("aggregate catalog identity check failed");
  }
  return det;
}

// Ladder rung P2: a single INT32/INT64-keyed aggregate over an opaque comparison-join child (the
// q2 MIN-per-partkey shape). The child is planned unchanged -- any join type, any conditions --
// which is sound because the fused DIRECT form computes plain GROUP BY semantics over the child's
// output and the executor's argument-validity gate routes outer-join padding NULLs to the exact
// mask-preserving sparse strategy. Requiring the comparison-join root is the anti-overlap guard:
// it confines this rung to fragments where a fused join+aggregate shape was the alternative,
// keeping it out of generic HASH_GROUP_BY / perfect-hash aggregate territory. Unlike rung P1 the
// group key need not be a join key. Every check is fail-closed.
struct direct_pathway_detection {
  std::size_t group_key_idx = 0;
  std::optional<std::size_t> arg_idx;
  sirius::op::groupjoin::agg_op slot_op = sirius::op::groupjoin::agg_op::COUNT_STAR;
};

static std::optional<direct_pathway_detection> detect_direct_group_join(
  duckdb::ClientContext& context, const duckdb::LogicalAggregate& op)
{
  // Same group/aggregate shape screens as rung P1, including the empty-grouping-set widening.
  bool const plain_single_grouping_set =
    op.grouping_sets.empty() || (op.grouping_sets.size() == 1 && op.grouping_sets[0].size() == 1 &&
                                 op.grouping_sets[0].count(0) == 1);
  if (op.groups.size() != 1 || !plain_single_grouping_set || !op.grouping_functions.empty() ||
      op.expressions.size() != 1) {
    return direct_rung_miss(
      "group/aggregate shape (one BOUND_REF group, one aggregate, plain grouping set)");
  }
  if (op.groups[0]->GetExpressionClass() != duckdb::ExpressionClass::BOUND_REF) {
    return direct_rung_miss("group is not a bound reference");
  }
  if (op.expressions[0]->GetExpressionClass() != duckdb::ExpressionClass::BOUND_AGGREGATE) {
    return direct_rung_miss("aggregate expression is not a bound aggregate");
  }
  auto const& aggr = op.expressions[0]->Cast<duckdb::BoundAggregateExpression>();
  if (aggr.IsDistinct() || aggr.filter || aggr.order_bys) {
    return direct_rung_miss("aggregate has DISTINCT, FILTER, or ORDER BY");
  }

  auto const name_id = sirius::from_duckdb_aggregate_name(aggr.function.name);
  if (!name_id) { return direct_rung_miss("aggregate name is not a recognized builtin"); }
  auto const slot_op = supported_slot_op(*name_id);
  if (!slot_op) { return direct_rung_miss("aggregate id outside the supported set"); }
  direct_pathway_detection det;
  det.slot_op               = *slot_op;
  bool const needs_argument = det.slot_op != sirius::op::groupjoin::agg_op::COUNT_STAR;
  if (needs_argument && (aggr.children.size() != 1 || aggr.children[0]->GetExpressionClass() !=
                                                        duckdb::ExpressionClass::BOUND_REF)) {
    return direct_rung_miss("aggregate argument is not a single bound reference");
  }
  if (!needs_argument && !aggr.children.empty()) {
    return direct_rung_miss("COUNT(*) carries arguments");
  }

  // The strict join-root requirement (the anti-overlap guard). The child stays opaque beyond its
  // root type: no condition, projection-map, or subtree screens apply because the child is
  // planned as-is and the fusion replaces only the aggregate above it.
  if (op.children.size() != 1 ||
      op.children[0]->type != duckdb::LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
    return direct_rung_miss("child is not a comparison join");
  }
  auto const& child_types = op.children[0]->types;

  // Group-key and argument indices come directly from the aggregate's input references into the
  // child's output (the child is planned unchanged, so no remapping applies).
  auto const& group_ref    = op.groups[0]->Cast<duckdb::BoundReferenceExpression>();
  auto const group_type_id = group_ref.return_type.id();
  if (group_type_id != duckdb::LogicalTypeId::INTEGER &&
      group_type_id != duckdb::LogicalTypeId::BIGINT) {
    return direct_rung_miss("group key is not INT32 or INT64");
  }
  if (group_ref.index >= child_types.size() ||
      group_ref.return_type != child_types[group_ref.index]) {
    return direct_rung_miss("group reference index or type is invalid");
  }
  det.group_key_idx = group_ref.index;

  if (needs_argument) {
    auto const& arg_ref = aggr.children[0]->Cast<duckdb::BoundReferenceExpression>();
    if (arg_ref.index >= child_types.size() || arg_ref.return_type != child_types[arg_ref.index]) {
      return direct_rung_miss("argument reference index or type is invalid");
    }
    if (!supported_argument_carrier(arg_ref.return_type)) {
      return direct_rung_miss("argument type outside INT32/INT64/DECIMAL32/DECIMAL64");
    }
    det.arg_idx = arg_ref.index;
  }

  if (!declared_output_type_admissible(det.slot_op, aggr.return_type.id())) {
    return direct_rung_miss("declared output type unsupported for this aggregate");
  }

  // Catalog identity last: it is the only check that does catalog work.
  if (!aggregate_catalog_identity_matches(context, aggr, *name_id, det.slot_op)) {
    return direct_rung_miss("aggregate catalog identity check failed");
  }
  return det;
}

// Display name for the value-rung fusion log lines.
static std::string_view aggregate_op_display_name(sirius::op::groupjoin::agg_op op)
{
  switch (op) {
    case sirius::op::groupjoin::agg_op::COUNT_STAR: return "COUNT(*)";
    case sirius::op::groupjoin::agg_op::COUNT_VALID: return "COUNT";
    case sirius::op::groupjoin::agg_op::SUM: return "SUM";
    case sirius::op::groupjoin::agg_op::MIN: return "MIN";
    case sirius::op::groupjoin::agg_op::MAX: return "MAX";
    case sirius::op::groupjoin::agg_op::AVG: return "AVG";
  }
  return "UNKNOWN";
}

// Byte estimate for one row of @p types: fixed-width columns use their physical size and
// variable-width columns the configured average, mirroring the GPU-admission estimator's
// convention.
static std::size_t estimated_row_bytes(duckdb::vector<duckdb::LogicalType> const& types,
                                       std::size_t avg_variable_column_bytes)
{
  std::size_t total = 0;
  for (auto const& type : types) {
    auto const physical = type.InternalType();
    std::size_t width   = 0;
    switch (physical) {
      case duckdb::PhysicalType::VARCHAR:
      case duckdb::PhysicalType::LIST:
      case duckdb::PhysicalType::STRUCT:
      case duckdb::PhysicalType::ARRAY: width = avg_variable_column_bytes; break;
      default: width = duckdb::GetTypeIdSize(physical); break;
    }
    total = sirius::memory::saturating_add(total, width);
  }
  return total;
}

}  // namespace

// The GROUPJOIN detection ladder: rungs are evaluated cheapest-first, each behind its own config
// gate, and any miss falls through to the next rung and finally to generic join+aggregate
// planning. Rung P0 is the COUNT-over-outer-join pathway and stays first and verbatim; rung P1 is
// the INNER value pathway.
duckdb::unique_ptr<sirius::op::sirius_physical_operator>
sirius_physical_plan_generator::try_plan_group_join(duckdb::LogicalAggregate& op)
{
  auto sirius_ctx = context.registered_state
                      ? context.registered_state->Get<duckdb::SiriusContext>("sirius_state")
                      : nullptr;
  if (!sirius_ctx) { return nullptr; }
  const auto& op_params = sirius_ctx->get_config().get_operator_params();

  // Rung P0. The config bool gates only this rung; later rungs are gated independently.
  if (op_params.enable_dense_count_join) {
    auto detection = detect_count_group_join(context, op);
    if (detection) {
      auto& join                      = op.children[0]->Cast<duckdb::LogicalComparisonJoin>();
      const std::size_t counted_child = 1 - detection->preserved_child;

      // Mirror plan_comparison_join: capture cardinalities before create_plan drains the nodes.
      const std::size_t preserved_cardinality =
        join.children[detection->preserved_child]->EstimateCardinality(context);
      const std::size_t counted_cardinality =
        join.children[counted_child]->EstimateCardinality(context);

      auto preserved                   = create_plan(*join.children[detection->preserved_child]);
      auto counted                     = create_plan(*join.children[counted_child]);
      preserved->estimated_cardinality = preserved_cardinality;
      counted->estimated_cardinality   = counted_cardinality;
      if (preserved->children.size() > 1 || counted->children.size() > 1) {
        throw sirius::internal_exception(
          "group_join: eligible logical child produced a non-unary physical root");
      }

      SIRIUS_LOG_INFO(
        "[sirius_plan_aggregate] Fusing COUNT-join into GROUP_JOIN: {} join, preserved child "
        "{} (key col {}, est {} rows), counted child {} (key col {}, {}, est {} rows)",
        join.join_type == duckdb::JoinType::LEFT ? "LEFT" : "RIGHT",
        detection->preserved_child,
        detection->preserved_key_idx,
        preserved_cardinality,
        counted_child,
        detection->counted_key_idx,
        detection->counted_value_idx
          ? "COUNT(col " + std::to_string(*detection->counted_value_idx) + ")"
          : std::string("COUNT(*)"),
        counted_cardinality);

      auto types = sirius::from_duckdb_vec(op.types);
      sirius::op::groupjoin::group_join_spec spec;
      spec.form              = sirius::op::groupjoin::join_form::OUTER_PRESERVING;
      spec.preserved_key_idx = detection->preserved_key_idx;
      spec.counted_key_idx   = detection->counted_key_idx;
      spec.slots.push_back(sirius::op::groupjoin::slot_spec{
        detection->counted_value_idx ? sirius::op::groupjoin::agg_op::COUNT_VALID
                                     : sirius::op::groupjoin::agg_op::COUNT_STAR,
        detection->counted_value_idx,
        types[1]});
      spec.max_state_bytes = op_params.dense_count_join_max_bytes;

      auto fused = duckdb::make_uniq<sirius::op::sirius_physical_group_join>(
        std::move(types), op.estimated_cardinality, std::move(spec));
      fused->children.push_back(std::move(preserved));
      fused->children.push_back(std::move(counted));
      return fused;
    }
  }

  // Rungs P1 and P2, in order, behind the shared value-pathway gate.
  if (op_params.enable_group_join) {
    if (auto fused = try_plan_inner_group_join(op)) { return fused; }
    if (auto fused = try_plan_direct_group_join(op)) { return fused; }
  }

  return nullptr;
}

duckdb::unique_ptr<sirius::op::sirius_physical_operator>
sirius_physical_plan_generator::try_plan_inner_group_join(duckdb::LogicalAggregate& op)
{
  auto sirius_ctx = context.registered_state
                      ? context.registered_state->Get<duckdb::SiriusContext>("sirius_state")
                      : nullptr;
  if (!sirius_ctx) { return nullptr; }
  const auto& op_params = sirius_ctx->get_config().get_operator_params();

  auto detection = detect_inner_value_group_join(context, op);
  if (!detection) { return nullptr; }
  // Detection validated the child chain: the join sits behind zero or more bare
  // column-selection projections, which the fusion replaces along with the aggregate.
  duckdb::LogicalOperator* join_node = op.children[0].get();
  while (join_node->type == duckdb::LogicalOperatorType::LOGICAL_PROJECTION) {
    join_node = join_node->children[0].get();
  }
  auto& join                      = join_node->Cast<duckdb::LogicalComparisonJoin>();
  const std::size_t counted_child = 1 - detection->preserved_child;

  // Counted-side plan-time byte gate, acting as the schedule selector. At or under the gate the
  // fused one-shot schedule colocates the whole counted side in one task reservation (today's
  // admission, wiring, and estimate verbatim). Over the gate that reservation could never be
  // scheduled, so the shape plans the streamed schedule instead -- when the INNER form is
  // stream-admitted and the plan-time proofs pass -- and declines to generic planning otherwise.
  // Estimate error is harmless in both directions here: it selects a schedule, not a reservation.
  const std::size_t counted_cardinality =
    join.children[counted_child]->EstimateCardinality(context);
  auto const counted_bytes = sirius::memory::saturating_mul(
    counted_cardinality,
    estimated_row_bytes(join.children[counted_child]->types, op_params.avg_variable_column_bytes));
  auto schedule                     = sirius::op::groupjoin::schedule_kind::ONE_SHOT;
  uint64_t stream_counted_row_bound = 0;
  if (counted_bytes > op_params.group_join_counted_bytes_gate) {
    auto const declined = [&](std::string_view reason) {
      SIRIUS_LOG_INFO(
        "[sirius_plan_aggregate] GROUP_JOIN INNER fusion declined: counted child estimate {} "
        "bytes ({} rows) exceeds the counted-side byte gate {} and the streamed schedule is "
        "inadmissible ({})",
        counted_bytes,
        counted_cardinality,
        op_params.group_join_counted_bytes_gate,
        reason);
      return nullptr;
    };
    if (!op_params.group_join_stream_inner) {
      return declined("INNER is outside group_join_stream_forms");
    }
    auto const admission = admit_group_join_inner_stream(context,
                                                         &sirius_ctx->get_scan_manager(),
                                                         *join.children[counted_child],
                                                         detection->counted_value_idx,
                                                         detection->slot_op);
    if (!admission.admitted) { return declined(admission.reason); }
    schedule                 = sirius::op::groupjoin::schedule_kind::STREAM;
    stream_counted_row_bound = admission.counted_row_bound;
  }

  // Dynamic-filter parity. The hash join this fusion replaces publishes membership filters from
  // its build side (children[1]) when that side carries filter or opaque-build evidence. When the
  // preserved side is children[1], the fused operator installs the equivalent preserved-key
  // publication below; when it is children[0], the replaced join's filters would flow from the
  // counted side, which the fused operator cannot reproduce -- fail closed rather than plan a
  // filter downgrade.
  bool const dynamic_filter_enabled = op_params.enable_dynamic_filter;
  bool const build_filtered =
    dynamic_filter_enabled && build_subtree_is_filtering(*join.children[1]);
  bool const build_opaque   = dynamic_filter_enabled && build_relation_is_opaque(*join.children[1]);
  bool const build_evidence = build_filtered || build_opaque;
  if (build_evidence && detection->preserved_child != 1) {
    SIRIUS_LOG_INFO(
      "[sirius_plan_aggregate] GROUP_JOIN INNER fusion declined: the replaced hash join would "
      "publish counted-side dynamic filters (evidence={}) that the fused operator cannot",
      build_filtered ? (build_opaque ? "filtered+opaque" : "filtered") : "opaque");
    return nullptr;
  }

  // Capture publication evidence before create_plan drains the logical children.
  auto condition_domains =
    build_evidence ? build_key_domain_cardinalities(join, duckdb_base_table_cardinality{context})
                   : std::vector<std::size_t>{};
  auto condition_shapes = classify_join_key_shapes(join.conditions);
  auto build_side_unique_cols =
    build_evidence ? prove_unique_columns(*join.children[1]) : std::unordered_set<duckdb::idx_t>{};

  // Mirror plan_comparison_join: capture cardinalities before create_plan drains the nodes.
  const std::size_t preserved_cardinality =
    join.children[detection->preserved_child]->EstimateCardinality(context);

  auto preserved                   = create_plan(*join.children[detection->preserved_child]);
  auto counted                     = create_plan(*join.children[counted_child]);
  preserved->estimated_cardinality = preserved_cardinality;
  counted->estimated_cardinality   = counted_cardinality;
  if (preserved->children.size() > 1 || counted->children.size() > 1) {
    throw sirius::internal_exception(
      "group_join: eligible logical child produced a non-unary physical root");
  }

  // Preserved-key membership publication (the P1 prerequisite): install the plan the replaced
  // hash join would have installed, targeting the counted subtree. Identical admission and
  // discovery inputs make an empty plan here parity with the replaced join, not a downgrade.
  sirius::op::dynamic_filter_publish_plan filter_plan;
  if (build_evidence) {
    auto const& cond = join.conditions[0];
    membership_publication_request request;
    request.join_type                    = join.join_type;
    request.condition.comparison         = sirius::comparison_type::equal;
    request.condition.left               = sirius::ast::from_duckdb(*cond.left);
    request.condition.right              = sirius::ast::from_duckdb(*cond.right);
    request.shape                        = condition_shapes[0];
    request.build_key_domain_cardinality = condition_domains.empty() ? 0 : condition_domains[0];
    request.build_side_unique_column =
      build_side_unique_cols.size() == 1
        ? std::optional<std::size_t>{static_cast<std::size_t>(*build_side_unique_cols.begin())}
        : std::nullopt;
    request.build_estimated_rows = preserved_cardinality;
    if (request.condition.left != nullptr && request.condition.right != nullptr) {
      filter_plan = plan_single_key_membership_publication(
        *sirius_ctx, op_params, std::move(request), counted, "sirius_plan_aggregate");
    }
  }

  SIRIUS_LOG_INFO(
    "[sirius_plan_aggregate] Fusing INNER {} into GROUP_JOIN ({} schedule): preserved child {} "
    "({}, key col {}, est {} rows), counted child {} (key col {}{}, est {} rows), membership "
    "publication {}",
    aggregate_op_display_name(detection->slot_op),
    schedule == sirius::op::groupjoin::schedule_kind::STREAM ? "STREAM" : "one-shot",
    detection->preserved_child,
    detection->preserved_is_delim ? "DELIM_GET" : "unique scan chain",
    detection->preserved_key_idx,
    preserved_cardinality,
    counted_child,
    detection->counted_key_idx,
    detection->counted_value_idx ? ", arg col " + std::to_string(*detection->counted_value_idx)
                                 : std::string(""),
    counted_cardinality,
    filter_plan.enabled() ? "installed" : "not wired");

  auto types = sirius::from_duckdb_vec(op.types);
  sirius::op::groupjoin::group_join_spec spec;
  spec.form              = sirius::op::groupjoin::join_form::INNER;
  spec.preserved_key_idx = detection->preserved_key_idx;
  spec.counted_key_idx   = detection->counted_key_idx;
  spec.slots.push_back(
    sirius::op::groupjoin::slot_spec{detection->slot_op, detection->counted_value_idx, types[1]});
  spec.max_state_bytes          = op_params.group_join_max_state_bytes;
  spec.schedule                 = schedule;
  spec.stream_counted_row_bound = stream_counted_row_bound;

  auto fused = duckdb::make_uniq<sirius::op::sirius_physical_group_join>(
    std::move(types),
    op.estimated_cardinality,
    std::move(spec),
    std::move(filter_plan),
    &sirius_ctx->get_dynamic_filter_stats());
  fused->children.push_back(std::move(preserved));
  fused->children.push_back(std::move(counted));
  return fused;
}

duckdb::unique_ptr<sirius::op::sirius_physical_operator>
sirius_physical_plan_generator::try_plan_direct_group_join(duckdb::LogicalAggregate& op)
{
  auto sirius_ctx = context.registered_state
                      ? context.registered_state->Get<duckdb::SiriusContext>("sirius_state")
                      : nullptr;
  if (!sirius_ctx) { return nullptr; }
  const auto& op_params = sirius_ctx->get_config().get_operator_params();

  auto detection = detect_direct_group_join(context, op);
  if (!detection) { return nullptr; }
  auto& child = *op.children[0];

  // Child plan-time byte gate, acting as the schedule selector (as in rung P1). An over-gate
  // DIRECT shape streams the sparse merge ladder -- committed at plan time, because a dense
  // DIRECT domain cannot be known before the stream ends -- and needs no admission proofs (the
  // sparse path is mask-exact with generic-parity int64 accumulation).
  const std::size_t child_cardinality = child.EstimateCardinality(context);
  auto const child_bytes              = sirius::memory::saturating_mul(
    child_cardinality, estimated_row_bytes(child.types, op_params.avg_variable_column_bytes));
  auto schedule = sirius::op::groupjoin::schedule_kind::ONE_SHOT;
  if (child_bytes > op_params.group_join_counted_bytes_gate) {
    if (!op_params.group_join_stream_direct) {
      SIRIUS_LOG_INFO(
        "[sirius_plan_aggregate] GROUP_JOIN DIRECT fusion declined: child estimate {} bytes "
        "({} rows) exceeds the counted-side byte gate {} and the streamed schedule is "
        "inadmissible (DIRECT is outside group_join_stream_forms)",
        child_bytes,
        child_cardinality,
        op_params.group_join_counted_bytes_gate);
      return nullptr;
    }
    schedule = sirius::op::groupjoin::schedule_kind::STREAM;
  }

  // Unlike rung P1, no dynamic-filter publication is installed: DIRECT replaces only the
  // aggregate. The join below stays in the plan -- create_plan wires its own publication as in
  // any unfused plan -- and the fused operator has no preserved side, so nothing the fusion
  // replaces ever published.
  auto counted                   = create_plan(child);
  counted->estimated_cardinality = child_cardinality;

  SIRIUS_LOG_INFO(
    "[sirius_plan_aggregate] Fusing DIRECT {} into GROUP_JOIN ({} schedule): group key col {}{}, "
    "opaque comparison-join child (est {} rows)",
    aggregate_op_display_name(detection->slot_op),
    schedule == sirius::op::groupjoin::schedule_kind::STREAM ? "STREAM" : "one-shot",
    detection->group_key_idx,
    detection->arg_idx ? ", arg col " + std::to_string(*detection->arg_idx) : std::string(""),
    child_cardinality);

  auto types = sirius::from_duckdb_vec(op.types);
  sirius::op::groupjoin::group_join_spec spec;
  spec.form = sirius::op::groupjoin::join_form::DIRECT;
  // DIRECT has no preserved side; the group key rides the counted-key slot of the spec.
  spec.preserved_key_idx = detection->group_key_idx;
  spec.counted_key_idx   = detection->group_key_idx;
  spec.slots.push_back(
    sirius::op::groupjoin::slot_spec{detection->slot_op, detection->arg_idx, types[1]});
  spec.max_state_bytes = op_params.group_join_max_state_bytes;
  spec.schedule        = schedule;

  auto fused = duckdb::make_uniq<sirius::op::sirius_physical_group_join>(
    std::move(types), op.estimated_cardinality, std::move(spec));
  fused->children.push_back(std::move(counted));
  return fused;
}

namespace {

static uint32_t required_bits_for_value(uint32_t n)
{
  std::size_t required_bits = 0;
  while (n > 0) {
    n >>= 1;
    required_bits++;
  }
  return duckdb::UnsafeNumericCast<uint32_t>(required_bits);
}

template <class T>
duckdb::hugeint_t get_range_hugeint(const duckdb::BaseStatistics& nstats)
{
  return duckdb::Hugeint::Convert(duckdb::NumericStats::GetMax<T>(nstats)) -
         duckdb::Hugeint::Convert(duckdb::NumericStats::GetMin<T>(nstats));
}

static bool can_use_partitioned_aggregate(duckdb::ClientContext& context,
                                          duckdb::LogicalAggregate& op,
                                          sirius::op::sirius_physical_operator& child,
                                          duckdb::vector<duckdb::column_t>& partition_columns)
{
  if (op.grouping_sets.size() > 1 || !op.grouping_functions.empty()) { return false; }
  // check if the source is partitioned by the aggregate columns
  // figure out the columns we are grouping by
  for (auto& group_expr : op.groups) {
    // only support bound reference here
    if (group_expr->GetExpressionType() != duckdb::ExpressionType::BOUND_REF) { return false; }
    auto& ref = group_expr->Cast<duckdb::BoundReferenceExpression>();
    partition_columns.push_back(ref.index);
  }
  // traverse the children of the aggregate to find the source operator
  duckdb::reference<sirius::op::sirius_physical_operator> child_ref(child);
  while (child_ref.get().type != sirius::op::SiriusPhysicalOperatorType::TABLE_SCAN) {
    auto& child_op = child_ref.get();
    switch (child_op.type) {
      case sirius::op::SiriusPhysicalOperatorType::PROJECTION: {
        // recompute partition columns
        auto& projection = child_op.Cast<sirius::op::sirius_physical_projection>();
        duckdb::vector<duckdb::column_t> new_columns;
        for (auto& partition_col : partition_columns) {
          // we only support bound reference here
          auto const* expr = projection.select_list[partition_col].get();
          if (expr == nullptr) { return false; }
          if (!expr->holds<sirius::ast::reference>()) { return false; }
          new_columns.push_back(expr->get<sirius::ast::reference>().column_index);
        }
        // continue into child node with new columns
        partition_columns = std::move(new_columns);
        child_ref         = *child_op.children[0];
        break;
      }
      case sirius::op::SiriusPhysicalOperatorType::FILTER:
        // continue into child operators
        child_ref = *child_op.children[0];
        break;
      default:
        // unsupported operator for partition pass-through
        return false;
    }
  }
  auto& table_scan = child_ref.get().Cast<sirius::op::sirius_physical_table_scan>();
  if (!table_scan.function.get_partition_info) {
    // this source does not expose partition information - skip
    return false;
  }
  // get the base columns by projecting over the projection_ids/column_ids
  if (!table_scan.projection_ids.empty()) {
    for (auto& partition_col : partition_columns) {
      if (partition_col >= table_scan.projection_ids.size()) { return false; }
      partition_col = table_scan.projection_ids[partition_col];
    }
  }
  duckdb::vector<duckdb::column_t> base_columns;
  for (const auto& partition_idx : partition_columns) {
    auto col_idx = partition_idx;
    col_idx      = table_scan.column_ids[col_idx].GetPrimaryIndex();
    base_columns.push_back(col_idx);
  }
  // check if the source operator is partitioned by the grouping columns
  duckdb::TableFunctionPartitionInput input(table_scan.bind_data.get(), base_columns);
  auto partition_info = table_scan.function.get_partition_info(context, input);
  if (partition_info != duckdb::TablePartitionInfo::SINGLE_VALUE_PARTITIONS) {
    // we only support single-value partitions currently
    return false;
  }
  // we have single value partitions!
  return true;
}

static bool can_use_perfect_hash_aggregate(duckdb::ClientContext& context,
                                           duckdb::LogicalAggregate& op,
                                           duckdb::vector<std::size_t>& bits_per_group)
{
  if (op.grouping_sets.size() > 1 || !op.grouping_functions.empty()) { return false; }
  std::size_t perfect_hash_bits = 0;
  for (std::size_t group_idx = 0; group_idx < op.groups.size(); group_idx++) {
    auto& group = op.groups[group_idx];
    auto& stats = op.group_stats[group_idx];

    switch (group->return_type.InternalType()) {
      case duckdb::PhysicalType::INT8:
      case duckdb::PhysicalType::INT16:
      case duckdb::PhysicalType::INT32:
      case duckdb::PhysicalType::INT64:
      case duckdb::PhysicalType::UINT8:
      case duckdb::PhysicalType::UINT16:
      case duckdb::PhysicalType::UINT32:
      case duckdb::PhysicalType::UINT64: break;
      default:
        // we only support simple integer types for perfect hashing
        return false;
    }
    // check if the group has stats available
    auto& group_type = group->return_type;
    if (!stats) {
      // no stats, but we might still be able to use perfect hashing if the type is small enough
      // for small types we can just set the stats to [type_min, type_max]
      switch (group_type.InternalType()) {
        case duckdb::PhysicalType::INT8:
        case duckdb::PhysicalType::INT16:
        case duckdb::PhysicalType::UINT8:
        case duckdb::PhysicalType::UINT16: break;
        default:
          // type is too large and there are no stats: skip perfect hashing
          return false;
      }
      // construct stats with the min and max value of the type
      stats = duckdb::NumericStats::CreateUnknown(group_type).ToUnique();
      duckdb::NumericStats::SetMin(*stats, duckdb::Value::MinimumValue(group_type));
      duckdb::NumericStats::SetMax(*stats, duckdb::Value::MaximumValue(group_type));
    }
    auto& nstats = *stats;

    if (!duckdb::NumericStats::HasMinMax(nstats)) { return false; }

    if (duckdb::NumericStats::Max(*stats) < duckdb::NumericStats::Min(*stats)) {
      // May result in underflow
      return false;
    }

    // we have a min and a max value for the stats: use that to figure out how many bits we have
    // we add two here, one for the NULL value, and one to make the computation one-indexed
    // (e.g. if min and max are the same, we still need one entry in total)
    duckdb::hugeint_t range_h;
    switch (group_type.InternalType()) {
      case duckdb::PhysicalType::INT8: range_h = get_range_hugeint<int8_t>(nstats); break;
      case duckdb::PhysicalType::INT16: range_h = get_range_hugeint<int16_t>(nstats); break;
      case duckdb::PhysicalType::INT32: range_h = get_range_hugeint<int32_t>(nstats); break;
      case duckdb::PhysicalType::INT64: range_h = get_range_hugeint<int64_t>(nstats); break;
      case duckdb::PhysicalType::UINT8: range_h = get_range_hugeint<uint8_t>(nstats); break;
      case duckdb::PhysicalType::UINT16: range_h = get_range_hugeint<uint16_t>(nstats); break;
      case duckdb::PhysicalType::UINT32: range_h = get_range_hugeint<uint32_t>(nstats); break;
      case duckdb::PhysicalType::UINT64: range_h = get_range_hugeint<uint64_t>(nstats); break;
      default:
        throw duckdb::InternalException(
          "Unsupported type for perfect hash (should be caught before)");
    }

    uint64_t range;
    if (!duckdb::Hugeint::TryCast(range_h, range)) { return false; }

    // bail out on any range bigger than 2^32
    if (range >= duckdb::NumericLimits<int32_t>::Maximum()) { return false; }

    range += 2;
    // figure out how many bits we need
    std::size_t required_bits = required_bits_for_value(duckdb::UnsafeNumericCast<uint32_t>(range));
    bits_per_group.push_back(required_bits);
    perfect_hash_bits += required_bits;
    // check if we have exceeded the bits for the hash
    if (perfect_hash_bits > duckdb::Settings::Get<duckdb::PerfectHtThresholdSetting>(context)) {
      // too many bits for perfect hash
      return false;
    }
  }
  for (auto& expression : op.expressions) {
    auto& aggregate = expression->Cast<duckdb::BoundAggregateExpression>();
    if (aggregate.IsDistinct() || !aggregate.function.combine) {
      // distinct aggregates are not supported in perfect hash aggregates
      return false;
    }
  }
  return true;
}

/// cuDF does not support HUGEINT (int128). DuckDB widens aggregates like sum(int32) to HUGEINT
/// to avoid overflow. We downcast to BIGINT at the plan level so all downstream operators
/// (including the result collector) use the correct type.
static void downcast_hugeint_types(duckdb::vector<duckdb::LogicalType>& types,
                                   duckdb::vector<duckdb::unique_ptr<duckdb::Expression>>& exprs)
{
  for (auto& type : types) {
    if (type == duckdb::LogicalType::HUGEINT) { type = duckdb::LogicalType::BIGINT; }
  }
  for (auto& expr : exprs) {
    if (expr->return_type == duckdb::LogicalType::HUGEINT) {
      expr->return_type = duckdb::LogicalType::BIGINT;
    }
  }
}

}  // namespace

duckdb::unique_ptr<sirius::op::sirius_physical_operator>
sirius_physical_plan_generator::create_plan(duckdb::LogicalAggregate& op)
{
  D_ASSERT(op.children.size() == 1);

  // Downcast HUGEINT to BIGINT since cuDF does not support int128
  downcast_hugeint_types(op.types, op.expressions);

  // Reject nested GROUP BY keys before extract_aggregate_expressions rewrites
  // groups into bare references (which lose the name needed for the error).
  for (auto const& group : op.groups) {
    reject_nested_column_operation(*group, "GROUP BY");
  }

  if (auto fused = try_plan_group_join(op)) { return fused; }

  auto plan = create_plan(*op.children[0]);

  plan = extract_aggregate_expressions(
    context, std::move(plan), op.expressions, op.groups, op.grouping_sets);
  bool can_use_simple_aggregation = true;
  for (auto& expression : op.expressions) {
    auto& aggregate = expression->Cast<duckdb::BoundAggregateExpression>();
    if (!aggregate.function.simple_update) {
      // unsupported aggregate for simple aggregation: use hash aggregation
      can_use_simple_aggregation = false;
      break;
    }
  }

  // Check if all groups are valid
  if (op.group_stats.empty()) { op.group_stats.resize(op.groups.size()); }
  auto group_validity = duckdb::TupleDataValidityType::CANNOT_HAVE_NULL_VALUES;
  for (const auto& stats : op.group_stats) {
    if (stats && !stats->CanHaveNull()) { continue; }
    group_validity = duckdb::TupleDataValidityType::CAN_HAVE_NULL_VALUES;
    break;
  }

  if (op.groups.empty() && op.grouping_sets.size() <= 1) {
    // no groups, check if we can use a simple aggregation
    // special case: aggregate entire columns together
    if (can_use_simple_aggregation) {
      auto group_by = duckdb::make_uniq_base<sirius::op::sirius_physical_operator,
                                             sirius::op::sirius_physical_ungrouped_aggregate>(
        sirius::from_duckdb_vec(op.types),
        translate_expressions(std::move(op.expressions)),
        op.estimated_cardinality,
        op.distinct_validity);
      group_by->children.push_back(std::move(plan));
      return group_by;
    }
    throw duckdb::NotImplementedException("Non simple aggregation is not supported");
  }

  // groups! create a GROUP BY aggregator
  // use a partitioned or perfect hash aggregate if possible
  duckdb::vector<duckdb::column_t> partition_columns;
  duckdb::vector<std::size_t> required_bits;
  if (can_use_simple_aggregation &&
      can_use_partitioned_aggregate(context, op, *plan, partition_columns)) {
    auto group_by = duckdb::make_uniq_base<sirius::op::sirius_physical_operator,
                                           sirius::op::sirius_physical_grouped_aggregate>(
      sirius::from_duckdb_vec(op.types),
      translate_expressions(std::move(op.expressions)),
      translate_expressions(std::move(op.groups)),
      std::move(op.grouping_sets),
      std::move(op.grouping_functions),
      op.estimated_cardinality,
      group_validity,
      op.distinct_validity);
    group_by->children.push_back(std::move(plan));
    return group_by;
  }

  if (can_use_perfect_hash_aggregate(context, op, required_bits)) {
    auto group_by = duckdb::make_uniq_base<sirius::op::sirius_physical_operator,
                                           sirius::op::sirius_physical_grouped_aggregate>(
      sirius::from_duckdb_vec(op.types),
      translate_expressions(std::move(op.expressions)),
      translate_expressions(std::move(op.groups)),
      std::move(op.grouping_sets),
      std::move(op.grouping_functions),
      op.estimated_cardinality,
      group_validity,
      op.distinct_validity);
    group_by->children.push_back(std::move(plan));
    return group_by;
  }

  auto group_by = duckdb::make_uniq_base<sirius::op::sirius_physical_operator,
                                         sirius::op::sirius_physical_grouped_aggregate>(
    sirius::from_duckdb_vec(op.types),
    translate_expressions(std::move(op.expressions)),
    translate_expressions(std::move(op.groups)),
    std::move(op.grouping_sets),
    std::move(op.grouping_functions),
    op.estimated_cardinality,
    group_validity,
    op.distinct_validity);
  group_by->children.push_back(std::move(plan));
  return group_by;
}

}  // namespace sirius::planner
