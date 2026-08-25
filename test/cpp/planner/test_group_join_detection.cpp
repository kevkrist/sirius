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

#include "op/scan/duckdb_native_gpu_ingestible.hpp"
#include "op/scan/sirius_gpu_scan_operator.hpp"
#include "op/sirius_physical_delim_join.hpp"
#include "op/sirius_physical_group_join.hpp"
#include "op/sirius_physical_hash_join.hpp"
#include "pipeline/repository_wiring.hpp"
#include "pipeline/sirius_pipeline.hpp"
#include "pipeline/sirius_pipeline_converter.hpp"
#include "planner/sirius_physical_plan_generator.hpp"
#include "utils/pipeline_conversion_test_utils.hpp"

#include <catch.hpp>
#include <duckdb.hpp>
#include <duckdb/execution/column_binding_resolver.hpp>
#include <duckdb/function/aggregate/distributive_functions.hpp>
#include <duckdb/function/aggregate_function.hpp>
#include <duckdb/main/config.hpp>
#include <duckdb/optimizer/optimizer.hpp>
#include <duckdb/parser/parser.hpp>
#include <duckdb/planner/bound_result_modifier.hpp>
#include <duckdb/planner/expression/bound_aggregate_expression.hpp>
#include <duckdb/planner/operator/logical_aggregate.hpp>
#include <duckdb/planner/planner.hpp>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

using namespace duckdb;

namespace {

// The GPU seq_scan ingestible requires a single-file block manager, so these tests use an
// on-disk database.
class scoped_temp_db_path {
 public:
  scoped_temp_db_path()
  {
    char tmpl[] = "/tmp/sirius_group_join_XXXXXX";
    int fd      = ::mkstemp(tmpl);
    REQUIRE(fd >= 0);
    ::close(fd);
    ::unlink(tmpl);
    _path = tmpl;
  }

  ~scoped_temp_db_path()
  {
    if (!_path.empty()) {
      std::remove(_path.c_str());
      std::remove((_path + ".wal").c_str());
    }
  }

  scoped_temp_db_path(const scoped_temp_db_path&)            = delete;
  scoped_temp_db_path& operator=(const scoped_temp_db_path&) = delete;

  const std::string& path() const { return _path; }

 private:
  std::string _path;
};

class scoped_temp_directory {
 public:
  scoped_temp_directory()
  {
    char tmpl[] = "/tmp/sirius_group_join_dso_XXXXXX";
    auto* path  = ::mkdtemp(tmpl);
    REQUIRE(path != nullptr);
    _path = path;
  }

  ~scoped_temp_directory()
  {
    std::error_code error;
    std::filesystem::remove_all(_path, error);
  }

  scoped_temp_directory(const scoped_temp_directory&)            = delete;
  scoped_temp_directory& operator=(const scoped_temp_directory&) = delete;

  const std::filesystem::path& path() const { return _path; }

 private:
  std::filesystem::path _path;
};

using logical_plan_mutator = bool (*)(duckdb::LogicalOperator&);

struct plan_generation_options {
  logical_plan_mutator mutate = nullptr;
};

void spoof_count_update(
  duckdb::Vector[], duckdb::AggregateInputData&, duckdb::idx_t, duckdb::Vector&, duckdb::idx_t)
{
}

duckdb::BoundAggregateExpression* find_first_count(duckdb::LogicalOperator& op)
{
  if (op.type == duckdb::LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
    auto& aggregate = op.Cast<duckdb::LogicalAggregate>();
    for (auto& expression : aggregate.expressions) {
      if (expression->GetExpressionClass() != duckdb::ExpressionClass::BOUND_AGGREGATE) {
        continue;
      }
      auto& bound = expression->Cast<duckdb::BoundAggregateExpression>();
      if (bound.function.name == "count" || bound.function.name == "count_star") { return &bound; }
    }
  }
  for (auto& child : op.children) {
    if (auto* bound = find_first_count(*child)) { return bound; }
  }
  return nullptr;
}

bool spoof_first_count_callback(duckdb::LogicalOperator& op)
{
  auto* bound = find_first_count(op);
  if (bound == nullptr) { return false; }
  bound->function.update = spoof_count_update;
  return true;
}

bool replace_first_count_with_internal_count_star(duckdb::LogicalOperator& op)
{
  auto* bound = find_first_count(op);
  if (bound == nullptr) { return false; }
  bound->function      = duckdb::CountStarFun::GetFunction();
  bound->function.name = "count_star";
  bound->function.catalog_name.clear();
  bound->function.schema_name.clear();
  bound->children.clear();
  bound->bind_info.reset();
  return true;
}

bool assign_non_system_count_provenance(duckdb::LogicalOperator& op)
{
  auto* bound = find_first_count(op);
  if (bound == nullptr) { return false; }
  bound->function.catalog_name = "user_catalog";
  bound->function.schema_name  = "main";
  return true;
}

duckdb::BoundAggregateExpression* find_first_value_aggregate(duckdb::LogicalOperator& op)
{
  if (op.type == duckdb::LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
    auto& aggregate = op.Cast<duckdb::LogicalAggregate>();
    for (auto& expression : aggregate.expressions) {
      if (expression->GetExpressionClass() != duckdb::ExpressionClass::BOUND_AGGREGATE) {
        continue;
      }
      auto& bound      = expression->Cast<duckdb::BoundAggregateExpression>();
      auto const& name = bound.function.name;
      if (name == "min" || name == "max" || name == "sum" || name == "avg") { return &bound; }
    }
  }
  for (auto& child : op.children) {
    if (auto* bound = find_first_value_aggregate(*child)) { return bound; }
  }
  return nullptr;
}

// A user aggregate reusing a builtin value-aggregate name differs from the catalog entry in its
// callbacks; swapping one callback models that collision.
bool spoof_first_value_aggregate_callback(duckdb::LogicalOperator& op)
{
  auto* bound = find_first_value_aggregate(op);
  if (bound == nullptr) { return false; }
  bound->function.update = spoof_count_update;
  return true;
}

// DuckDB's binder erases ORDER BY from order-agnostic builtins at bind time, so an ordered
// value aggregate cannot reach detection from SQL; attach one directly to exercise the screen.
bool attach_order_by_to_first_value_aggregate(duckdb::LogicalOperator& op)
{
  auto* bound = find_first_value_aggregate(op);
  if (bound == nullptr || bound->children.empty()) { return false; }
  auto modifier = duckdb::make_uniq<duckdb::BoundOrderModifier>();
  modifier->orders.emplace_back(
    duckdb::OrderType::ASCENDING, duckdb::OrderByNullType::NULLS_LAST, bound->children[0]->Copy());
  bound->order_bys = std::move(modifier);
  return true;
}

duckdb::unique_ptr<sirius::op::sirius_physical_operator> generate_sirius_plan(
  Connection& con, const std::string& query, plan_generation_options options = {})
{
  auto& context = *con.context;

  auto original_disabled = DBConfig::GetConfig(context).options.disabled_optimizers;
  auto& disabled         = DBConfig::GetConfig(context).options.disabled_optimizers;
  disabled.insert(OptimizerType::IN_CLAUSE);
  disabled.insert(OptimizerType::COMPRESSED_MATERIALIZATION);

  con.Query("BEGIN TRANSACTION");

  duckdb::unique_ptr<sirius::op::sirius_physical_operator> result;
  try {
    Parser parser(context.GetParserOptions());
    parser.ParseQuery(query);
    REQUIRE(!parser.statements.empty());

    Planner planner(context);
    planner.CreatePlan(std::move(parser.statements[0]));
    REQUIRE(planner.plan);

    auto plan = std::move(planner.plan);

    if (context.config.enable_optimizer) {
      Optimizer optimizer(*planner.binder, context);
      plan = optimizer.Optimize(std::move(plan));
    }

    plan->ResolveOperatorTypes();

    ColumnBindingResolver resolver;
    ColumnBindingResolver::Verify(*plan);
    resolver.VisitOperator(*plan);
    if (options.mutate) { REQUIRE(options.mutate(*plan)); }

    sirius::planner::sirius_physical_plan_generator gen(context);
    result = gen.create_plan(std::move(plan));
  } catch (...) {
    con.Query("ROLLBACK");
    DBConfig::GetConfig(context).options.disabled_optimizers = original_disabled;
    throw;
  }

  con.Query("COMMIT");
  DBConfig::GetConfig(context).options.disabled_optimizers = original_disabled;
  return result;
}

std::vector<sirius::op::sirius_physical_operator*> collect(
  sirius::op::sirius_physical_operator* root, sirius::op::SiriusPhysicalOperatorType type)
{
  std::vector<sirius::op::sirius_physical_operator*> out;
  if (!root) { return out; }
  if (root->type == type) { out.push_back(root); }
  for (auto& child : root->children) {
    auto sub = collect(child.get(), type);
    out.insert(out.end(), sub.begin(), sub.end());
  }
  return out;
}

// Like collect, but also descends into a delim join's owned subtrees (`join`, `distinct_root`),
// which live outside `children` -- required to find operators fused under a delim join's branch.
std::vector<sirius::op::sirius_physical_operator*> collect_deep(
  sirius::op::sirius_physical_operator* root, sirius::op::SiriusPhysicalOperatorType type)
{
  std::vector<sirius::op::sirius_physical_operator*> out;
  if (!root) { return out; }
  if (root->type == type) { out.push_back(root); }
  auto absorb = [&](sirius::op::sirius_physical_operator* node) {
    auto sub = collect_deep(node, type);
    out.insert(out.end(), sub.begin(), sub.end());
  };
  for (auto& child : root->children) {
    absorb(child.get());
  }
  using T = sirius::op::SiriusPhysicalOperatorType;
  if (root->type == T::LEFT_DELIM_JOIN || root->type == T::RIGHT_DELIM_JOIN) {
    auto& delim = static_cast<sirius::op::sirius_physical_delim_join&>(*root);
    absorb(delim.join.get());
    absorb(delim.distinct_root.get());
  }
  return out;
}

const sirius::op::scan::duckdb_native_ingestible_table_info& require_native_scan(
  sirius::op::sirius_physical_operator* root, std::string_view table_name, bool has_row_filter)
{
  using T    = sirius::op::SiriusPhysicalOperatorType;
  auto scans = collect(root, T::GPU_SCAN);
  REQUIRE(scans.size() == 1);

  auto const& scan = scans[0]->Cast<sirius::op::scan::sirius_gpu_scan_operator>();
  auto const* info = dynamic_cast<sirius::op::scan::duckdb_native_ingestible_table_info const*>(
    &scan.get_ingestible().table_info());
  REQUIRE(info != nullptr);
  CHECK(info->table_name == table_name);
  CHECK(scan.get_ingestible().has_row_filter() == has_row_filter);
  return *info;
}

void require_q13_counted_filter(sirius::op::sirius_physical_operator* preserved,
                                sirius::op::sirius_physical_operator* counted)
{
  auto const& preserved_scan = require_native_scan(preserved, "cust", false);
  CHECK((preserved_scan.table_filters == nullptr || preserved_scan.table_filters->filters.empty()));

  auto const& counted_scan = require_native_scan(counted, "ord", true);
  REQUIRE(counted_scan.table_filters != nullptr);
  REQUIRE(counted_scan.table_filters->filters.size() == 1);
  auto const& [column_index, filter] = *counted_scan.table_filters->filters.begin();
  REQUIRE(filter != nullptr);
  REQUIRE(column_index < counted_scan.column_ids.size());
  REQUIRE(counted_scan.column_ids[column_index].HasPrimaryIndex());
  CHECK(counted_scan.column_ids[column_index].GetPrimaryIndex() == 2);
}

struct group_join_fixture {
  group_join_fixture()
  {
    auto cfg = std::filesystem::path(SIRIUS_PROJECT_ROOT) / "test" / "cpp" / "config" / "data" /
               "minimal.yaml";
    setenv("SIRIUS_CONFIG_FILE", cfg.string().c_str(), 1);
    unsetenv("SIRIUS_DISABLE");
    db = std::make_unique<DuckDB>(db_path.path());
    setenv("SIRIUS_DISABLE", "1", 1);
    con          = std::make_unique<Connection>(*db);
    auto enabled = con->Query("SET enable_dense_count_join = true");
    REQUIRE(enabled != nullptr);
    REQUIRE_FALSE(enabled->HasError());
    // Isolate rung P0: with the value rungs live (enable_group_join defaults true), many of this
    // fixture's off-P0 shapes would legitimately fuse as DIRECT. The value rungs get their own
    // fixtures below.
    auto value_rungs_off = con->Query("SET enable_group_join = false");
    REQUIRE(value_rungs_off != nullptr);
    REQUIRE_FALSE(value_rungs_off->HasError());

    con->Query("CREATE TABLE cust (c_id INTEGER, c_grp INTEGER)");
    con->Query("INSERT INTO cust SELECT range, range % 3 FROM range(20)");
    // Keep c_grp nullable so DuckDB does not rewrite count(c_grp) to count_star().
    con->Query("INSERT INTO cust VALUES (100, NULL)");
    con->Query("CREATE TABLE ord (o_id BIGINT, o_cust INTEGER, o_note VARCHAR)");
    con->Query(
      "INSERT INTO ord SELECT range, (range * 7) % 30, concat('n', range) FROM range(200)");
  }

  ~group_join_fixture() { unsetenv("SIRIUS_CONFIG_FILE"); }

  bool has_group_join(const std::string& query, plan_generation_options options = {})
  {
    auto plan = generate_sirius_plan(*con, query, options);
    REQUIRE(plan);
    using T          = sirius::op::SiriusPhysicalOperatorType;
    auto const fused = collect(plan.get(), T::GROUP_JOIN);
    if (fused.empty()) { return false; }
    REQUIRE(fused.size() == 1);
    REQUIRE(collect(plan.get(), T::HASH_JOIN).empty());
    REQUIRE(collect(plan.get(), T::NESTED_LOOP_JOIN).empty());
    REQUIRE(fused[0]->children.size() == 2);
    return true;
  }

  scoped_temp_db_path db_path;
  std::unique_ptr<DuckDB> db;
  std::unique_ptr<Connection> con;
};

}  // namespace

TEST_CASE_METHOD(group_join_fixture,
                 "group_join fires on COUNT(col) grouped by the preserved LEFT-join key",
                 "[group_join][plan]")
{
  auto const query =
    "SELECT c_id, count(o_id) FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY c_id";
  REQUIRE(has_group_join(query));
  auto plan = generate_sirius_plan(*con, query);
  REQUIRE(collect(plan.get(), sirius::op::SiriusPhysicalOperatorType::HASH_GROUP_BY).empty());
}

TEST_CASE_METHOD(group_join_fixture,
                 "group_join fires on COUNT(*) and on the RIGHT-join orientation",
                 "[group_join][plan]")
{
  REQUIRE(
    has_group_join("SELECT c_id, count(*) FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY c_id"));
  REQUIRE(has_group_join(
    "SELECT c_id, count(o_id) FROM ord RIGHT JOIN cust ON o_cust = c_id GROUP BY c_id"));
}

TEST_CASE_METHOD(group_join_fixture,
                 "group_join fires inside the filtered two-level q13 distribution shape",
                 "[group_join][plan]")
{
  auto const query =
    "SELECT c_count, count(*) AS custdist FROM ("
    "  SELECT c_id, count(o_id) AS c_count FROM cust LEFT JOIN ord ON c_id = o_cust "
    "    AND o_note NOT LIKE '%special%requests%' GROUP BY c_id"
    ") t GROUP BY c_count";
  REQUIRE(has_group_join(query));

  auto plan  = generate_sirius_plan(*con, query);
  using T    = sirius::op::SiriusPhysicalOperatorType;
  auto fused = collect(plan.get(), T::GROUP_JOIN);
  REQUIRE(fused.size() == 1);
  REQUIRE(fused[0]->children.size() == 2);
  CHECK(collect(fused[0]->children[0].get(), T::FILTER).empty());
  CHECK(collect(fused[0]->children[1].get(), T::FILTER).empty());
  require_q13_counted_filter(fused[0]->children[0].get(), fused[0]->children[1].get());
  REQUIRE(collect(plan.get(), T::HASH_GROUP_BY).size() == 1);
}

TEST_CASE_METHOD(group_join_fixture,
                 "group_join conversion preserves its filtered counted input and FULL "
                 "barriers",
                 "[group_join][pipeline]")
{
  auto const query =
    "SELECT c_id, count(o_id) FROM cust LEFT JOIN ord ON c_id = o_cust "
    "  AND o_note NOT LIKE '%special%requests%' GROUP BY c_id";

  REQUIRE(has_group_join(query));

  sirius::test::with_conversion_result(
    *con, query, [](sirius::pipeline::pipeline_conversion_result& result) {
      using sirius::op::MemoryBarrierType;
      using sirius::op::SiriusPhysicalOperatorType;
      using sirius::op::sirius_physical_group_join;
      using sirius::pipeline::repository_wiring;
      using sirius::pipeline::sirius_pipeline;

      sirius_pipeline* fused_pipeline = nullptr;
      for (auto const& pipeline : result.scheduled_pipelines) {
        for (auto const& op_ref : pipeline->get_operators()) {
          if (op_ref.get().type != SiriusPhysicalOperatorType::GROUP_JOIN) { continue; }
          REQUIRE(fused_pipeline == nullptr);
          fused_pipeline = pipeline.get();
        }
      }

      REQUIRE(fused_pipeline != nullptr);
      auto const operators = fused_pipeline->get_operators();
      REQUIRE(operators.size() == 1);
      auto const* fused = &operators[0].get();
      CHECK(fused_pipeline->get_source().get() == fused);
      CHECK(fused_pipeline->get_sink().get() == fused);
      REQUIRE(fused->children.size() == 2);
      CHECK(collect(fused->children[0].get(), SiriusPhysicalOperatorType::FILTER).empty());
      CHECK(collect(fused->children[1].get(), SiriusPhysicalOperatorType::FILTER).empty());
      require_q13_counted_filter(fused->children[0].get(), fused->children[1].get());

      std::vector<repository_wiring const*> inputs;
      for (auto const& wiring : result.repository_wirings) {
        if (wiring.dest_pipeline.get() == fused_pipeline) { inputs.push_back(&wiring); }
      }
      REQUIRE(inputs.size() == 2);

      auto require_direct_input = [&](std::size_t child_index, std::string_view port_id) {
        repository_wiring const* match = nullptr;
        for (auto const* wiring : inputs) {
          if (wiring->port_id != port_id) { continue; }
          REQUIRE(match == nullptr);
          match = wiring;
        }
        REQUIRE(match != nullptr);
        CHECK(match->barrier_type == MemoryBarrierType::FULL);
        CHECK(match->source_op == fused->children[child_index].get());
        REQUIRE(match->source_pipeline);
        CHECK(match->source_pipeline->get_sink().get() == fused->children[child_index].get());
      };

      require_direct_input(0, sirius_physical_group_join::PRESERVED_PORT);
      require_direct_input(1, sirius_physical_group_join::COUNTED_PORT);
    });
}

TEST_CASE_METHOD(group_join_fixture,
                 "group_join declines off-shape aggregates and joins",
                 "[group_join][plan]")
{
  CHECK_FALSE(
    has_group_join("SELECT c_id, count(o_id) FROM cust JOIN ord ON c_id = o_cust GROUP BY c_id"));
  CHECK_FALSE(has_group_join(
    "SELECT c_grp, count(o_id) FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY c_grp"));
  CHECK_FALSE(has_group_join(
    "SELECT c_id, count(o_id), max(o_id) FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY "
    "c_id"));
  CHECK_FALSE(has_group_join(
    "SELECT c_id, count(DISTINCT o_id) FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY c_id"));
  CHECK_FALSE(has_group_join(
    "SELECT c_id, sum(o_id) FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY c_id"));
  // A nullable preserved-side COUNT has different outer-join NULL semantics.
  CHECK_FALSE(has_group_join(
    "SELECT c_id, count(c_grp) FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY c_id"));
}

TEST_CASE_METHOD(group_join_fixture,
                 "group_join authenticates COUNT callbacks, not its public name",
                 "[group_join][plan]")
{
  auto const query =
    "SELECT c_id, count(o_id) FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY c_id";
  auto plan = generate_sirius_plan(
    *con, query, plan_generation_options{.mutate = spoof_first_count_callback});
  REQUIRE(plan);
  using T = sirius::op::SiriusPhysicalOperatorType;
  CHECK(collect(plan.get(), T::GROUP_JOIN).empty());
  CHECK(collect(plan.get(), T::HASH_JOIN).size() == 1);
  CHECK(collect(plan.get(), T::HASH_GROUP_BY).size() == 1);
}

TEST_CASE_METHOD(group_join_fixture,
                 "group_join accepts optimizer-created COUNT_STAR without provenance",
                 "[group_join][plan]")
{
  auto const query =
    "SELECT c_id, count(o_id) FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY c_id";
  REQUIRE(has_group_join(
    query, plan_generation_options{.mutate = replace_first_count_with_internal_count_star}));
}

TEST_CASE_METHOD(group_join_fixture,
                 "group_join rejects COUNT with non-system provenance",
                 "[group_join][plan]")
{
  auto const query =
    "SELECT c_id, count(o_id) FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY c_id";
  auto plan = generate_sirius_plan(
    *con, query, plan_generation_options{.mutate = assign_non_system_count_provenance});
  REQUIRE(plan);
  using T = sirius::op::SiriusPhysicalOperatorType;
  CHECK(collect(plan.get(), T::GROUP_JOIN).empty());
  CHECK(collect(plan.get(), T::HASH_JOIN).size() == 1);
  CHECK(collect(plan.get(), T::HASH_GROUP_BY).size() == 1);
}

TEST_CASE_METHOD(group_join_fixture,
                 "group_join defaults to the fused plan and supports an explicit opt-out",
                 "[group_join][plan]")
{
  auto const query =
    "SELECT c_id, count(o_id) FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY c_id";

  auto reset = con->Query("RESET enable_dense_count_join");
  REQUIRE_FALSE(reset->HasError());
  REQUIRE(has_group_join(query));

  auto off = con->Query("SET enable_dense_count_join = false");
  REQUIRE_FALSE(off->HasError());
  auto disabled_plan = generate_sirius_plan(*con, query);
  REQUIRE(disabled_plan);
  using T = sirius::op::SiriusPhysicalOperatorType;
  CHECK(collect(disabled_plan.get(), T::GROUP_JOIN).empty());
  CHECK(collect(disabled_plan.get(), T::HASH_JOIN).size() == 1);
  CHECK(collect(disabled_plan.get(), T::HASH_GROUP_BY).size() == 1);
}

TEST_CASE_METHOD(group_join_fixture,
                 "group_join declines filtered aggregates, extra conditions, and "
                 "non-plain keys",
                 "[group_join][plan]")
{
  CHECK_FALSE(has_group_join(
    "SELECT c_id, count(o_id) FILTER (WHERE o_id > 0) FROM cust LEFT JOIN ord ON c_id = o_cust "
    "GROUP BY c_id"));
  CHECK_FALSE(has_group_join(
    "SELECT c_id, count(o_id) FROM cust LEFT JOIN ord ON c_id = o_cust AND c_grp = o_cust "
    "GROUP BY c_id"));
  // A preserved-side ON residual cannot move below LEFT JOIN without dropping retained rows.
  CHECK_THROWS_WITH(
    has_group_join(
      "SELECT c_id, count(o_id) FROM cust LEFT JOIN ord ON c_id = o_cust AND c_grp > 0 "
      "GROUP BY c_id"),
    Catch::Contains("Any join not supported"));
  // INTEGER = BIGINT inserts a CAST, so the plain-reference gate declines.
  CHECK_FALSE(has_group_join(
    "SELECT c_id, count(o_cust) FROM cust LEFT JOIN ord ON c_id = o_id GROUP BY c_id"));
  CHECK_FALSE(has_group_join(
    "SELECT c_id, count(o_id) FROM cust LEFT JOIN ord ON c_id IS NOT DISTINCT FROM o_cust "
    "GROUP BY c_id"));
}

TEST_CASE_METHOD(group_join_fixture,
                 "group_join declines intervening operators and non-linear join children",
                 "[group_join][plan]")
{
  CHECK_FALSE(
    has_group_join("SELECT c_id, count(o_id) FROM cust LEFT JOIN ord ON c_id = o_cust "
                   "WHERE (o_id IS NULL OR c_grp = 0) GROUP BY c_id"));
  CHECK_FALSE(has_group_join(
    "SELECT o_cust, count(o_id) FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY o_cust"));
  // Identity projection can be elided, exposing the non-linear join child.
  CHECK_FALSE(
    has_group_join("SELECT c_id, count(o.o_id) FROM cust LEFT JOIN ("
                   "  SELECT o1.o_id, o1.o_cust FROM ord o1 JOIN ord o2 ON o1.o_id = o2.o_id"
                   ") o ON c_id = o.o_cust GROUP BY c_id"));
}

namespace {

// Rung P1 fixture: enables the value rungs and adds an INNER-join corpus -- a PRIMARY KEY dim
// (provable unique preserved side), a plain dim (unprovable), a large PK dim (publication-parity
// shapes), and a fact table with every v1 argument carrier.
struct inner_group_join_fixture : group_join_fixture {
  inner_group_join_fixture()
  {
    auto enabled = con->Query("SET enable_group_join = true");
    REQUIRE(enabled != nullptr);
    REQUIRE_FALSE(enabled->HasError());

    con->Query("CREATE TABLE dim (d_id INTEGER PRIMARY KEY, d_pad INTEGER, d_txt VARCHAR)");
    con->Query("INSERT INTO dim SELECT range, range % 5, concat('t', range % 9) FROM range(40)");
    con->Query("CREATE TABLE dim_plain (dp_id INTEGER, dp_pad INTEGER)");
    con->Query("INSERT INTO dim_plain SELECT range % 30, range % 5 FROM range(40)");
    con->Query("CREATE TABLE dim_big (b_id INTEGER PRIMARY KEY)");
    con->Query("INSERT INTO dim_big SELECT range FROM range(2000)");
    con->Query(
      "CREATE TABLE fact (f_d INTEGER, f_v INTEGER, f_dec DECIMAL(15,2), f_big BIGINT, "
      "f_txt VARCHAR)");
    con->Query(
      "INSERT INTO fact SELECT range % 50, range % 7, (range % 90) / 4.0, range, "
      "concat('t', range % 9) FROM range(200)");
  }

  // Finds the single fused operator of any form anywhere in the plan (including under delim
  // joins), or nullptr when the plan did not fuse.
  sirius::op::sirius_physical_group_join* find_group_join(
    sirius::op::sirius_physical_operator* root)
  {
    auto const fused = collect_deep(root, sirius::op::SiriusPhysicalOperatorType::GROUP_JOIN);
    if (fused.empty()) { return nullptr; }
    REQUIRE(fused.size() == 1);
    return &fused[0]->Cast<sirius::op::sirius_physical_group_join>();
  }

  // Which rung (if any) fused the query, reported as the spec's join form.
  std::optional<sirius::op::groupjoin::join_form> fused_form(const std::string& query)
  {
    auto plan = generate_sirius_plan(*con, query);
    REQUIRE(plan);
    auto* op = find_group_join(plan.get());
    if (op == nullptr) { return std::nullopt; }
    return op->spec().form;
  }

  // Finds the fused INNER-form operator and validates its shape, or nullptr when the plan did
  // not fuse at all. Use only where INNER-or-nothing is the expectation.
  sirius::op::sirius_physical_group_join* find_inner_group_join(
    sirius::op::sirius_physical_operator* root)
  {
    auto* op = find_group_join(root);
    if (op == nullptr) { return nullptr; }
    REQUIRE(op->spec().form == sirius::op::groupjoin::join_form::INNER);
    REQUIRE(op->children.size() == 2);
    return op;
  }

  bool has_inner_group_join(const std::string& query)
  {
    auto plan = generate_sirius_plan(*con, query);
    REQUIRE(plan);
    return find_inner_group_join(plan.get()) != nullptr;
  }
};

}  // namespace

TEST_CASE_METHOD(inner_group_join_fixture,
                 "group_join rung P1 fuses delim-preserved value aggregates",
                 "[group_join][plan]")
{
  // The q17 shape: a correlated aggregate whose group key is the DELIM_GET's correlation key.
  // The outer correlated table must carry a filter -- an unfiltered one lets DuckDB's deliminator
  // rewrite the delim join away entirely (aggregate directly over the fact side).
  auto const correlated = [](std::string_view aggregate) {
    return "SELECT sum(f.f_v) FROM fact f, dim_plain p WHERE p.dp_id = f.f_d AND p.dp_pad = 2 "
           "AND f.f_v < (SELECT " +
           std::string(aggregate) + " FROM fact f2 WHERE f2.f_d = p.dp_id)";
  };
  REQUIRE(has_inner_group_join(correlated("avg(f2.f_v)")));
  REQUIRE(has_inner_group_join(correlated("avg(f2.f_dec)")));
  REQUIRE(has_inner_group_join(correlated("sum(f2.f_v)")));
  REQUIRE(has_inner_group_join(correlated("min(f2.f_v)")));
  REQUIRE(has_inner_group_join(correlated("max(f2.f_dec)")));

  // The DELIM_GET preserved side is opaque-build evidence, so the fused operator must carry the
  // membership publication plan the replaced hash join would have used.
  auto plan   = generate_sirius_plan(*con, correlated("avg(f2.f_dec)"));
  auto* fused = find_inner_group_join(plan.get());
  REQUIRE(fused != nullptr);
  CHECK(fused->children[0]->type == sirius::op::SiriusPhysicalOperatorType::DELIM_SCAN);
  CHECK(fused->dynamic_filter_plan().enabled());
  REQUIRE(fused->dynamic_filter_plan().admitted_keys().size() == 1);
  CHECK(fused->dynamic_filter_plan().admitted_keys()[0].build_key_ordinal ==
        static_cast<cudf::size_type>(fused->preserved_key_idx()));
}

TEST_CASE_METHOD(inner_group_join_fixture,
                 "group_join rung P1 fuses unique-scan-preserved value aggregates",
                 "[group_join][plan]")
{
  REQUIRE(
    has_inner_group_join("SELECT d_id, sum(f_v) FROM dim JOIN fact ON d_id = f_d GROUP BY d_id"));
  REQUIRE(
    has_inner_group_join("SELECT d_id, avg(f_dec) FROM dim JOIN fact ON d_id = f_d GROUP BY d_id"));
  REQUIRE(
    has_inner_group_join("SELECT d_id, min(f_v) FROM dim JOIN fact ON d_id = f_d GROUP BY d_id"));
  REQUIRE(
    has_inner_group_join("SELECT d_id, count(f_v) FROM dim JOIN fact ON d_id = f_d GROUP BY d_id"));
  REQUIRE(
    has_inner_group_join("SELECT d_id, count(*) FROM dim JOIN fact ON d_id = f_d GROUP BY d_id"));
}

TEST_CASE_METHOD(inner_group_join_fixture,
                 "group_join rung P1 is gated by enable_group_join",
                 "[group_join][plan]")
{
  auto const query = "SELECT d_id, sum(f_v) FROM dim JOIN fact ON d_id = f_d GROUP BY d_id";
  REQUIRE(has_inner_group_join(query));

  auto off = con->Query("SET enable_group_join = false");
  REQUIRE_FALSE(off->HasError());
  CHECK_FALSE(has_inner_group_join(query));
  auto on = con->Query("SET enable_group_join = true");
  REQUIRE_FALSE(on->HasError());
}

TEST_CASE_METHOD(inner_group_join_fixture,
                 "group_join rung P1 declines off-shape aggregates and joins",
                 "[group_join][plan]")
{
  using sirius::op::groupjoin::join_form;

  // The first five shapes miss rung P1 but stay valid plain-GROUP-BY-over-a-join fragments, so
  // rung P2 legitimately picks them up as DIRECT -- which is itself the ladder-ordering evidence
  // that P1 declined them.
  // Preserved side neither DELIM_GET nor provably unique.
  CHECK(fused_form("SELECT dp_id, sum(f_v) FROM dim_plain JOIN fact ON dp_id = f_d GROUP BY "
                   "dp_id") == join_form::DIRECT);
  // Group key on the counted side: the optimizer canonicalizes an INNER-equality group ref onto
  // either side, so the shape that stays P1-declined is the one where the group side is neither a
  // DELIM_GET nor provably unique on either binding.
  CHECK(fused_form("SELECT f_d, sum(f_v) FROM dim_plain JOIN fact ON dp_id = f_d GROUP BY f_d") ==
        join_form::DIRECT);
  // Expression key: the INTEGER = BIGINT comparison inserts a cast (opaque to rung P2).
  CHECK(fused_form("SELECT d_id, sum(f_v) FROM dim JOIN fact ON d_id = f_big GROUP BY d_id") ==
        join_form::DIRECT);
  // Multi-condition join (opaque to rung P2).
  CHECK(fused_form("SELECT d_id, sum(f_v) FROM dim JOIN fact ON d_id = f_d AND d_pad = f_v "
                   "GROUP BY d_id") == join_form::DIRECT);
  // Null-safe key (opaque to rung P2).
  CHECK(fused_form("SELECT d_id, sum(f_v) FROM dim JOIN fact ON d_id IS NOT DISTINCT FROM f_d "
                   "GROUP BY d_id") == join_form::DIRECT);

  // These miss every rung.
  // Non-integer keys.
  CHECK_FALSE(
    fused_form("SELECT d_txt, min(f_v) FROM dim JOIN fact ON d_txt = f_txt GROUP BY d_txt"));
  // DISTINCT and FILTER modifiers.
  CHECK_FALSE(
    fused_form("SELECT d_id, sum(DISTINCT f_v) FROM dim JOIN fact ON d_id = f_d GROUP BY d_id"));
  CHECK_FALSE(fused_form(
    "SELECT d_id, sum(f_v) FILTER (WHERE f_v > 2) FROM dim JOIN fact ON d_id = f_d GROUP BY "
    "d_id"));
  // Multiple groups and multiple aggregates.
  CHECK_FALSE(fused_form(
    "SELECT d_id, d_pad, sum(f_v) FROM dim JOIN fact ON d_id = f_d GROUP BY d_id, d_pad"));
  CHECK_FALSE(
    fused_form("SELECT d_id, sum(f_v), min(f_v) FROM dim JOIN fact ON d_id = f_d GROUP BY d_id"));
}

TEST_CASE_METHOD(inner_group_join_fixture,
                 "group_join rung P1 declines when the counted-side byte gate trips",
                 "[group_join][plan]")
{
  auto const query = "SELECT d_id, sum(f_v) FROM dim JOIN fact ON d_id = f_d GROUP BY d_id";
  REQUIRE(has_inner_group_join(query));

  auto tiny = con->Query("SET group_join_counted_bytes_gate = 1");
  REQUIRE_FALSE(tiny->HasError());
  CHECK_FALSE(has_inner_group_join(query));
  auto reset = con->Query("RESET group_join_counted_bytes_gate");
  REQUIRE_FALSE(reset->HasError());
  CHECK(has_inner_group_join(query));
}

TEST_CASE_METHOD(inner_group_join_fixture,
                 "group_join rung P1 declines a dynamic-filter downgrade and fuses when filters "
                 "are off",
                 "[group_join][plan]")
{
  using sirius::op::groupjoin::join_form;

  // The filtered fact is the smaller relation, so it becomes the replaced hash join's build side
  // (children[1]) and carries filter evidence, while the preserved (group-key) side is
  // children[0]: the replaced join would publish fact-key filters onto the dim_big scan, which
  // the fused operator cannot reproduce -- rung P1 must decline. Rung P2 then fuses only the
  // aggregate as DIRECT, which is not a downgrade: the hash join stays in the plan with its own
  // publication.
  auto const query =
    "SELECT b_id, sum(f.f_v) FROM dim_big JOIN (SELECT * FROM fact WHERE f_v > 3) f "
    "ON b_id = f.f_d GROUP BY b_id";
  {
    auto plan = generate_sirius_plan(*con, query);
    REQUIRE(plan);
    auto* fused = find_group_join(plan.get());
    REQUIRE(fused != nullptr);
    REQUIRE(fused->spec().form == join_form::DIRECT);
    CHECK_FALSE(fused->dynamic_filter_plan().enabled());
    auto const joins = collect_deep(plan.get(), sirius::op::SiriusPhysicalOperatorType::HASH_JOIN);
    REQUIRE(joins.size() == 1);
    CHECK(joins[0]->Cast<sirius::op::sirius_physical_hash_join>().dynamic_filter_plan().enabled());
  }

  // With dynamic filters disabled there is nothing to drop, so the same shape fuses as INNER.
  auto off = con->Query("SET enable_dynamic_filter = false");
  REQUIRE_FALSE(off->HasError());
  CHECK(has_inner_group_join(query));
  auto reset = con->Query("RESET enable_dynamic_filter");
  REQUIRE_FALSE(reset->HasError());
}

TEST_CASE_METHOD(inner_group_join_fixture,
                 "group_join rung P1 conversion wires the fused-under-delim shape",
                 "[group_join][pipeline]")
{
  auto const query =
    "SELECT sum(f.f_v) FROM fact f, dim_plain p WHERE p.dp_id = f.f_d AND p.dp_pad = 2 "
    "AND f.f_v < (SELECT avg(f2.f_v) FROM fact f2 WHERE f2.f_d = p.dp_id)";
  REQUIRE(has_inner_group_join(query));

  sirius::test::with_conversion_result(
    *con, query, [](sirius::pipeline::pipeline_conversion_result& result) {
      using sirius::op::MemoryBarrierType;
      using sirius::op::SiriusPhysicalOperatorType;
      using sirius::op::sirius_physical_group_join;
      using sirius::pipeline::repository_wiring;
      using sirius::pipeline::sirius_pipeline;

      // The fused operator forms its own source+sink pipeline, and the routing-only DELIM_SCAN
      // never lands in any pipeline (no sourceless dead pipeline exists).
      sirius_pipeline* fused_pipeline   = nullptr;
      sirius_physical_group_join* fused = nullptr;
      for (auto const& pipeline : result.scheduled_pipelines) {
        REQUIRE(pipeline->get_source() != nullptr);
        for (auto const& op_ref : pipeline->get_operators()) {
          REQUIRE(op_ref.get().type != SiriusPhysicalOperatorType::DELIM_SCAN);
          if (op_ref.get().type != SiriusPhysicalOperatorType::GROUP_JOIN) { continue; }
          REQUIRE(fused_pipeline == nullptr);
          fused_pipeline = pipeline.get();
          fused          = &op_ref.get().Cast<sirius_physical_group_join>();
        }
      }
      REQUIRE(fused_pipeline != nullptr);
      REQUIRE(fused != nullptr);
      REQUIRE(fused->children.size() == 2);
      REQUIRE(fused->children[0]->type == SiriusPhysicalOperatorType::DELIM_SCAN);

      // Exactly two FULL input wirings: the counted child pipeline onto "counted", and the
      // distinct-chain root (a non-child producer owned by the delim join) onto "preserved".
      repository_wiring const* preserved = nullptr;
      repository_wiring const* counted   = nullptr;
      for (auto const& wiring : result.repository_wirings) {
        if (wiring.dest_pipeline.get() != fused_pipeline) { continue; }
        if (wiring.port_id == sirius_physical_group_join::PRESERVED_PORT) {
          REQUIRE(preserved == nullptr);
          preserved = &wiring;
        } else if (wiring.port_id == sirius_physical_group_join::COUNTED_PORT) {
          REQUIRE(counted == nullptr);
          counted = &wiring;
        } else {
          FAIL("unexpected input wiring port: " << wiring.port_id);
        }
      }
      REQUIRE(preserved != nullptr);
      REQUIRE(counted != nullptr);
      CHECK(preserved->barrier_type == MemoryBarrierType::FULL);
      CHECK(counted->barrier_type == MemoryBarrierType::FULL);
      CHECK(counted->source_op == fused->children[1].get());

      // The preserved producer is the delim distinct chain's root, not the DELIM_SCAN child.
      REQUIRE(preserved->source_op != nullptr);
      CHECK(preserved->source_op->owning_delim_join() != nullptr);
      REQUIRE(preserved->source_pipeline);
      CHECK(preserved->source_pipeline->get_sink().get() == preserved->source_op);

      // The DELIM_SCAN dependency registration and both input wirings land in the fused
      // pipeline's dependency set, so its single task fires only after both producers finish.
      auto const& dependencies = fused_pipeline->dependencies;
      auto depends_on          = [&](sirius_pipeline const* producer) {
        for (auto const& dependency : dependencies) {
          if (dependency.get() == producer) { return true; }
        }
        return false;
      };
      CHECK(depends_on(preserved->source_pipeline.get()));
      CHECK(depends_on(counted->source_pipeline.get()));
    });
}

TEST_CASE_METHOD(inner_group_join_fixture,
                 "group_join value rungs decline hostile aggregate mutations",
                 "[group_join][plan]")
{
  using T          = sirius::op::SiriusPhysicalOperatorType;
  auto const query = "SELECT d_id, min(f_v) FROM dim JOIN fact ON d_id = f_d GROUP BY d_id";
  REQUIRE(fused_form(query) == sirius::op::groupjoin::join_form::INNER);

  // A callback spoofed on a value aggregate fails the re-bind identity check on both rungs.
  {
    auto plan = generate_sirius_plan(
      *con, query, plan_generation_options{.mutate = spoof_first_value_aggregate_callback});
    REQUIRE(plan);
    CHECK(collect_deep(plan.get(), T::GROUP_JOIN).empty());
    CHECK(collect_deep(plan.get(), T::HASH_JOIN).size() == 1);
  }

  // A synthetic ordered value aggregate (unbuildable from SQL: the binder erases ORDER BY on
  // order-agnostic builtins) trips the order_bys screen on both rungs. Generic planning may
  // subsequently reject the sorted-aggregate rewrite entirely (a CPU fallback throw); either
  // outcome proves no rung fused it.
  try {
    auto plan = generate_sirius_plan(
      *con, query, plan_generation_options{.mutate = attach_order_by_to_first_value_aggregate});
    REQUIRE(plan);
    CHECK(collect_deep(plan.get(), T::GROUP_JOIN).empty());
  } catch (std::exception const&) {
    SUCCEED("generic planning rejected the synthetic ordered aggregate without fusing it");
  }

  // ORDER BY written in SQL on an order-agnostic builtin is erased by the binder before
  // detection, so the plain INNER fusion still applies with unchanged (order-free) semantics.
  CHECK(fused_form("SELECT d_id, sum(f_v ORDER BY f_v) FROM dim JOIN fact ON d_id = f_d "
                   "GROUP BY d_id") == sirius::op::groupjoin::join_form::INNER);

  // A non-equality residual in the ON clause is planned as FILTER nodes above the join, which
  // both rungs refuse (the child is no longer a comparison-join root).
  CHECK_FALSE(
    fused_form("SELECT d_id, sum(f_v) FROM dim JOIN fact ON d_id = f_d AND (d_pad + f_v) % 3 = 0 "
               "GROUP BY d_id"));
}

TEST_CASE_METHOD(inner_group_join_fixture,
                 "group_join rung P2 fuses aggregates over opaque join children as DIRECT",
                 "[group_join][plan]")
{
  using T = sirius::op::SiriusPhysicalOperatorType;
  using sirius::op::groupjoin::agg_op;
  using sirius::op::groupjoin::join_form;

  // The q2 shape: the group key is NOT a join key of the child, so rung P1 misses and rung P2
  // fuses only the aggregate, leaving the join subtree planned unchanged beneath it.
  auto const query = "SELECT f_v, min(f_dec) FROM dim_plain JOIN fact ON dp_id = f_d GROUP BY f_v";
  auto plan        = generate_sirius_plan(*con, query);
  REQUIRE(plan);
  auto* fused = find_group_join(plan.get());
  REQUIRE(fused != nullptr);
  REQUIRE(fused->spec().form == join_form::DIRECT);
  REQUIRE(fused->children.size() == 1);
  CHECK(fused->spec().slots[0].op == agg_op::MIN);
  CHECK(fused->spec().slots[0].arg_idx.has_value());
  CHECK(fused->counted_key_idx() == fused->preserved_key_idx());
  CHECK_FALSE(fused->dynamic_filter_plan().enabled());
  CHECK(collect_deep(fused->children[0].get(), T::HASH_JOIN).size() == 1);
  CHECK(collect_deep(plan.get(), T::HASH_GROUP_BY).empty());

  // Aggregate variants over the same opaque child.
  auto const variant = [](std::string_view aggregate) {
    return "SELECT f_v, " + std::string(aggregate) +
           " FROM dim_plain JOIN fact ON dp_id = f_d GROUP BY f_v";
  };
  CHECK(fused_form(variant("sum(f_big)")) == join_form::DIRECT);
  CHECK(fused_form(variant("avg(f_dec)")) == join_form::DIRECT);
  CHECK(fused_form(variant("count(f_dec)")) == join_form::DIRECT);
  CHECK(fused_form(variant("count(*)")) == join_form::DIRECT);

  // Any join type: the child is opaque, so an outer join fuses too (its padding NULLs are the
  // executor's argument-validity-gate concern, not the planner's).
  CHECK(fused_form("SELECT f_v, sum(f_big) FROM dim_plain LEFT JOIN fact ON dp_id = f_d "
                   "GROUP BY f_v") == join_form::DIRECT);
  // BIGINT group key.
  CHECK(fused_form("SELECT f_big, min(f_v) FROM dim_plain JOIN fact ON dp_id = f_d "
                   "GROUP BY f_big") == join_form::DIRECT);
  // Ladder ordering: a rung-P1-eligible shape stays INNER; P2 never sees it.
  CHECK(fused_form("SELECT d_id, sum(f_v) FROM dim JOIN fact ON d_id = f_d GROUP BY d_id") ==
        join_form::INNER);
}

TEST_CASE_METHOD(inner_group_join_fixture,
                 "group_join rung P2 fails closed on off-shape children and aggregates",
                 "[group_join][plan]")
{
  // Non-join children: the join-root requirement is the anti-overlap guard against generic
  // aggregate planning -- a plain scan, an aggregate, or a computing projection must never fuse.
  CHECK_FALSE(fused_form("SELECT c_grp, min(c_id) FROM cust GROUP BY c_grp"));
  CHECK_FALSE(fused_form(
    "SELECT g, min(n) FROM (SELECT c_grp AS g, count(*) AS n FROM cust GROUP BY c_grp) t "
    "GROUP BY g"));
  CHECK_FALSE(fused_form(
    "SELECT k, min(fv) FROM (SELECT dp_pad + f_v AS k, f_v AS fv FROM dim_plain JOIN fact "
    "ON dp_id = f_d) t GROUP BY k"));

  // Off-shape groups and aggregates over an otherwise eligible opaque join child.
  CHECK_FALSE(
    fused_form("SELECT f_txt, min(f_v) FROM dim_plain JOIN fact ON dp_id = f_d GROUP BY f_txt"));
  CHECK_FALSE(
    fused_form("SELECT f_dec, min(f_v) FROM dim_plain JOIN fact ON dp_id = f_d GROUP BY f_dec"));
  CHECK_FALSE(
    fused_form("SELECT f_v, dp_pad, min(f_dec) FROM dim_plain JOIN fact ON dp_id = f_d "
               "GROUP BY f_v, dp_pad"));
  // DISTINCT on a distinct-sensitive aggregate is a miss; the optimizer erases DISTINCT from
  // min/max (distinct-agnostic), so those still fuse with unchanged semantics.
  CHECK_FALSE(fused_form(
    "SELECT f_v, sum(DISTINCT f_big) FROM dim_plain JOIN fact ON dp_id = f_d GROUP BY f_v"));
  CHECK(fused_form("SELECT f_v, min(DISTINCT f_dec) FROM dim_plain JOIN fact ON dp_id = f_d "
                   "GROUP BY f_v") == sirius::op::groupjoin::join_form::DIRECT);
  CHECK_FALSE(fused_form(
    "SELECT f_v, min(f_dec) FILTER (WHERE f_big > 2) FROM dim_plain JOIN fact ON dp_id = f_d "
    "GROUP BY f_v"));
  CHECK_FALSE(
    fused_form("SELECT f_v, min(f_big + 1) FROM dim_plain JOIN fact ON dp_id = f_d GROUP BY f_v"));
  CHECK_FALSE(
    fused_form("SELECT f_v, min(f_txt) FROM dim_plain JOIN fact ON dp_id = f_d GROUP BY f_v"));
}

TEST_CASE_METHOD(inner_group_join_fixture,
                 "group_join rung P2 is gated by enable_group_join and the child byte gate",
                 "[group_join][plan]")
{
  using sirius::op::groupjoin::join_form;
  auto const query = "SELECT f_v, min(f_dec) FROM dim_plain JOIN fact ON dp_id = f_d GROUP BY f_v";
  REQUIRE(fused_form(query) == join_form::DIRECT);

  auto off = con->Query("SET enable_group_join = false");
  REQUIRE_FALSE(off->HasError());
  CHECK_FALSE(fused_form(query));
  auto on = con->Query("SET enable_group_join = true");
  REQUIRE_FALSE(on->HasError());

  auto tiny = con->Query("SET group_join_counted_bytes_gate = 1");
  REQUIRE_FALSE(tiny->HasError());
  CHECK_FALSE(fused_form(query));
  auto reset = con->Query("RESET group_join_counted_bytes_gate");
  REQUIRE_FALSE(reset->HasError());
  CHECK(fused_form(query) == join_form::DIRECT);
}

TEST_CASE_METHOD(inner_group_join_fixture,
                 "group_join rung P2 conversion wires the single-child sink shape",
                 "[group_join][pipeline]")
{
  auto const query = "SELECT f_v, min(f_dec) FROM dim_plain JOIN fact ON dp_id = f_d GROUP BY f_v";
  REQUIRE(fused_form(query) == sirius::op::groupjoin::join_form::DIRECT);

  sirius::test::with_conversion_result(
    *con, query, [](sirius::pipeline::pipeline_conversion_result& result) {
      using sirius::op::MemoryBarrierType;
      using sirius::op::SiriusPhysicalOperatorType;
      using sirius::op::sirius_physical_group_join;
      using sirius::pipeline::repository_wiring;
      using sirius::pipeline::sirius_pipeline;

      // The fused operator forms its own one-task source+sink pipeline; every pipeline has a
      // real source (no sourceless pipeline exists).
      sirius_pipeline* fused_pipeline   = nullptr;
      sirius_physical_group_join* fused = nullptr;
      for (auto const& pipeline : result.scheduled_pipelines) {
        REQUIRE(pipeline->get_source() != nullptr);
        for (auto const& op_ref : pipeline->get_operators()) {
          if (op_ref.get().type != SiriusPhysicalOperatorType::GROUP_JOIN) { continue; }
          REQUIRE(fused_pipeline == nullptr);
          fused_pipeline = pipeline.get();
          fused          = &op_ref.get().Cast<sirius_physical_group_join>();
        }
      }
      REQUIRE(fused_pipeline != nullptr);
      REQUIRE(fused != nullptr);
      auto const operators = fused_pipeline->get_operators();
      REQUIRE(operators.size() == 1);
      CHECK(fused_pipeline->get_source().get() == fused);
      CHECK(fused_pipeline->get_sink().get() == fused);
      REQUIRE(fused->children.size() == 1);

      // Exactly one FULL input wiring exists: the opaque child subtree onto "counted".
      repository_wiring const* counted = nullptr;
      for (auto const& wiring : result.repository_wirings) {
        if (wiring.dest_pipeline.get() != fused_pipeline) { continue; }
        REQUIRE(wiring.port_id == sirius_physical_group_join::COUNTED_PORT);
        REQUIRE(counted == nullptr);
        counted = &wiring;
      }
      REQUIRE(counted != nullptr);
      CHECK(counted->barrier_type == MemoryBarrierType::FULL);
      CHECK(counted->source_op == fused->children[0].get());
      REQUIRE(counted->source_pipeline);
      CHECK(counted->source_pipeline->get_sink().get() == fused->children[0].get());

      // The single task fires only after the child producer pipeline finishes.
      bool depends_on_counted = false;
      for (auto const& dependency : fused_pipeline->dependencies) {
        if (dependency.get() == counted->source_pipeline.get()) { depends_on_counted = true; }
      }
      CHECK(depends_on_counted);
    });
}

TEST_CASE("group_join recognizes host COUNT callbacks through a dynamically loaded extension",
          "[group_join][plan][dynamic_load]")
{
  scoped_temp_directory temp;
  auto const executable = std::filesystem::canonical("/proc/self/exe");
  auto const extension =
    executable.parent_path().parent_path().parent_path() / "sirius.duckdb_extension";
  auto const config = std::filesystem::path(SIRIUS_PROJECT_ROOT) / "test" / "cpp" / "config" /
                      "data" / "configurator_dense_count_join.yaml";
  REQUIRE(std::filesystem::is_regular_file(extension));
  REQUIRE(std::filesystem::is_regular_file(config));

  static constexpr std::string_view script = R"PY(
import os
import sys
from pathlib import Path

extension, config, temp_root = sys.argv[1:]
root = Path(temp_root)
logs = root / "logs"
logs.mkdir()

os.environ.pop("SIRIUS_DISABLE", None)
os.environ["SIRIUS_CONFIG_FILE"] = config
os.environ["SIRIUS_LOG_DIR"] = str(logs)
os.environ["SIRIUS_LOG_BACKEND"] = "spdlog"
os.environ["SIRIUS_LOG_LEVEL"] = "info"

import duckdb

def sql_literal(value):
    return "'" + str(value).replace("'", "''") + "'"

customer_path = root / "customer.parquet"
orders_path = root / "orders.parquet"
con = duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})
con.execute(
    f"COPY (SELECT range::INTEGER AS c_custkey FROM range(9)) "
    f"TO {sql_literal(customer_path)} (FORMAT PARQUET)"
)
con.execute(
    "COPY (SELECT range::BIGINT AS o_orderkey, "
    "             (range % 8)::INTEGER AS o_custkey, "
    "             CASE WHEN range % 5 = 0 THEN 'special x requests' ELSE 'ordinary' END AS o_comment "
    "      FROM range(64)) "
    f"TO {sql_literal(orders_path)} (FORMAT PARQUET)"
)
con.execute(
    f"CREATE VIEW customer AS SELECT * FROM read_parquet([{sql_literal(customer_path)}])"
)
con.execute(
    f"CREATE VIEW orders AS SELECT * FROM read_parquet([{sql_literal(orders_path)}])"
)

con.execute(f"LOAD {sql_literal(extension)}")
con.execute("SET gpu_execution = true")
con.execute("SET enable_duckdb_fallback = false")
con.execute(
    f"CALL pin_table({sql_literal(customer_path)}, tier='host', "
    "name='customer', cols=['c_custkey'])"
).fetchall()
con.execute(
    f"CALL pin_table({sql_literal(orders_path)}, tier='host', "
    "name='orders', cols=['o_custkey','o_orderkey','o_comment'])"
).fetchall()

queries = [
    (
        "SELECT c_custkey, count(o_orderkey) FROM customer c LEFT JOIN orders o "
        "ON c.c_custkey = o.o_custkey AND o.o_comment NOT LIKE '%special%requests%' "
        "GROUP BY c_custkey ORDER BY c_custkey",
        [(0, 6), (1, 7), (2, 6), (3, 7), (4, 6), (5, 6), (6, 7), (7, 6), (8, 0)],
    ),
    (
        "SELECT c_custkey, count(*) FROM customer c LEFT JOIN orders o "
        "ON c.c_custkey = o.o_custkey AND o.o_comment NOT LIKE '%special%requests%' "
        "GROUP BY c_custkey ORDER BY c_custkey",
        [(0, 6), (1, 7), (2, 6), (3, 7), (4, 6), (5, 6), (6, 7), (7, 6), (8, 1)],
    ),
]
for query, expected in queries:
    actual = con.execute(query).fetchall()
    if actual != expected:
        raise AssertionError((query, actual, expected))
con.close()
)PY";

  auto const child_output_path = temp.path() / "python-output.txt";
  auto const child_output_fd =
    ::open(child_output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  REQUIRE(child_output_fd >= 0);

  auto const pid = ::fork();
  if (pid != 0) { ::close(child_output_fd); }
  REQUIRE(pid >= 0);
  if (pid == 0) {
    if (::dup2(child_output_fd, STDOUT_FILENO) < 0 || ::dup2(child_output_fd, STDERR_FILENO) < 0) {
      ::_exit(126);
    }
    ::close(child_output_fd);
    ::execlp("python",
             "python",
             "-c",
             script.data(),
             extension.c_str(),
             config.c_str(),
             temp.path().c_str(),
             static_cast<char*>(nullptr));
    ::_exit(127);
  }

  int status            = 0;
  int wait_error        = 0;
  pid_t waited          = 0;
  bool timed_out        = false;
  auto const deadline   = std::chrono::steady_clock::now() + std::chrono::seconds{120};
  auto const poll_delay = std::chrono::milliseconds{20};
  while (true) {
    waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid) { break; }
    if (waited < 0) {
      if (errno == EINTR) { continue; }
      wait_error = errno;
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      timed_out = true;
      ::kill(pid, SIGKILL);
      do {
        waited = ::waitpid(pid, &status, 0);
      } while (waited < 0 && errno == EINTR);
      break;
    }
    std::this_thread::sleep_for(poll_delay);
  }

  std::ifstream child_output_stream(child_output_path);
  std::string const child_output{std::istreambuf_iterator<char>{child_output_stream},
                                 std::istreambuf_iterator<char>{}};
  INFO("dynamic-load child output:\n" << child_output);
  REQUIRE_FALSE(timed_out);
  REQUIRE(wait_error == 0);
  REQUIRE(waited == pid);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  std::string log_text;
  for (auto const& entry : std::filesystem::directory_iterator(temp.path() / "logs")) {
    if (!entry.is_regular_file()) { continue; }
    std::ifstream log_stream(entry.path());
    log_text.append(std::istreambuf_iterator<char>{log_stream}, std::istreambuf_iterator<char>{});
    log_text.push_back('\n');
  }

  static constexpr std::string_view marker = "Fusing COUNT-join into GROUP_JOIN";
  std::vector<std::string> fusion_lines;
  std::istringstream log_lines{log_text};
  for (std::string line; std::getline(log_lines, line);) {
    if (line.find(marker) != std::string::npos) { fusion_lines.push_back(std::move(line)); }
  }

  INFO("dynamic-load Sirius logs:\n" << log_text);
  REQUIRE(fusion_lines.size() >= 2);
  bool saw_count_column = false;
  bool saw_count_star   = false;
  for (auto const& line : fusion_lines) {
    saw_count_column = saw_count_column || line.find("COUNT(col ") != std::string::npos;
    saw_count_star   = saw_count_star || line.find("COUNT(*)") != std::string::npos;
  }
  CHECK(saw_count_column);
  CHECK(saw_count_star);
}
