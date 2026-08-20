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

/**
 * @file test_filter_cascade_policy_propagation.cpp
 * @brief Verifies that the connection-local filter-cascade setting is snapshotted into every
 *        filter and scan planning boundary during Sirius plan generation.
 */

#include "config.hpp"
#include "expression_evaluator/filter_cascade_policy.hpp"
#include "op/scan/sirius_gpu_scan_operator.hpp"
#include "op/sirius_physical_filter.hpp"
#include "op/sirius_physical_table_scan.hpp"
#include "planner/sirius_physical_plan_generator.hpp"

#include <catch.hpp>
#include <duckdb.hpp>
#include <duckdb/execution/column_binding_resolver.hpp>
#include <duckdb/main/config.hpp>
#include <duckdb/optimizer/optimizer.hpp>
#include <duckdb/parser/parser.hpp>
#include <duckdb/planner/expression/bound_constant_expression.hpp>
#include <duckdb/planner/filter/constant_filter.hpp>
#include <duckdb/planner/operator/logical_dummy_scan.hpp>
#include <duckdb/planner/operator/logical_filter.hpp>
#include <duckdb/planner/operator/logical_get.hpp>
#include <duckdb/planner/planner.hpp>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using sirius::op::sirius_physical_operator;
using sirius::op::SiriusPhysicalOperatorType;

class scoped_sirius_disable {
 public:
  scoped_sirius_disable()
  {
    if (auto const* value = ::getenv("SIRIUS_DISABLE")) { previous_ = value; }
    ::setenv("SIRIUS_DISABLE", "1", 1);
  }

  ~scoped_sirius_disable()
  {
    if (previous_) {
      ::setenv("SIRIUS_DISABLE", previous_->c_str(), 1);
    } else {
      ::unsetenv("SIRIUS_DISABLE");
    }
  }

  scoped_sirius_disable(scoped_sirius_disable const&)            = delete;
  scoped_sirius_disable& operator=(scoped_sirius_disable const&) = delete;

 private:
  std::optional<std::string> previous_;
};

class scoped_temp_db_path {
 public:
  scoped_temp_db_path()
  {
    char tmpl[]  = "/tmp/sirius_filter_cascade_policy_XXXXXX";
    int const fd = ::mkstemp(tmpl);
    REQUIRE(fd >= 0);
    ::close(fd);
    ::unlink(tmpl);
    path_ = tmpl;
  }

  ~scoped_temp_db_path()
  {
    std::remove(path_.c_str());
    std::remove((path_ + ".wal").c_str());
  }

  scoped_temp_db_path(scoped_temp_db_path const&)            = delete;
  scoped_temp_db_path& operator=(scoped_temp_db_path const&) = delete;

  [[nodiscard]] std::string const& path() const { return path_; }

 private:
  std::string path_;
};

void require_success(duckdb::unique_ptr<duckdb::MaterializedQueryResult> result)
{
  REQUIRE(result != nullptr);
  if (result->HasError()) { UNSCOPED_INFO(result->GetError()); }
  REQUIRE_FALSE(result->HasError());
}

template <typename Operator>
Operator& require_only_operator(sirius_physical_operator& root,
                                SiriusPhysicalOperatorType expected_type)
{
  std::vector<sirius_physical_operator*> matches;
  auto visit = [&](auto const& self, sirius_physical_operator& op) -> void {
    if (op.type == expected_type) { matches.push_back(&op); }
    for (auto& child : op.children) {
      self(self, *child);
    }
  };
  visit(visit, root);
  REQUIRE(matches.size() == 1);
  return matches.front()->Cast<Operator>();
}

// Expose the recursive logical-to-physical conversion without the post-pass that replaces a
// TABLE_SCAN. This makes the LogicalGet-only branch independently observable; the production
// post-pass is exercised separately below and checked through GPU_SCAN::table_info().
class inspectable_plan_generator : public sirius::planner::sirius_physical_plan_generator {
 public:
  using sirius_physical_plan_generator::sirius_physical_plan_generator;

  duckdb::unique_ptr<sirius_physical_operator> create_before_pipeline_rewrite(
    duckdb::unique_ptr<duckdb::LogicalOperator> logical)
  {
    return sirius_physical_plan_generator::create_plan(*logical);
  }
};

duckdb::unique_ptr<sirius_physical_operator> make_logical_filter_plan(
  duckdb::ClientContext& context)
{
  auto filter = duckdb::make_uniq<duckdb::LogicalFilter>(
    duckdb::make_uniq<duckdb::BoundConstantExpression>(duckdb::Value::BOOLEAN(true)));
  filter->children.push_back(duckdb::make_uniq<duckdb::LogicalDummyScan>(0));
  filter->SetEstimatedCardinality(1);
  filter->ResolveOperatorTypes();

  sirius::planner::sirius_physical_plan_generator generator(context);
  return generator.create_plan(std::move(filter));
}

duckdb::unique_ptr<sirius_physical_operator> make_logical_get_filter_plan(
  duckdb::ClientContext& context)
{
  duckdb::TableFunction function("parquet_scan", {}, nullptr, nullptr);
  function.projection_pushdown    = true;
  function.filter_pushdown        = true;
  function.supports_pushdown_type = [](duckdb::FunctionData const&, duckdb::idx_t) {
    return false;
  };

  duckdb::vector<duckdb::LogicalType> returned_types{duckdb::LogicalType::INTEGER};
  duckdb::vector<std::string> names{"value"};
  auto get = duckdb::make_uniq<duckdb::LogicalGet>(0,
                                                   std::move(function),
                                                   duckdb::make_uniq<duckdb::TableFunctionData>(),
                                                   std::move(returned_types),
                                                   std::move(names));
  duckdb::vector<duckdb::ColumnIndex> column_ids{duckdb::ColumnIndex(0)};
  get->SetColumnIds(std::move(column_ids));
  get->projection_ids.push_back(0);
  get->table_filters.PushFilter(
    duckdb::ColumnIndex(0),
    duckdb::make_uniq<duckdb::ConstantFilter>(duckdb::ExpressionType::COMPARE_GREATERTHAN,
                                              duckdb::Value::INTEGER(0)));
  get->SetEstimatedCardinality(1);
  get->ResolveOperatorTypes();

  inspectable_plan_generator generator(context);
  return generator.create_before_pipeline_rewrite(std::move(get));
}

duckdb::unique_ptr<sirius_physical_operator> make_native_gpu_scan_plan(duckdb::Connection& con)
{
  auto& context                = *con.context;
  auto const original_disabled = duckdb::DBConfig::GetConfig(context).options.disabled_optimizers;
  auto& disabled               = duckdb::DBConfig::GetConfig(context).options.disabled_optimizers;
  disabled.insert(duckdb::OptimizerType::IN_CLAUSE);
  disabled.insert(duckdb::OptimizerType::COMPRESSED_MATERIALIZATION);

  require_success(con.Query("BEGIN TRANSACTION"));
  duckdb::unique_ptr<sirius_physical_operator> result;
  try {
    duckdb::Parser parser(context.GetParserOptions());
    parser.ParseQuery("SELECT value FROM cascade_policy_source");
    REQUIRE(parser.statements.size() == 1);

    duckdb::Planner planner(context);
    planner.CreatePlan(std::move(parser.statements[0]));
    REQUIRE(planner.plan != nullptr);
    auto logical = std::move(planner.plan);

    if (context.config.enable_optimizer) {
      duckdb::Optimizer optimizer(*planner.binder, context);
      logical = optimizer.Optimize(std::move(logical));
    }

    logical->ResolveOperatorTypes();
    duckdb::ColumnBindingResolver resolver;
    duckdb::ColumnBindingResolver::Verify(*logical);
    resolver.VisitOperator(*logical);

    sirius::planner::sirius_physical_plan_generator generator(context);
    result = generator.create_plan(std::move(logical));
  } catch (...) {
    con.Query("ROLLBACK");
    duckdb::DBConfig::GetConfig(context).options.disabled_optimizers = original_disabled;
    throw;
  }

  require_success(con.Query("COMMIT"));
  duckdb::DBConfig::GetConfig(context).options.disabled_optimizers = original_disabled;
  return result;
}

struct generated_policy_plans {
  duckdb::unique_ptr<sirius_physical_operator> logical_filter;
  duckdb::unique_ptr<sirius_physical_operator> logical_get_filter;
  duckdb::unique_ptr<sirius_physical_operator> native_gpu_scan;
};

generated_policy_plans generate_policy_plans(duckdb::Connection& con, bool enabled)
{
  require_success(
    con.Query(std::string("SET filter_cascade_cheap_conjuncts = ") + (enabled ? "true" : "false")));
  REQUIRE(sirius::filter_cascade_policy_from_context(*con.context).enabled == enabled);

  generated_policy_plans plans;
  plans.logical_filter     = make_logical_filter_plan(*con.context);
  plans.logical_get_filter = make_logical_get_filter_plan(*con.context);
  plans.native_gpu_scan    = make_native_gpu_scan_plan(con);
  return plans;
}

void require_propagated_policy(generated_policy_plans& plans, bool expected)
{
  auto& logical_filter = require_only_operator<sirius::op::sirius_physical_filter>(
    *plans.logical_filter, SiriusPhysicalOperatorType::FILTER);
  CHECK(logical_filter.cascade_policy.enabled == expected);

  auto& get_filter = require_only_operator<sirius::op::sirius_physical_filter>(
    *plans.logical_get_filter, SiriusPhysicalOperatorType::FILTER);
  CHECK(get_filter.cascade_policy.enabled == expected);
  auto& get_scan = require_only_operator<sirius::op::sirius_physical_table_scan>(
    *plans.logical_get_filter, SiriusPhysicalOperatorType::TABLE_SCAN);
  CHECK(get_scan.cascade_policy.enabled == expected);

  auto& gpu_scan = require_only_operator<sirius::op::scan::sirius_gpu_scan_operator>(
    *plans.native_gpu_scan, SiriusPhysicalOperatorType::GPU_SCAN);
  CHECK(gpu_scan.get_ingestible().table_info().filter_policy.enabled == expected);
}

}  // namespace

TEST_CASE("filter cascade policy is captured at every planner boundary",
          "[planner][filter_cascade][isolated_context]")
{
  scoped_sirius_disable disable_sirius;
  scoped_temp_db_path db_path;
  duckdb::DuckDB db(db_path.path());
  duckdb::Connection con(db);

  require_success(con.Query("CREATE TABLE cascade_policy_source(value INTEGER)"));
  require_success(con.Query("CHECKPOINT"));

  auto const built_in_default = sirius::default_filter_cascade_policy().enabled;
  CHECK_FALSE(built_in_default);
  CHECK(sirius::filter_cascade_policy_from_context(*con.context).enabled == built_in_default);

  auto registered = con.Query(
    "SELECT current_setting('filter_cascade_cheap_conjuncts')::BOOLEAN, "
    "       value::BOOLEAN "
    "FROM duckdb_settings() WHERE name = 'filter_cascade_cheap_conjuncts'");
  REQUIRE(registered != nullptr);
  if (registered->HasError()) { UNSCOPED_INFO(registered->GetError()); }
  REQUIRE_FALSE(registered->HasError());
  REQUIRE(registered->RowCount() == 1);
  CHECK(registered->GetValue(0, 0).GetValue<bool>() == built_in_default);
  CHECK(registered->GetValue(1, 0).GetValue<bool>() == built_in_default);

  auto enabled_plans = generate_policy_plans(con, true);
  require_propagated_policy(enabled_plans, true);

  // Changing the connection affects only generators constructed afterwards; already-generated
  // operators and ingestible bind data retain their immutable snapshot.
  auto disabled_plans = generate_policy_plans(con, false);
  require_propagated_policy(disabled_plans, false);
  require_propagated_policy(enabled_plans, true);
}
