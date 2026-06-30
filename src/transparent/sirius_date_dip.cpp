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

#include "transparent/sirius_date_dip.hpp"

#include "log/logging.hpp"

#include <duckdb/catalog/catalog.hpp>
#include <duckdb/catalog/catalog_entry/table_catalog_entry.hpp>
#include <duckdb/common/column_index.hpp>
#include <duckdb/common/enums/expression_type.hpp>
#include <duckdb/common/enums/join_type.hpp>
#include <duckdb/common/enums/logical_operator_type.hpp>
#include <duckdb/common/types.hpp>
#include <duckdb/common/types/value.hpp>
#include <duckdb/main/attached_database.hpp>
#include <duckdb/planner/expression/bound_columnref_expression.hpp>
#include <duckdb/planner/filter/conjunction_filter.hpp>
#include <duckdb/planner/filter/constant_filter.hpp>
#include <duckdb/planner/filter/optional_filter.hpp>
#include <duckdb/planner/operator/logical_comparison_join.hpp>
#include <duckdb/planner/operator/logical_get.hpp>
#include <duckdb/storage/single_file_block_manager.hpp>
#include <duckdb/storage/storage_manager.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>

namespace sirius::transparent {

//===----------------------------------------------------------------------===//
// Pure core
//===----------------------------------------------------------------------===//

namespace {

void accumulate_date_bounds(const duckdb::TableFilter& f, derived_date_window& w)
{
  switch (f.filter_type) {
    case duckdb::TableFilterType::CONSTANT_COMPARISON: {
      auto const& cf = f.Cast<duckdb::ConstantFilter>();
      if (cf.constant.IsNull() || cf.constant.type().id() != duckdb::LogicalTypeId::DATE) {
        return;
      }
      auto const v = cf.constant.GetValue<duckdb::date_t>();
      switch (cf.comparison_type) {
        case duckdb::ExpressionType::COMPARE_GREATERTHAN:
        case duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO:
          if (!w.lo.has_value() || v.days > w.lo->days) { w.lo = v; }
          break;
        case duckdb::ExpressionType::COMPARE_LESSTHAN:
        case duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO:
          if (!w.hi.has_value() || v.days < w.hi->days) { w.hi = v; }
          break;
        case duckdb::ExpressionType::COMPARE_EQUAL:
          w.lo = v;
          w.hi = v;
          break;
        default: break;
      }
      break;
    }
    case duckdb::TableFilterType::CONJUNCTION_AND: {
      auto const& cj = f.Cast<duckdb::ConjunctionAndFilter>();
      for (auto const& c : cj.child_filters) {
        accumulate_date_bounds(*c, w);
      }
      break;
    }
    case duckdb::TableFilterType::OPTIONAL_FILTER: {
      auto const& of = f.Cast<duckdb::OptionalFilter>();
      if (of.child_filter) { accumulate_date_bounds(*of.child_filter, w); }
      break;
    }
    default: break;
  }
}

}  // namespace

derived_date_window extract_date_window(const duckdb::TableFilter& filter)
{
  derived_date_window w;
  accumulate_date_bounds(filter, w);
  return w;
}

derived_date_window derive_forward_window(const derived_date_window& dim_window,
                                          const date_correlation& correlation)
{
  // Lag-histogram path: with a two-sided dim window we apply only the lag of the buckets
  // the query overlaps, keeping the bound tight when the lag is non-stationary. A
  // one-sided window can't be intersected against buckets, so it uses the global lag.
  if (!correlation.buckets.empty() && dim_window.lo.has_value() && dim_window.hi.has_value()) {
    const std::int32_t dlo = dim_window.lo->days;
    const std::int32_t dhi = dim_window.hi->days;
    std::optional<std::int32_t> lag_lo;
    std::optional<std::int32_t> lag_hi;
    for (const auto& b : correlation.buckets) {
      if (b.dim_hi_day < dlo || b.dim_lo_day > dhi) { continue; }  // bucket disjoint from query
      lag_lo = lag_lo.has_value() ? std::min(*lag_lo, b.lag_lo_days) : b.lag_lo_days;
      lag_hi = lag_hi.has_value() ? std::max(*lag_hi, b.lag_hi_days) : b.lag_hi_days;
    }
    if (lag_lo.has_value()) {
      derived_date_window out;
      out.lo = duckdb::date_t(dlo + *lag_lo);
      out.hi = duckdb::date_t(dhi + *lag_hi);
      return out;
    }
    // No bucket overlapped the query window — fall through to the global lag.
  }

  // Global-lag path (the 1-bucket case, and one-sided dim windows).
  derived_date_window out;
  if (dim_window.lo.has_value()) {
    out.lo = duckdb::date_t(dim_window.lo->days + correlation.lag_lo_days);
  }
  if (dim_window.hi.has_value()) {
    out.hi = duckdb::date_t(dim_window.hi->days + correlation.lag_hi_days);
  }
  return out;
}

duckdb::unique_ptr<duckdb::TableFilter> make_prune_only_date_filter(
  const derived_date_window& window)
{
  if (window.empty()) { return nullptr; }
  if (window.lo.has_value() && window.hi.has_value() && window.lo->days > window.hi->days) {
    return nullptr;  // degenerate empty interval
  }

  duckdb::vector<duckdb::unique_ptr<duckdb::TableFilter>> bounds;
  if (window.lo.has_value()) {
    bounds.push_back(duckdb::make_uniq<duckdb::ConstantFilter>(
      duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO, duckdb::Value::DATE(*window.lo)));
  }
  if (window.hi.has_value()) {
    bounds.push_back(duckdb::make_uniq<duckdb::ConstantFilter>(
      duckdb::ExpressionType::COMPARE_LESSTHANOREQUALTO, duckdb::Value::DATE(*window.hi)));
  }

  duckdb::unique_ptr<duckdb::TableFilter> inner;
  if (bounds.size() == 1) {
    inner = std::move(bounds[0]);
  } else {
    auto conj = duckdb::make_uniq<duckdb::ConjunctionAndFilter>();
    for (auto& b : bounds) {
      conj->child_filters.push_back(std::move(b));
    }
    inner = std::move(conj);
  }
  // OptionalFilter at the top level => row-level no-op (skipped by
  // convert_table_filters_to_expression) but honored by the row-group pruner.
  return duckdb::make_uniq<duckdb::OptionalFilter>(std::move(inner));
}

std::optional<db_write_token> read_db_write_token(duckdb::TableCatalogEntry& table)
{
  auto& storage = table.ParentCatalog().GetAttached().GetStorageManager();
  if (storage.InMemory()) { return std::nullopt; }  // no durable iteration => fail closed
  auto& bm = storage.GetBlockManager().Cast<duckdb::SingleFileBlockManager>();
  return db_write_token{bm.GetCheckpointIteration(), storage.GetWALSize()};
}

//===----------------------------------------------------------------------===//
// The pass
//===----------------------------------------------------------------------===//

namespace {

// A bound plan has a unique table_index per base-table GET, so keying by it is safe.
using get_map       = std::unordered_map<duckdb::idx_t, duckdb::LogicalGet*>;
using eq_bindings_t = std::vector<std::pair<duckdb::ColumnBinding, duckdb::ColumnBinding>>;

void collect_gets(duckdb::LogicalOperator& op, get_map& out)
{
  if (op.type == duckdb::LogicalOperatorType::LOGICAL_GET) {
    auto& g            = op.Cast<duckdb::LogicalGet>();
    out[g.table_index] = &g;
  }
  for (auto& child : op.children) {
    collect_gets(*child, out);
  }
}

// Only INNER and SEMI joins are sound to prune the fact side through: for OUTER /
// ANTI / MARK joins, unmatched fact rows can survive into the result, so removing
// a fact row group could change the answer.
bool is_prunable_join_type(duckdb::JoinType t)
{
  return t == duckdb::JoinType::INNER || t == duckdb::JoinType::SEMI;
}

void collect_equal_join_bindings(duckdb::LogicalOperator& op, eq_bindings_t& out)
{
  if (op.type == duckdb::LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
    auto& join = op.Cast<duckdb::LogicalComparisonJoin>();
    if (is_prunable_join_type(join.join_type)) {
      for (auto& cond : join.conditions) {
        if (cond.comparison != duckdb::ExpressionType::COMPARE_EQUAL) { continue; }
        if (cond.left->type != duckdb::ExpressionType::BOUND_COLUMN_REF ||
            cond.right->type != duckdb::ExpressionType::BOUND_COLUMN_REF) {
          continue;
        }
        const auto& l = cond.left->Cast<duckdb::BoundColumnRefExpression>();
        const auto& r = cond.right->Cast<duckdb::BoundColumnRefExpression>();
        out.emplace_back(l.binding, r.binding);
      }
    }
  }
  for (auto& child : op.children) {
    collect_equal_join_bindings(*child, out);
  }
}

// Storage primary index of a named column on a base-table GET (index into the
// table's full column list, which is how table_filters and ColumnIndex are keyed).
std::optional<duckdb::idx_t> storage_index_of(duckdb::LogicalGet& g, const std::string& col)
{
  for (duckdb::idx_t i = 0; i < g.names.size(); ++i) {
    if (g.names[i] == col) { return i; }
  }
  return std::nullopt;
}

std::optional<std::string> base_table_name(duckdb::LogicalGet& g)
{
  auto tbl = g.GetTable();
  if (!tbl) { return std::nullopt; }
  return tbl->name;
}

// Resolve a join-condition column binding to (table_index, column_name) of its
// source base-table GET, or nullopt if it does not point directly at one.
std::optional<std::pair<duckdb::idx_t, std::string>> resolve_binding(
  const get_map& gets, const duckdb::ColumnBinding& binding)
{
  auto it = gets.find(binding.table_index);
  if (it == gets.end()) { return std::nullopt; }
  auto& g             = *it->second;
  const auto& col_ids = g.GetColumnIds();
  if (binding.column_index >= col_ids.size()) { return std::nullopt; }
  const auto storage = col_ids[binding.column_index].GetPrimaryIndex();
  if (storage >= g.names.size()) { return std::nullopt; }
  return std::make_pair(binding.table_index, g.names[storage]);
}

// Is there an equi-join condition directly linking dim.dim_key_col to
// fact.fact_key_col (in either order)?
bool fk_linked(const get_map& gets,
               const eq_bindings_t& eq,
               duckdb::idx_t dim_ti,
               const std::string& dim_key_col,
               duckdb::idx_t fact_ti,
               const std::string& fact_key_col)
{
  for (const auto& [lb, rb] : eq) {
    auto l = resolve_binding(gets, lb);
    auto r = resolve_binding(gets, rb);
    if (!l.has_value() || !r.has_value()) { continue; }
    const bool forward = l->first == dim_ti && l->second == dim_key_col && r->first == fact_ti &&
                         r->second == fact_key_col;
    const bool reverse = r->first == dim_ti && r->second == dim_key_col && l->first == fact_ti &&
                         l->second == fact_key_col;
    if (forward || reverse) { return true; }
  }
  return false;
}

void ensure_column_scanned(duckdb::LogicalGet& g, duckdb::idx_t storage)
{
  for (const auto& ci : g.GetColumnIds()) {
    if (ci.GetPrimaryIndex() == storage) { return; }
  }
  // Required: create_table_filter_set (sirius_plan_get.cpp) throws to CPU if a
  // filtered column is absent from column_ids. Appending keeps existing output
  // bindings stable (the new column is read but takes a fresh trailing index).
  g.AddColumnId(storage);
}

// If `dim_get` is `correlation`'s dimension table and carries a pushed-down filter on
// its date column, return the seeded dimension date window; nullopt otherwise.
std::optional<derived_date_window> match_dimension(const date_correlation& correlation,
                                                   duckdb::LogicalGet& dim_get)
{
  auto name = base_table_name(dim_get);
  if (!name.has_value() || *name != correlation.dim_table) { return std::nullopt; }

  auto date_storage = storage_index_of(dim_get, correlation.dim_date_col);
  if (!date_storage.has_value()) { return std::nullopt; }

  auto dim_filter = dim_get.table_filters.filters.find(*date_storage);
  if (dim_filter == dim_get.table_filters.filters.end()) { return std::nullopt; }

  auto window = extract_date_window(*dim_filter->second);
  if (window.empty()) { return std::nullopt; }
  return window;
}

// Try to inject the derived prune-only filter onto `fact_get` for `correlation`, given
// the matched dimension `dim_window` over the join `dim_ti -- fact_ti`. Returns true iff
// a filter was injected. See apply_date_dips' header for the gating preconditions.
bool try_inject_fact(duckdb::LogicalGet& fact_get,
                     const date_correlation& correlation,
                     const derived_date_window& dim_window,
                     const get_map& gets,
                     const eq_bindings_t& eq_bindings,
                     duckdb::idx_t dim_ti,
                     duckdb::idx_t fact_ti)
{
  auto name = base_table_name(fact_get);
  if (!name.has_value() || *name != correlation.fact_table) { return false; }

  auto date_storage = storage_index_of(fact_get, correlation.fact_date_col);
  if (!date_storage.has_value()) { return false; }

  // Only fire when the fact date column is unfiltered, so the injected OptionalFilter
  // stays the top-level filter on that column (hence a row-level no-op).
  if (fact_get.table_filters.filters.count(*date_storage) != 0) { return false; }

  if (!fk_linked(
        gets, eq_bindings, dim_ti, correlation.dim_key_col, fact_ti, correlation.fact_key_col)) {
    return false;
  }

  // Staleness gate: inject only if the fact table's database is unchanged since the
  // correlation was measured, and the dim table shares that database (so one DB-wide token
  // covers writes to both). Any uncertainty (stale token, unverifiable, cross-database) =>
  // skip the DIP. This is a positive-confirmation gate: every other path fails closed.
  auto dim_tbl  = gets.at(dim_ti)->GetTable();
  auto fact_tbl = fact_get.GetTable();
  if (!dim_tbl || !fact_tbl) { return false; }
  if (&dim_tbl->ParentCatalog().GetAttached() != &fact_tbl->ParentCatalog().GetAttached()) {
    return false;
  }
  auto current = read_db_write_token(*fact_tbl);
  if (!current.has_value() ||
      current->checkpoint_iteration != correlation.snapshot_checkpoint_iteration ||
      current->wal_size != correlation.snapshot_wal_size) {
    return false;
  }

  auto filter = make_prune_only_date_filter(derive_forward_window(dim_window, correlation));
  if (!filter) { return false; }

  ensure_column_scanned(fact_get, *date_storage);
  fact_get.table_filters.PushFilter(duckdb::ColumnIndex(*date_storage), std::move(filter));
  return true;
}

}  // namespace

std::size_t apply_date_dips(duckdb::LogicalOperator& plan,
                            const std::vector<date_correlation>& correlations)
{
  get_map gets;
  collect_gets(plan, gets);
  if (gets.empty()) { return 0; }

  eq_bindings_t eq_bindings;
  collect_equal_join_bindings(plan, eq_bindings);
  if (eq_bindings.empty()) { return 0; }

  std::size_t injected = 0;
  for (const auto& correlation : correlations) {
    for (auto& [dim_ti, dim_get] : gets) {
      auto dim_window = match_dimension(correlation, *dim_get);
      if (!dim_window.has_value()) { continue; }

      for (auto& [fact_ti, fact_get] : gets) {
        if (fact_ti == dim_ti) { continue; }
        if (!try_inject_fact(
              *fact_get, correlation, *dim_window, gets, eq_bindings, dim_ti, fact_ti)) {
          continue;
        }
        ++injected;
        SIRIUS_LOG_DEBUG("[date_dip] {}.{} -> {}.{} derived prune-only range",
                         correlation.dim_table,
                         correlation.dim_date_col,
                         correlation.fact_table,
                         correlation.fact_date_col);
      }
    }
  }
  return injected;
}

}  // namespace sirius::transparent
