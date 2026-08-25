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

#include <catch.hpp>
#include <duckdb.hpp>
#include <utils/dynamic_filter_test_utils.hpp>
#include <utils/gpu_execution_fixture.hpp>
#include <utils/log_test_utils.hpp>
#include <utils/scoped_sirius_setting.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace {

class GroupJoinFixture : public sirius::test::GpuExecutionFixture {
 public:
  GroupJoinFixture()
  {
    enable_guard.emplace(*con, "enable_dense_count_join", true);
    run_ok("CREATE TABLE cust (c_id INTEGER, c_grp INTEGER);");
    run_ok(
      "INSERT INTO cust VALUES (1, 0), (2, 1), (3, 0), (3, 1), (4, 0), (5, 1), (6, 0), (7, 1), "
      "(8, 0), (NULL, 0), (NULL, 1);");
    run_ok("CREATE TABLE ord (o_id BIGINT, o_cust INTEGER, o_val INTEGER);");
    run_ok(
      "INSERT INTO ord VALUES (100, 2, 10), (101, 2, NULL), (102, 3, 11), (103, 5, 12), "
      "(104, 5, NULL), (105, 5, 13), (106, 42, 14), (107, NULL, 15);");
    run_ok("CHECKPOINT;");
  }

  std::optional<sirius::test::scoped_sirius_setting> enable_guard;
};

}  // namespace

TEST_CASE_METHOD(GroupJoinFixture,
                 "gpu_execution group join: COUNT(col) grouped by the LEFT-join key",
                 "[integration][gpu_execution][group_join]")
{
  compare_gpu_vs_cpu(
    "SELECT c_id, count(o_id) AS c_count FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY c_id");
}

TEST_CASE_METHOD(GroupJoinFixture,
                 "gpu_execution group join: COUNT(*) and nullable COUNT(col) semantics",
                 "[integration][gpu_execution][group_join]")
{
  compare_gpu_vs_cpu(
    "SELECT c_id, count(*) AS c_count FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY c_id");
  compare_gpu_vs_cpu(
    "SELECT c_id, count(o_val) AS c_count FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY "
    "c_id");
}

TEST_CASE_METHOD(GroupJoinFixture,
                 "gpu_execution group join: RIGHT-join orientation",
                 "[integration][gpu_execution][group_join]")
{
  compare_gpu_vs_cpu(
    "SELECT c_id, count(o_id) AS c_count FROM ord RIGHT JOIN cust ON o_cust = c_id GROUP BY "
    "c_id");
}

TEST_CASE_METHOD(GroupJoinFixture,
                 "gpu_execution group join: full q13 distribution shape with ORDER BY",
                 "[integration][gpu_execution][group_join]")
{
  compare_gpu_vs_cpu_ordered(
    "SELECT c_count, count(*) AS custdist FROM ("
    "  SELECT c_id, count(o_id) AS c_count FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY "
    "c_id"
    ") t GROUP BY c_count ORDER BY custdist DESC, c_count DESC");
}

TEST_CASE_METHOD(GroupJoinFixture,
                 "gpu_execution group join: sparse strategy under a tiny histogram budget",
                 "[integration][gpu_execution][group_join]")
{
  sirius::test::scoped_sirius_setting budget{*con, "dense_count_join_max_bytes", std::uint64_t{8}};
  compare_gpu_vs_cpu(
    "SELECT c_id, count(o_id) AS c_count FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY c_id");
  compare_gpu_vs_cpu(
    "SELECT c_id, count(*) AS c_count FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY c_id");
}

TEST_CASE_METHOD(GroupJoinFixture,
                 "gpu_execution group join: disabled knob keeps the join plan correct",
                 "[integration][gpu_execution][group_join]")
{
  sirius::test::scoped_sirius_setting disabled{*con, "enable_dense_count_join", false};
  compare_gpu_vs_cpu(
    "SELECT c_id, count(o_id) AS c_count FROM cust LEFT JOIN ord ON c_id = o_cust GROUP BY c_id");
}

TEST_CASE_METHOD(GroupJoinFixture,
                 "gpu_execution group join: scoped settings restore during unwinding",
                 "[integration][gpu_execution][group_join]")
{
  auto sirius_ctx          = sirius::test::get_registered_sirius_context(*con);
  auto const enable_before = sirius_ctx->get_config().get_operator_params().enable_dense_count_join;
  auto const budget_before =
    sirius_ctx->get_config().get_operator_params().dense_count_join_max_bytes;

  struct forced_unwind {};
  try {
    sirius::test::scoped_sirius_setting disabled{*con, "enable_dense_count_join", false};
    sirius::test::scoped_sirius_setting budget{
      *con, "dense_count_join_max_bytes", std::uint64_t{8}};
    throw forced_unwind{};
  } catch (forced_unwind const&) {
  }

  CHECK(sirius_ctx->get_config().get_operator_params().enable_dense_count_join == enable_before);
  CHECK(sirius_ctx->get_config().get_operator_params().dense_count_join_max_bytes == budget_before);
}

TEST_CASE_METHOD(GroupJoinFixture,
                 "gpu_execution group join: value-form state budget test hook round-trips",
                 "[integration][gpu_execution][group_join]")
{
  // The knob is engine-owned and consumed by later planner rungs; this pins the SQL test-hook
  // plumbing (set, engine-visible value, scope restore).
  auto sirius_ctx = sirius::test::get_registered_sirius_context(*con);
  auto const budget_before =
    sirius_ctx->get_config().get_operator_params().group_join_max_state_bytes;
  {
    sirius::test::scoped_sirius_setting budget{
      *con, "group_join_max_state_bytes", std::uint64_t{12345}};
    CHECK(sirius_ctx->get_config().get_operator_params().group_join_max_state_bytes == 12345);
  }
  CHECK(sirius_ctx->get_config().get_operator_params().group_join_max_state_bytes == budget_before);
}

TEST_CASE_METHOD(GroupJoinFixture,
                 "gpu_execution group join: runtime-empty sides",
                 "[integration][gpu_execution][group_join]")
{
  // Keep scans nonempty at plan time so filters produce empty inputs at runtime.
  compare_gpu_vs_cpu(
    "SELECT c_id, count(o.o_id) AS c_count FROM cust "
    "LEFT JOIN (SELECT * FROM ord WHERE o_val > 1000000) o ON c_id = o.o_cust GROUP BY c_id");
  compare_gpu_vs_cpu(
    "SELECT c.c_id, count(o_id) AS c_count FROM (SELECT * FROM cust WHERE c_grp > 1000000) c "
    "LEFT JOIN ord ON c.c_id = o_cust GROUP BY c.c_id");
  compare_gpu_vs_cpu(
    "SELECT c.c_id, count(o.o_id) AS c_count FROM (SELECT * FROM cust WHERE c_grp > 1000000) c "
    "LEFT JOIN (SELECT * FROM ord WHERE o_val > 1000000) o ON c.c_id = o.o_cust "
    "GROUP BY c.c_id");
}

namespace {

// Rung P1 fixture: enables the value rungs and provides a q17-shaped corpus. line_t carries
// exactly 16 rows per l_p key with quarter-valued decimals, so every correlated threshold used
// below is provably never exactly integral -- CPU/GPU floating-point averages then always land on
// the same side of the integer quantities they are compared against.
class GroupJoinInnerFixture : public sirius::test::GpuExecutionFixture {
 public:
  GroupJoinInnerFixture()
  {
    enable_guard.emplace(*con, "enable_group_join", true);
    run_ok("CREATE TABLE part_t (p_id INTEGER, p_size INTEGER);");
    run_ok("INSERT INTO part_t SELECT range, range % 5 FROM range(12);");
    run_ok("CREATE TABLE line_t (l_p INTEGER, l_qty INTEGER, l_dec DECIMAL(15,2));");
    run_ok("INSERT INTO line_t SELECT range % 15, range % 9, (range % 50) / 4.0 FROM range(240);");
    run_ok("CREATE TABLE dim_dense (d INTEGER PRIMARY KEY);");
    run_ok("INSERT INTO dim_dense SELECT range FROM range(10000);");
    run_ok("CREATE TABLE fact_dense (fd INTEGER, fv INTEGER);");
    run_ok("INSERT INTO fact_dense SELECT range % 10000, range % 97 FROM range(40000);");
    run_ok("CHECKPOINT;");
  }

  // The q17 shape over the fixture corpus: AVG per delim-preserved partkey under an INNER
  // correlation join, feeding an outer decimal SUM. The outer correlated table carries a filter
  // like q17's part table -- without one DuckDB's deliminator rewrites the delim join away.
  std::string q17_shape(std::string_view inner_aggregate) const
  {
    return "SELECT sum(l.l_dec) AS s FROM line_t l, part_t p WHERE p.p_id = l.l_p AND "
           "p.p_size < 3 AND l.l_qty < (SELECT " +
           std::string(inner_aggregate) + " FROM line_t l2 WHERE l2.l_p = p.p_id)";
  }

  static bool log_contains(sirius::test::recording_log_sink const& sink, std::string_view needle)
  {
    auto const records = sink.records();
    return std::any_of(records.begin(), records.end(), [&](auto const& record) {
      return record.message.find(needle) != std::string::npos;
    });
  }

  std::optional<sirius::test::scoped_sirius_setting> enable_guard;
};

}  // namespace

TEST_CASE_METHOD(GroupJoinInnerFixture,
                 "gpu_execution group join P1: q17-shaped AVG over the delim-preserved key",
                 "[integration][gpu_execution][group_join]")
{
  sirius::test::scoped_recording_log_sink log{"info"};
  compare_gpu_vs_cpu(q17_shape("0.2 * avg(l2.l_qty) + 1.03"));
  CHECK(log_contains(log.sink(), "Fusing INNER AVG into GROUP_JOIN"));
}

TEST_CASE_METHOD(GroupJoinInnerFixture,
                 "gpu_execution group join P1: q17-shaped value aggregates incl. DECIMAL args",
                 "[integration][gpu_execution][group_join]")
{
  // avg over the DECIMAL argument: the 16-rows-per-key corpus keeps the threshold away from the
  // integer quantities (see the fixture comment).
  compare_gpu_vs_cpu(q17_shape("0.2 * avg(l2.l_dec) + 1.03"));
  compare_gpu_vs_cpu(q17_shape("0.2 * sum(l2.l_qty) - 3.03"));
  // MIN/MAX over DECIMAL stay in exact fixed point end to end.
  compare_gpu_vs_cpu(q17_shape("min(l2.l_dec) + 2.25"));
  compare_gpu_vs_cpu(q17_shape("max(l2.l_dec) - 4.75"));
}

TEST_CASE_METHOD(GroupJoinInnerFixture,
                 "gpu_execution group join P1: empty delim buffer",
                 "[integration][gpu_execution][group_join]")
{
  // The outer filter is stats-opaque (a modulo), so the plan keeps its delim shape and fuses,
  // but zero correlation keys reach the preserved port at runtime.
  sirius::test::scoped_recording_log_sink log{"info"};
  compare_gpu_vs_cpu(
    "SELECT sum(l.l_dec) AS s FROM line_t l, part_t p WHERE p.p_size % 7 = 6 AND "
    "p.p_id = l.l_p AND l.l_qty < (SELECT 0.2 * avg(l2.l_qty) + 1.03 FROM line_t l2 "
    "WHERE l2.l_p = p.p_id)");
  REQUIRE(log_contains(log.sink(), "Fusing INNER AVG into GROUP_JOIN"));
}

TEST_CASE_METHOD(GroupJoinInnerFixture,
                 "gpu_execution group join P1: unique-preserved COUNT and dense-forcing MIN/MAX",
                 "[integration][gpu_execution][group_join]")
{
  compare_gpu_vs_cpu(
    "SELECT d, count(fv) AS c FROM dim_dense JOIN fact_dense ON d = fd GROUP BY d");

  // Dense-domain MIN/MAX reachability proof: no TPC-H query instantiates the dense
  // sentinel-init/atomicMin machinery, so rung P1 must be shown to reach it from SQL through the
  // full planner, asserted through the executor's strategy log.
  sirius::test::scoped_recording_log_sink log{"info"};
  compare_gpu_vs_cpu("SELECT d, min(fv) AS m FROM dim_dense JOIN fact_dense ON d = fd GROUP BY d");
  CHECK(log_contains(log.sink(), "Fusing INNER MIN into GROUP_JOIN"));
  REQUIRE(log_contains(log.sink(), "INNER MIN dense path"));
  compare_gpu_vs_cpu("SELECT d, max(fv) AS m FROM dim_dense JOIN fact_dense ON d = fd GROUP BY d");
  REQUIRE(log_contains(log.sink(), "INNER MAX dense path"));
}

TEST_CASE_METHOD(GroupJoinInnerFixture,
                 "gpu_execution group join P1: counted-side byte gate declines to the generic "
                 "plan",
                 "[integration][gpu_execution][group_join]")
{
  sirius::test::scoped_sirius_setting tiny_gate{
    *con, "group_join_counted_bytes_gate", std::uint64_t{1}};
  sirius::test::scoped_recording_log_sink log{"info"};
  compare_gpu_vs_cpu(q17_shape("0.2 * avg(l2.l_qty) + 1.03"));
  REQUIRE(log_contains(log.sink(), "GROUP_JOIN INNER fusion declined: counted child estimate"));
  CHECK_FALSE(log_contains(log.sink(), "Fusing INNER AVG into GROUP_JOIN"));
}

TEST_CASE_METHOD(GroupJoinInnerFixture,
                 "gpu_execution group join P1: preserved-key membership publication parity",
                 "[integration][gpu_execution][group_join]")
{
  auto const query = q17_shape("0.2 * avg(l2.l_qty) + 1.03");

  // Fused: the GROUP_JOIN publishes the preserved (delim) keys to the counted line_t scan.
  auto const before_fused = sirius::test::get_dynamic_filter_stats_snapshot(*con);
  compare_gpu_vs_cpu(query);
  auto const after_fused = sirius::test::get_dynamic_filter_stats_snapshot(*con);
  REQUIRE(after_fused.publication_attempts > before_fused.publication_attempts);
  REQUIRE(after_fused.publications_finished > before_fused.publications_finished);
  REQUIRE(after_fused.filters_pushed > before_fused.filters_pushed);
  REQUIRE(after_fused.publications_failed == before_fused.publications_failed);

  // Unfused baseline: the replaced hash join publishes the same membership filter.
  sirius::test::scoped_sirius_setting knob_off{*con, "enable_group_join", false};
  auto const before_baseline = sirius::test::get_dynamic_filter_stats_snapshot(*con);
  compare_gpu_vs_cpu(query);
  auto const after_baseline = sirius::test::get_dynamic_filter_stats_snapshot(*con);
  CHECK(after_baseline.filters_pushed > before_baseline.filters_pushed);
}
