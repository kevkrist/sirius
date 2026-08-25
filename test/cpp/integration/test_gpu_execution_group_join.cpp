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
                 "gpu_execution group join: disabled knobs keep the generic join plan correct",
                 "[integration][gpu_execution][group_join]")
{
  // Disable the value rungs too: rung P2 would otherwise pick this shape up as DIRECT, and the
  // intent here is the fully generic join+aggregate plan.
  sirius::test::scoped_sirius_setting disabled{*con, "enable_dense_count_join", false};
  sirius::test::scoped_sirius_setting value_rungs_off{*con, "enable_group_join", false};
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
    // NULL-bearing corpus for the SQL-level NULL rows: NULL counted keys (never match), a group
    // whose key matches carry only NULL arguments (kept with SUM NULL / COUNT 0), and organic
    // NULL arguments that trip the argument-validity gate into the sparse strategy.
    run_ok("CREATE TABLE dim_n (dn INTEGER PRIMARY KEY);");
    run_ok("INSERT INTO dim_n SELECT range FROM range(8);");
    run_ok("CREATE TABLE fact_n (fn INTEGER, nv INTEGER);");
    run_ok(
      "INSERT INTO fact_n VALUES (1, 10), (1, NULL), (2, NULL), (2, NULL), (3, 5), (NULL, 7), "
      "(NULL, NULL), (9, 3);");
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

TEST_CASE_METHOD(GroupJoinInnerFixture,
                 "gpu_execution group join P1: NULL keys and NULL arguments through the "
                 "argument-validity gate",
                 "[integration][gpu_execution][group_join]")
{
  // The corpus carries NULL counted keys (never match), a group whose matches have only NULL
  // arguments (row kept, SUM NULL / COUNT 0), and organic NULL arguments -- so the fused task
  // must take the mask-preserving sparse strategy.
  sirius::test::scoped_recording_log_sink log{"info"};
  compare_gpu_vs_cpu("SELECT dn, sum(nv) AS s FROM dim_n JOIN fact_n ON dn = fn GROUP BY dn");
  CHECK(log_contains(log.sink(), "Fusing INNER SUM into GROUP_JOIN"));
  REQUIRE(log_contains(log.sink(), "INNER SUM sparse path"));
  compare_gpu_vs_cpu("SELECT dn, count(nv) AS c FROM dim_n JOIN fact_n ON dn = fn GROUP BY dn");
  REQUIRE(log_contains(log.sink(), "INNER COUNT_VALID sparse path"));
  compare_gpu_vs_cpu("SELECT dn, avg(nv) AS a FROM dim_n JOIN fact_n ON dn = fn GROUP BY dn");
}

namespace {

// Rung P2 fixture: DIRECT-form corpora. ps_t's group values are sparse in a wide domain (the q2
// regime); dj_t's group values densely cover a small range and include NULL group keys (the
// dense-forcing regime); cust_t/ord_t build outer-join padding.
class GroupJoinDirectFixture : public sirius::test::GpuExecutionFixture {
 public:
  GroupJoinDirectFixture()
  {
    enable_guard.emplace(*con, "enable_group_join", true);
    run_ok("CREATE TABLE sup_t (s_id INTEGER);");
    run_ok("INSERT INTO sup_t SELECT range FROM range(10);");
    run_ok("CREATE TABLE ps_t (ps_s INTEGER, ps_p INTEGER, ps_cost DECIMAL(15,2));");
    run_ok(
      "INSERT INTO ps_t SELECT range % 12, (range % 40) * 100000, (range % 90) / 4.0 "
      "FROM range(240);");
    run_ok("CREATE TABLE dk_t (k INTEGER);");
    run_ok("INSERT INTO dk_t SELECT range FROM range(10);");
    run_ok("CREATE TABLE dj_t (j INTEGER, g INTEGER, v INTEGER);");
    run_ok(
      "INSERT INTO dj_t SELECT range % 10, CASE WHEN range % 97 = 0 THEN NULL "
      "ELSE range % 500 END, range % 37 FROM range(4000);");
    run_ok("CREATE TABLE cust_t (c INTEGER);");
    run_ok("INSERT INTO cust_t VALUES (1), (2), (3), (4), (NULL);");
    run_ok("CREATE TABLE ord_t (oc INTEGER, amt INTEGER, dec_amt DECIMAL(15,2));");
    run_ok("INSERT INTO ord_t VALUES (1, 10, 1.25), (1, 5, 2.50), (3, 7, 0.75), (7, 9, 3.00);");
    run_ok("CHECKPOINT;");
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

TEST_CASE_METHOD(GroupJoinDirectFixture,
                 "gpu_execution group join P2: q2-shaped MIN over an opaque join runs sparse "
                 "inside the fused task",
                 "[integration][gpu_execution][group_join]")
{
  // The q2 shape: MIN over a DECIMAL argument, grouped by a column that is not a join key of the
  // opaque INNER-join child. Its sparse group domain makes the runtime gates pick the sparse
  // strategy inside the single fused task -- q2's own regime by design.
  auto const query =
    "SELECT ps_p, min(ps_cost) AS m FROM sup_t JOIN ps_t ON s_id = ps_s GROUP BY ps_p";
  {
    sirius::test::scoped_recording_log_sink log{"info"};
    compare_gpu_vs_cpu(query);
    CHECK(log_contains(log.sink(), "Fusing DIRECT MIN into GROUP_JOIN"));
    REQUIRE(log_contains(log.sink(), "DIRECT MIN sparse path"));
  }
  // Feature-off oracle: the same query through generic planning agrees with the CPU too.
  {
    sirius::test::scoped_sirius_setting knob_off{*con, "enable_group_join", false};
    sirius::test::scoped_recording_log_sink log{"info"};
    compare_gpu_vs_cpu(query);
    CHECK_FALSE(log_contains(log.sink(), "Fusing DIRECT"));
  }
}

TEST_CASE_METHOD(GroupJoinDirectFixture,
                 "gpu_execution group join P2: dense-forcing DIRECT reachability",
                 "[integration][gpu_execution][group_join]")
{
  // The reachability proof for the dense DIRECT machinery (sentinel-init MIN, atomicMin, and the
  // NULL-group slot at index `range`): a dense-domain group key with NULL keys, planned through
  // the FULL planner from SQL, must select the dense strategy.
  auto const query = "SELECT g, min(v) AS m FROM dk_t JOIN dj_t ON k = j GROUP BY g";
  {
    sirius::test::scoped_recording_log_sink log{"info"};
    compare_gpu_vs_cpu(query);
    CHECK(log_contains(log.sink(), "Fusing DIRECT MIN into GROUP_JOIN"));
    CHECK_FALSE(log_contains(log.sink(), "DIRECT MIN sparse path"));
    REQUIRE(log_contains(log.sink(), "DIRECT MIN dense path"));
  }
  // Domain-cap bail: a forced tiny state budget flips the same query to the exact sparse
  // strategy with identical results.
  {
    sirius::test::scoped_sirius_setting budget{
      *con, "group_join_max_state_bytes", std::uint64_t{8}};
    sirius::test::scoped_recording_log_sink log{"info"};
    compare_gpu_vs_cpu(query);
    REQUIRE(log_contains(log.sink(), "DIRECT MIN sparse path"));
  }
  // The remaining dense bundles over the same dense domain.
  sirius::test::scoped_recording_log_sink log{"info"};
  compare_gpu_vs_cpu("SELECT g, sum(v) AS s FROM dk_t JOIN dj_t ON k = j GROUP BY g");
  REQUIRE(log_contains(log.sink(), "DIRECT SUM dense path"));
  compare_gpu_vs_cpu("SELECT g, max(v) AS s FROM dk_t JOIN dj_t ON k = j GROUP BY g");
  REQUIRE(log_contains(log.sink(), "DIRECT MAX dense path"));
  compare_gpu_vs_cpu("SELECT g, avg(v) AS a FROM dk_t JOIN dj_t ON k = j GROUP BY g");
  REQUIRE(log_contains(log.sink(), "DIRECT AVG dense path"));
  // count(v) over the provably NULL-free v is rewritten to count(*) by the optimizer's stats
  // pass, so both count queries reach the COUNT_STAR bundle; COUNT_VALID's dense path is covered
  // at the operator level.
  compare_gpu_vs_cpu("SELECT g, count(v) AS c FROM dk_t JOIN dj_t ON k = j GROUP BY g");
  compare_gpu_vs_cpu("SELECT g, count(*) AS c FROM dk_t JOIN dj_t ON k = j GROUP BY g");
  REQUIRE(log_contains(log.sink(), "DIRECT COUNT_STAR dense path"));
}

TEST_CASE_METHOD(GroupJoinDirectFixture,
                 "gpu_execution group join P2: outer-join padding NULLs route to the sparse "
                 "strategy",
                 "[integration][gpu_execution][group_join]")
{
  // DIRECT over a LEFT JOIN child (the opaque-child soundness case): padded groups carry NULL
  // arguments, the argument-validity gate fires, and the sparse strategy emits SUM/MIN = NULL
  // for them -- including the NULL-key group.
  sirius::test::scoped_recording_log_sink log{"info"};
  compare_gpu_vs_cpu("SELECT c, sum(amt) AS s FROM cust_t LEFT JOIN ord_t ON c = oc GROUP BY c");
  CHECK(log_contains(log.sink(), "Fusing DIRECT SUM into GROUP_JOIN"));
  REQUIRE(log_contains(log.sink(), "DIRECT SUM sparse path"));
  compare_gpu_vs_cpu(
    "SELECT c, min(dec_amt) AS m FROM cust_t LEFT JOIN ord_t ON c = oc GROUP BY c");
  REQUIRE(log_contains(log.sink(), "DIRECT MIN sparse path"));
  // COUNT over the padded argument: padding NULLs count as 0, and the NULL-key group is real.
  compare_gpu_vs_cpu("SELECT c, count(amt) AS n FROM cust_t LEFT JOIN ord_t ON c = oc GROUP BY c");
  // GROUP BY on the padded side's key: unmatched rows form the NULL group.
  compare_gpu_vs_cpu(
    "SELECT oc, count(amt) AS n FROM cust_t LEFT JOIN ord_t ON c = oc GROUP BY oc");
}

TEST_CASE_METHOD(GroupJoinDirectFixture,
                 "gpu_execution group join P2: runtime-empty child and byte-gate decline",
                 "[integration][gpu_execution][group_join]")
{
  // Keep scans nonempty at plan time so the filter produces an empty child at runtime: the fused
  // task never fires and the result is empty, exactly as the oracle's.
  compare_gpu_vs_cpu(
    "SELECT g, min(v) AS m FROM dk_t JOIN (SELECT * FROM dj_t WHERE v > 1000000) d "
    "ON k = d.j GROUP BY g");

  // A forced tiny child byte gate declines fusion to the generic plan.
  sirius::test::scoped_sirius_setting tiny_gate{
    *con, "group_join_counted_bytes_gate", std::uint64_t{1}};
  sirius::test::scoped_recording_log_sink log{"info"};
  compare_gpu_vs_cpu("SELECT g, min(v) AS m FROM dk_t JOIN dj_t ON k = j GROUP BY g");
  REQUIRE(log_contains(log.sink(), "GROUP_JOIN DIRECT fusion declined: child estimate"));
  CHECK_FALSE(log_contains(log.sink(), "Fusing DIRECT MIN into GROUP_JOIN"));
}
