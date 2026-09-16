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

// Single-GPU, isolated-context test of the pinned-registry evidence behind the dynamic-filter
// domain-coverage gate. A parquet build table carries no constraints, so the gate can only learn
// that a key is unique from `pin_table(..., unique_cols => [...])`, and it learns the key's domain
// from the pinned entry's exact row count. The test pins a build table with a declared unique key
// and runs a filtered join whose build covers 60 % of that key's domain:
//   - under the default `catalog_and_pinned` evidence the key is skipped before any filter is
//     built (keys_with_known_domain and keys_skipped_domain_gate advance, no membership filter);
//   - under `catalog_only` the same query publishes a membership filter and knows no domain;
//   - under `catalog_and_pinned` with a threshold above the coverage the domain is known but the
//     key is kept, so the decision is the coverage test and not the evidence alone.
// It also gates the declaration surface: bind-time rejection of a column outside `cols`,
// pre-materialization rejection of a column outside the schema, and union semantics across a
// same-row-count re-pin merge.

#include "scan_manager/sirius_scan_manager.hpp"
#include "sirius_context.hpp"

#include <catch.hpp>
#include <duckdb.hpp>
#include <unistd.h>
#include <utils/sirius_test_env.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::int64_t kBuildRows  = 100'000;
constexpr std::int64_t kProbeRows  = 2'000'000;  // every build key appears 20 times
constexpr std::int64_t kBuildLimit = 60'000;     // WHERE v < 60000 keeps 60 % of the domain

// A throwaway, Sirius-disabled DuckDB writes the parquet so the extension callback does not
// build a SiriusContext on it — the real instance is created later from the yaml.
void generate_parquet(fs::path const& build_path, fs::path const& probe_path)
{
  setenv("SIRIUS_DISABLE", "1", 1);
  {
    duckdb::DuckDB gen_db(nullptr);
    duckdb::Connection gen(gen_db);
    auto build = gen.Query("COPY (SELECT range AS k, range AS v, range * 3 AS w FROM range(" +
                           std::to_string(kBuildRows) + ")) TO '" + build_path.string() +
                           "' (FORMAT PARQUET);");
    REQUIRE(build);
    REQUIRE_FALSE(build->HasError());
    auto probe = gen.Query("COPY (SELECT (range % " + std::to_string(kBuildRows) +
                           ") AS k FROM range(" + std::to_string(kProbeRows) + ")) TO '" +
                           probe_path.string() + "' (FORMAT PARQUET);");
    REQUIRE(probe);
    REQUIRE_FALSE(probe->HasError());
  }
  unsetenv("SIRIUS_DISABLE");
}

// Single-GPU config with a generous budget for the tiny fixture. Dynamic filters keep their
// engine defaults (enabled, catalog_and_pinned evidence, threshold 0.45).
void write_config(fs::path const& yaml_path)
{
  std::ofstream f(yaml_path);
  f << "sirius:\n"
       "  topology:\n"
       "    num_gpus: 1\n"
       "  memory:\n"
       "    gpu:\n"
       "      usage_limit_fraction: 0.4\n"
       "      reservation_limit_fraction: 1.0\n"
       "    host:\n"
       "      capacity_bytes: 32000000000\n"
       "      initial_number_pools: 10\n"
       "      pool_size: 512\n"
       "      block_size: 1048576\n"
       "  executor:\n"
       "    pipeline:\n"
       "      num_threads: 4\n"
       "    task_creator:\n"
       "      num_threads: 2\n"
       "    downgrade:\n"
       "      num_threads: 1\n"
       "      monitor_period: 10ms\n"
       "  operator_params:\n"
       "    scan_task_batch_size: 100000000\n"
       "    max_sort_partition_bytes: 0\n"
       "    hash_partition_bytes: 100000000\n"
       "    concat_batch_bytes: 100000000\n"
       "    max_build_hash_table_bytes: 90000000\n";
}

void require_ok(duckdb::unique_ptr<duckdb::MaterializedQueryResult> const& result,
                std::string const& what)
{
  REQUIRE(result);
  if (result->HasError()) { UNSCOPED_INFO(what << " error: " << result->GetError()); }
  REQUIRE_FALSE(result->HasError());
}

sirius::scan_manager::pinned_entry const* find_entry(
  sirius::scan_manager::sirius_scan_manager const& mgr, std::string_view wanted)
{
  sirius::scan_manager::pinned_entry const* found = nullptr;
  mgr.visit_pinned_entries([&](std::string_view name, auto const& entry) {
    if (name != wanted) { return true; }
    found = &entry;
    return false;
  });
  return found;
}

}  // namespace

// NB: no [integration]/[shared_context] tag — those make the Catch2 listener bind a shared env,
// which would fight this test's own local_env (see test_pin_table_merge_columns.cpp).
TEST_CASE("pin_table unique_cols feed the dynamic-filter domain-coverage gate on parquet",
          "[dynamic_filter][pin_table][domain_gate]")
{
  if (sirius::test::g_shared_env && sirius::test::g_shared_env->is_active()) {
    sirius::test::g_shared_env->pause();
  }
  if (sirius::test::g_integration_env && sirius::test::g_integration_env->is_active()) {
    sirius::test::g_integration_env->pause();
  }
  if (sirius::test::g_integration_env_2gpu && sirius::test::g_integration_env_2gpu->is_active()) {
    sirius::test::g_integration_env_2gpu->pause();
  }

  auto tmp = fs::temp_directory_path() / ("sirius-unique-cols-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp);

  auto const build_path = tmp / "build.parquet";
  auto const probe_path = tmp / "probe.parquet";
  generate_parquet(build_path, probe_path);

  auto const yaml_path = tmp / "unique_cols.yaml";
  write_config(yaml_path);
  REQUIRE(fs::exists(yaml_path));

  {
    sirius::test::shared_test_env local_env(yaml_path);
    auto con = local_env.make_connection();
    require_ok(con.Query("SET enable_duckdb_fallback = false;"), "fallback off");

    auto sirius_ctx = con.context->registered_state->Get<duckdb::SiriusContext>("sirius_state");
    REQUIRE(sirius_ctx != nullptr);
    auto const& mgr = sirius_ctx->get_scan_manager();

    // --- Declaration surface -------------------------------------------------------------
    auto outside_cols =
      con.Query("CALL pin_table('" + build_path.string() +
                "', tier='gpu', name='bad_cols', cols=['k'], unique_cols=['v']);");
    REQUIRE(outside_cols);
    REQUIRE(outside_cols->HasError());
    REQUIRE(outside_cols->GetError().find("not in the pinned 'cols' list") != std::string::npos);
    REQUIRE(find_entry(mgr, "bad_cols") == nullptr);

    auto outside_schema = con.Query("CALL pin_table('" + build_path.string() +
                                    "', tier='gpu', name='bad_schema', unique_cols=['nope']);");
    REQUIRE(outside_schema);
    REQUIRE(outside_schema->HasError());
    REQUIRE(outside_schema->GetError().find("nope") != std::string::npos);
    REQUIRE(find_entry(mgr, "bad_schema") == nullptr);  // rejected before materialization

    require_ok(con.Query("CALL pin_table('" + build_path.string() +
                         "', tier='gpu', name='build', cols=['k', 'v'], unique_cols=['k']);"),
               "pin build");
    {
      auto const* entry = find_entry(mgr, "build");
      REQUIRE(entry != nullptr);
      REQUIRE(entry->num_rows == static_cast<std::size_t>(kBuildRows));
      REQUIRE(entry->declared_unique_columns == std::vector<std::string>{"k"});
      REQUIRE(entry->is_declared_unique("k"));
      REQUIRE_FALSE(entry->is_declared_unique("v"));
    }

    // --- The gate on a filtered join -----------------------------------------------------
    std::string const query = "SELECT count(*) FROM read_parquet('" + probe_path.string() +
                              "') p JOIN read_parquet('" + build_path.string() +
                              "') b ON p.k = b.k WHERE b.v < " + std::to_string(kBuildLimit) + ";";
    std::string const expected_count =
      std::to_string(kBuildLimit * (kProbeRows / kBuildRows));  // 60000 keys x 20 probe rows

    auto run_and_measure = [&](std::string const& label) {
      auto const before = sirius_ctx->get_dynamic_filter_stats_snapshot();
      auto result       = con.Query(query);
      require_ok(result, label);
      REQUIRE(result->GetValue(0, 0).ToString() == expected_count);
      auto const after = sirius_ctx->get_dynamic_filter_stats_snapshot();
      struct delta {
        std::uint64_t considered, known_domain, skipped_domain, membership_built;
      };
      return delta{after.keys_considered - before.keys_considered,
                   after.keys_with_known_domain - before.keys_with_known_domain,
                   after.keys_skipped_domain_gate - before.keys_skipped_domain_gate,
                   after.membership_filters_built - before.membership_filters_built};
    };

    {
      // Default evidence: pinned row count (domain 100k) + declared key; build 60k covers 0.6
      // >= 0.45, so the key is skipped before any filter is built.
      auto const d = run_and_measure("catalog_and_pinned");
      INFO("considered=" << d.considered << " known=" << d.known_domain
                         << " skipped=" << d.skipped_domain << " built=" << d.membership_built);
      REQUIRE(d.considered >= 1);
      REQUIRE(d.known_domain >= 1);
      REQUIRE(d.skipped_domain == d.considered);
      REQUIRE(d.membership_built == 0);
    }
    {
      // Catalog-only evidence: parquet has no catalog identity, so the domain stays unknown and
      // the filter is published.
      require_ok(con.Query("SET dynamic_filter_domain_evidence = 'catalog_only';"),
                 "set catalog_only");
      auto const d = run_and_measure("catalog_only");
      INFO("considered=" << d.considered << " known=" << d.known_domain
                         << " skipped=" << d.skipped_domain << " built=" << d.membership_built);
      REQUIRE(d.considered >= 1);
      REQUIRE(d.known_domain == 0);
      REQUIRE(d.skipped_domain == 0);
      REQUIRE(d.membership_built >= 1);
    }
    {
      // Evidence back on, threshold above the coverage: the domain is known but the key stays.
      require_ok(con.Query("SET dynamic_filter_domain_evidence = 'catalog_and_pinned';"),
                 "set catalog_and_pinned");
      require_ok(con.Query("SET dynamic_filter_domain_coverage_threshold = 0.7;"),
                 "set threshold 0.7");
      auto const d = run_and_measure("threshold above coverage");
      INFO("considered=" << d.considered << " known=" << d.known_domain
                         << " skipped=" << d.skipped_domain << " built=" << d.membership_built);
      REQUIRE(d.considered >= 1);
      REQUIRE(d.known_domain == d.considered);
      REQUIRE(d.skipped_domain == 0);
      REQUIRE(d.membership_built >= 1);
      require_ok(con.Query("SET dynamic_filter_domain_coverage_threshold = 0.45;"),
                 "restore threshold");
    }
    {
      // An unknown evidence source is rejected at the SET surface.
      auto bad = con.Query("SET dynamic_filter_domain_evidence = 'pinned_only';");
      REQUIRE(bad);
      REQUIRE(bad->HasError());
    }

    // --- Merge semantics -----------------------------------------------------------------
    // Same file, same row count, a new column: the merge unions the declarations.
    require_ok(con.Query("CALL pin_table('" + build_path.string() +
                         "', tier='gpu', name='build', cols=['k', 'w'], unique_cols=['w']);"),
               "re-pin build with w");
    {
      auto const* entry = find_entry(mgr, "build");
      REQUIRE(entry != nullptr);
      REQUIRE(entry->declared_unique_columns == std::vector<std::string>{"k", "w"});
    }
    // A fresh pin after unpin starts from the new pin's list.
    require_ok(con.Query("CALL unpin_table('build');"), "unpin build");
    require_ok(con.Query("CALL pin_table('" + build_path.string() +
                         "', tier='gpu', name='build', cols=['k']);"),
               "re-pin build without declarations");
    {
      auto const* entry = find_entry(mgr, "build");
      REQUIRE(entry != nullptr);
      REQUIRE(entry->declared_unique_columns.empty());
    }
    require_ok(con.Query("CALL unpin_table('build');"), "unpin build");
  }

  fs::remove_all(tmp, ec);
}
