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

#include "op/dynamic_filter/dynamic_filter_stats.hpp"
#include "s4_experiment_common.hpp"
#include "sirius_context.hpp"
#include "util/env_guard.hpp"
#include "utils/gpu_execution_fixture.hpp"

#include <duckdb/main/config.hpp>
#include <duckdb/parser/sql_statement.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <utility>

namespace sirius::test::s4 {
namespace {

using rows = std::vector<std::vector<std::string>>;

auto query(duckdb::Connection& connection, std::string const& sql)
{
  auto result = connection.Query(sql);
  if (!result || result->HasError()) {
    throw std::runtime_error(result ? result->GetError() : "query returned no result");
  }
  return result;
}

rows canonical_rows(duckdb::QueryResult& result)
{
  auto output = sirius::test::collect_rows(result, false);
  auto values = result.Collection().GetRows();
  for (duckdb::idx_t row = 0; row < output.size(); ++row) {
    for (duckdb::idx_t column = 0; column < output[row].size(); ++column) {
      // Preserve SQL NULL independently of a VARCHAR whose contents happen to be "NULL".
      output[row][column] = values.GetValue(column, row).IsNull() ? "N" : "V" + output[row][column];
    }
  }
  std::ranges::sort(output);
  return output;
}

std::string digest(rows const& values)
{
  std::uint64_t hash = 14695981039346656037ULL;
  auto byte          = [&](unsigned char value) { hash = (hash ^ value) * 1099511628211ULL; };
  auto length        = [&](std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      byte(static_cast<unsigned char>(value >> shift));
    }
  };
  length(values.size());
  for (auto const& row : values) {
    length(row.size());
    for (auto const& cell : row) {
      length(cell.size());
      for (unsigned char value : cell) {
        byte(value);
      }
    }
  }
  return std::to_string(hash);
}

std::uint64_t delta(std::uint64_t before, std::uint64_t after)
{
  if (after < before) { throw std::runtime_error("counter decreased across a query"); }
  return after - before;
}

void filter_counters(record& sample,
                     op::dynamic_filter_stats_snapshot const& before,
                     op::dynamic_filter_stats_snapshot const& after)
{
  using counters = op::dynamic_filter_stats_snapshot;
  struct field {
    char const* name;
    std::uint64_t counters::* value;
  };
  constexpr field fields[]{
    {"accumulations_started", &counters::accumulations_started},
    {"accumulation_expected_contributions", &counters::accumulation_expected_contributions},
    {"accumulation_completed_contributions", &counters::accumulation_completed_contributions},
    {"accumulations_skipped_inventory", &counters::accumulations_skipped_inventory},
    {"accumulations_skipped_admission", &counters::accumulations_skipped_admission},
    {"accumulations_skipped_cap", &counters::accumulations_skipped_cap},
    {"accumulations_skipped_resource", &counters::accumulations_skipped_resource},
    {"accumulations_skipped_error", &counters::accumulations_skipped_error},
    {"accumulations_skipped_transient", &counters::accumulations_skipped_transient},
    {"accumulations_incomplete", &counters::accumulations_incomplete},
    {"accumulations_abandoned", &counters::accumulations_abandoned},
    {"accumulation_storage_leaks", &counters::accumulation_storage_leaks},
    {"accumulation_publications_finished", &counters::accumulation_publications_finished},
    {"accumulation_publication_latency_ns", &counters::accumulation_publication_latency_ns},
    {"accumulation_input_bitmap_bytes", &counters::accumulation_input_bitmap_bytes},
    {"accumulation_ready_replica_bytes", &counters::accumulation_ready_replica_bytes},
    {"accumulation_published_bitmap_bytes", &counters::accumulation_published_bitmap_bytes},
    {"accumulation_admitted_keys", &counters::accumulation_admitted_keys},
    {"accumulation_contributing_gpus", &counters::accumulation_contributing_gpus},
    {"accumulation_prepare_to_completion_ns", &counters::accumulation_prepare_to_completion_ns},
    {"accumulation_direct_publications", &counters::accumulation_direct_publications},
    {"accumulation_host_publications", &counters::accumulation_host_publications},
    {"keys_skipped_bloom_size_gate", &counters::keys_skipped_bloom_size_gate},
    {"keys_skipped_domain_gate", &counters::keys_skipped_domain_gate},
    {"keys_skipped_type_mismatch", &counters::keys_skipped_type_mismatch},
    {"keys_build_exceeded_domain", &counters::keys_build_exceeded_domain},
    {"membership_filters_built", &counters::membership_filters_built},
    {"filters_pushed", &counters::filters_pushed}};
  for (auto const& field : fields) {
    sample.set(field.name, delta(before.*field.value, after.*field.value));
  }
  sample.set("input_bitmap_bytes",
             delta(before.accumulation_input_bitmap_bytes, after.accumulation_input_bitmap_bytes));
  sample.set(
    "ready_replica_bytes",
    delta(before.accumulation_ready_replica_bytes, after.accumulation_ready_replica_bytes));
  sample.set(
    "published_bitmap_bytes",
    delta(before.accumulation_published_bitmap_bytes, after.accumulation_published_bitmap_bytes));
  auto const direct =
    delta(before.accumulation_direct_publications, after.accumulation_direct_publications);
  auto const host =
    delta(before.accumulation_host_publications, after.accumulation_host_publications);
  sample.set("actual_transport", direct ? (host ? "mixed" : "direct") : (host ? "host" : "none"));
  auto const publications =
    delta(before.accumulation_publications_finished, after.accumulation_publications_finished);
  sample.set("publication_count", publications);
  sample.set("publication_measured", publications != 0);
  if (publications) {
    sample.set("host_elapsed_ns",
               delta(before.accumulation_prepare_to_completion_ns,
                     after.accumulation_prepare_to_completion_ns));
    sample.null("publication_skip_reason");
  } else {
    auto const* reason =
      after.accumulations_skipped_cap > before.accumulations_skipped_cap ? "cap_refused"
      : after.accumulations_skipped_resource > before.accumulations_skipped_resource
        ? "resource_refused"
      : after.keys_skipped_domain_gate > before.keys_skipped_domain_gate ? "domain_policy"
      : after.accumulations_skipped_inventory > before.accumulations_skipped_inventory
        ? "inventory_refused"
      : after.accumulations_skipped_admission > before.accumulations_skipped_admission
        ? "admission_refused"
        : "no_accumulated_publication";
    sample.set("publication_skip_reason", reason);
  }
}

}  // namespace

void run_sql(options const& r, std::ostream& output)
{
  if (r.query_file.empty()) { throw std::invalid_argument("sql requires --query-file"); }
  if (r.database != ":memory:" && !std::filesystem::is_regular_file(r.database)) {
    for (std::uint64_t i = 0; i < r.warmups + r.repetitions; ++i) {
      auto sample = sample_record(r, i);
      sample.set("status", "unavailable");
      sample.set("skip_reason", "database_not_found");
      sample.set("dataset", r.dataset);
      sample.set("query_id", r.query_id);
      sample.write(output);
    }
    return;
  }
  auto const sql         = read_text(r.query_file);
  auto const preparation = r.prepare_file.empty() ? std::string{} : read_text(r.prepare_file);
  std::optional<util::env_guard> config;
  if (!r.config.empty()) { config.emplace("SIRIUS_CONFIG_FILE", r.config); }
  util::env_guard const enabled{"SIRIUS_DISABLE", std::nullopt};
  util::env_guard const internal_options{"SIRIUS_ENABLE_TEST_OPTIONS", "1"};
  duckdb::DBConfig db_config;
  db_config.SetOptionByName("allow_unsigned_extensions", duckdb::Value::BOOLEAN(true));
  duckdb::DuckDB database(r.database == ":memory:" ? nullptr : r.database.c_str(), &db_config);
  duckdb::Connection connection(database);
  auto context = connection.context->registered_state->Get<duckdb::SiriusContext>("sirius_state");
  if (!context) { throw std::runtime_error("native loader did not register SiriusContext"); }
  std::vector<int> configured;
  for (auto const* owner :
       context->get_memory_manager().get_memory_spaces_for_tier(cucascade::memory::Tier::GPU)) {
    configured.push_back(owner->get_device_id());
  }
  auto requested = r.devices;
  std::ranges::sort(requested);
  std::ranges::sort(configured);
  if (configured != requested) {
    throw std::runtime_error("SQL config GPU IDs do not match --devices");
  }
  auto statements = connection.ExtractStatements(sql);
  if (statements.size() != 1 ||
      statements.front()->type != duckdb::StatementType::SELECT_STATEMENT) {
    throw std::invalid_argument("--query-file must contain one SELECT statement");
  }
  query(connection, "SET gpu_execution = false;");
  if (!preparation.empty()) {
    for (auto& statement : connection.ExtractStatements(preparation)) {
      auto result = connection.Query(std::move(statement));
      if (!result || result->HasError()) {
        throw std::runtime_error(result ? result->GetError() : "preparation failed");
      }
    }
  }
  query(connection, "SET max_dynamic_filter_bloom_bytes_per_gpu = " + std::to_string(r.cap_bytes));
  query(connection,
        "SET dynamic_filter_domain_coverage_threshold = " +
          duckdb::Value::DOUBLE(r.coverage).ToSQLString());
  query(connection, "SET enable_dynamic_filter = true;");
  query(connection, "SET enable_dynamic_filter_multi_partition = true;");
  auto const& profile = duckdb::session_operator_params(*connection.context);
  if (profile.max_dynamic_filter_bloom_bytes_per_gpu != r.cap_bytes ||
      profile.dynamic_filter_domain_coverage_threshold != r.coverage ||
      !profile.enable_dynamic_filter || !profile.enable_dynamic_filter_multi_partition) {
    throw std::runtime_error("effective SQL operator profile does not match requested profile");
  }
  query(connection, "SET gpu_execution = false;");
  auto cpu                   = query(connection, sql);
  auto const expected        = canonical_rows(*cpu);
  auto const expected_digest = digest(expected);
  auto const expected_types  = cpu->GetTypes();
  cpu.reset();
  query(connection, "SET gpu_execution = true;");
  for (std::uint64_t i = 0; i < r.warmups + r.repetitions; ++i) {
    auto sample = sample_record(r, i);
    sample.set("query_id", r.query_id);
    sample.null("root");
    sample.set("dataset", r.dataset);
    sample.set("database", r.database);
    sample.set("internal_test_options_enabled", true);
    sample.set("profile_verified", true);
    sample.set("enable_dynamic_filter", profile.enable_dynamic_filter);
    sample.set("enable_dynamic_filter_multi_partition",
               profile.enable_dynamic_filter_multi_partition);
    sample.set("oracle_digest", expected_digest);
    sample.set("digest_algorithm", "fnv1a64_length_delimited_null_tagged_sorted_rows");
    auto const before           = context->get_dynamic_filter_stats_snapshot();
    auto const execution_before = context->get_transparent_execution_stats();
    auto const start            = clock::now();
    auto actual                 = query(connection, sql);
    auto const query_ns         = elapsed_ns(start);
    auto const after            = context->get_dynamic_filter_stats_snapshot();
    auto const execution_after  = context->get_transparent_execution_stats();
    auto const values           = canonical_rows(*actual);
    bool const equal            = actual->GetTypes() == expected_types && values == expected;
    auto const rebinds =
      delta(execution_before.successful_rebinds, execution_after.successful_rebinds);
    auto const executions = delta(execution_before.executions, execution_after.executions);
    auto const fallbacks  = delta(execution_before.fallbacks, execution_after.fallbacks);
    auto const runtime_fallbacks =
      delta(execution_before.runtime_fallbacks, execution_after.runtime_fallbacks);
    auto const read_mismatches =
      delta(execution_before.read_view_mismatches, execution_after.read_view_mismatches);
    bool const intercepted =
      rebinds == 1 && executions == 1 && fallbacks == 0 && runtime_fallbacks == 0 &&
      read_mismatches == 0 &&
      execution_after.certificate_mismatches == execution_before.certificate_mismatches &&
      execution_after.certificate_incompletes == execution_before.certificate_incompletes &&
      execution_after.checkpoint_revalidation_failures ==
        execution_before.checkpoint_revalidation_failures &&
      execution_after.late_failure_no_replay == execution_before.late_failure_no_replay &&
      execution_after.lease_held_at_replay == execution_before.lease_held_at_replay;
    sample.set("query_elapsed_ns", query_ns);
    sample.set("timing_scope", "normal_sql_query");
    sample.set("publication_timing_scope", "sum_all_finish_attempts");
    sample.set("result_rows", values.size());
    sample.set("result_digest", digest(values));
    sample.set("gpu_intercepted", intercepted);
    sample.set("successful_rebinds", rebinds);
    sample.set("executions", executions);
    sample.set("fallbacks", fallbacks);
    sample.set("runtime_fallbacks", runtime_fallbacks);
    sample.set("read_view_mismatches", read_mismatches);
    filter_counters(sample, before, after);
    passed(sample);
    if (!intercepted) { skipped(sample, "query_did_not_complete_on_gpu"); }
    if (!equal) {
      sample.set("status", "error");
      sample.set("skip_reason", "cpu_gpu_result_mismatch");
      sample.set("oracle_outcome", "failed");
      sample.set("oracle_passed", false);
    }
    sample.write(output);
    if (!equal) { throw std::runtime_error("CPU/GPU result mismatch"); }
  }
}

}  // namespace sirius::test::s4
