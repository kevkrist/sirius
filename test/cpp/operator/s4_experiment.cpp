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

#include "op/dynamic_filter/bloom_sizing.hpp"
#include "s4_experiment_common.hpp"

#include <rmm/aligned.hpp>

#include <duckdb.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <system_error>

#ifndef SIRIUS_S4_BASELINE_SQL
#define SIRIUS_S4_BASELINE_SQL 0
#endif
#ifndef SIRIUS_S4_REVISION_ROLE
#define SIRIUS_S4_REVISION_ROLE "final"
#endif

namespace sirius::test::s4 {
namespace {
constexpr std::string_view revision_role{SIRIUS_S4_REVISION_ROLE};
static_assert(revision_role == (SIRIUS_S4_BASELINE_SQL ? "baseline" : "final"));
}  // namespace

record::record()
  : doc_(duckdb_yyjson::yyjson_mut_doc_new(nullptr), duckdb_yyjson::yyjson_mut_doc_free)
{
  if (!doc_) { throw std::bad_alloc{}; }
  auto* object = duckdb_yyjson::yyjson_mut_obj(doc_.get());
  if (!object) { throw std::bad_alloc{}; }
  duckdb_yyjson::yyjson_mut_doc_set_root(doc_.get(), object);
}

void record::put(std::string_view key, duckdb_yyjson::yyjson_mut_val* value)
{
  auto* name = duckdb_yyjson::yyjson_mut_strncpy(doc_.get(), key.data(), key.size());
  if (!name || !value ||
      !duckdb_yyjson::yyjson_mut_obj_put(
        duckdb_yyjson::yyjson_mut_doc_get_root(doc_.get()), name, value)) {
    throw std::bad_alloc{};
  }
}

void record::set(std::string_view key, std::string_view value)
{
  put(key, duckdb_yyjson::yyjson_mut_strncpy(doc_.get(), value.data(), value.size()));
}
void record::set(std::string_view key, bool value)
{
  put(key, duckdb_yyjson::yyjson_mut_bool(doc_.get(), value));
}

void record::array(std::string_view key, std::span<std::string_view const> values)
{
  auto* result = duckdb_yyjson::yyjson_mut_arr(doc_.get());
  if (!result) { throw std::bad_alloc{}; }
  for (auto value : values) {
    auto* element = duckdb_yyjson::yyjson_mut_strncpy(doc_.get(), value.data(), value.size());
    if (!element || !duckdb_yyjson::yyjson_mut_arr_append(result, element)) {
      throw std::bad_alloc{};
    }
  }
  put(key, result);
}
void record::set(std::string_view key, double value)
{
  put(key, duckdb_yyjson::yyjson_mut_real(doc_.get(), value));
}
void record::null(std::string_view key) { put(key, duckdb_yyjson::yyjson_mut_null(doc_.get())); }

void record::write(std::ostream& output) const
{
  std::size_t length{};
  std::unique_ptr<char, decltype(&std::free)> text{
    duckdb_yyjson::yyjson_mut_write(doc_.get(), 0, &length), std::free};
  if (!text) { throw std::runtime_error("JSON encoding failed"); }
  output.write(text.get(), static_cast<std::streamsize>(length));
  output << '\n';
  output.flush();
  if (!output) { throw std::runtime_error("JSONL output failed"); }
}

std::uint64_t elapsed_ns(clock::time_point start)
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
}

std::uint64_t checked_product(std::uint64_t lhs, std::uint64_t rhs)
{
  if (rhs && lhs > std::numeric_limits<std::uint64_t>::max() / rhs) {
    throw std::invalid_argument("byte arithmetic overflow");
  }
  return lhs * rhs;
}

std::string read_text(std::string const& path)
{
  std::ifstream input(path);
  if (!input) { throw std::runtime_error("cannot read " + path); }
  std::string text{std::istreambuf_iterator<char>{input}, {}};
  if (input.bad()) { throw std::runtime_error("read failed: " + path); }
  return text;
}

record sample_record(options const& r, std::uint64_t index)
{
  record result;
  if (r.mode == "sql") { record_sql_oracle_policy(result); }
  auto const warmup     = index < r.warmups;
  auto const repetition = warmup ? index : index - r.warmups;
  result.set("schema_version", 2);
  result.set("run_id", r.run_id);
  result.set("sample_id",
             r.sample_id + (warmup ? ":warmup:" : ":sample:") + std::to_string(repetition));
  result.set("build_id", r.build_id);
  result.set("mode", r.mode);
  result.set("process_cluster", r.process_cluster);
  result.set("pair", r.pair);
  result.set("order", r.order);
  result.set("seed", r.seed);
  result.set("warmup", warmup);
  result.set("repetition", repetition);
  result.array("devices", std::span<int const>{r.devices});
  result.set("root", r.root);
  result.set("revision_role", revision_role);
  result.set("identity_source", "fixture_build_metadata");
  result.set("linkage", "static_sirius_core");
  result.set("profile", r.profile);
  result.set("cap_bytes", r.cap_bytes);
  result.set("coverage", r.coverage);
  result.set("N", r.devices.size());
  result.set("status", "error");
  result.set("oracle_outcome", "not_measured");
  for (auto const* field : {"skip_reason",
                            "oracle_passed",
                            "gpu_intercepted",
                            "actual_transport",
                            "transport_api",
                            "stream_owner",
                            "K",
                            "B",
                            "C",
                            "input_bitmap_bytes",
                            "ready_replica_bytes",
                            "published_bitmap_bytes",
                            "logical_edge_bytes",
                            "d2h_bytes",
                            "h2d_bytes",
                            "planned_logical_edge_bytes",
                            "planned_d2h_bytes",
                            "planned_h2d_bytes",
                            "scratch_requested_bytes",
                            "scratch_aligned_bytes",
                            "host_staging_physical_bytes",
                            "host_elapsed_ns",
                            "query_elapsed_ns",
                            "local_event_durations",
                            "allocator_committed_peak_bytes"}) {
    result.null(field);
  }
  result.set("physical_route", "not_measured");
  return result;
}

void passed(record& sample)
{
  sample.set("status", "ok");
  sample.null("skip_reason");
  sample.set("oracle_outcome", "passed");
  sample.set("oracle_passed", true);
}

void skipped(record& sample, std::string_view reason)
{
  sample.set("status", "skipped");
  sample.set("skip_reason", reason);
}

namespace {

template <typename T>
T number(std::string_view value)
{
  T result{};
  auto const [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size()) {
    throw std::invalid_argument("invalid numeric argument: " + std::string{value});
  }
  return result;
}

std::vector<int> device_list(std::string_view value)
{
  std::vector<int> result;
  do {
    auto const comma  = value.find(',');
    auto const device = number<int>(value.substr(0, comma));
    if (device < 0 || std::ranges::find(result, device) != result.end()) {
      throw std::invalid_argument("devices must be distinct nonnegative CUDA ordinals");
    }
    result.push_back(device);
    if (comma == std::string_view::npos) { break; }
    value.remove_prefix(comma + 1);
  } while (true);
  return result;
}

options parse(int argc, char** argv)
{
  options result;
  result.mode = argv[1];
  if (result.mode != "identify" && result.mode != "transport" && result.mode != "builder" &&
      result.mode != "sql") {
    throw std::invalid_argument("unknown mode: " + result.mode);
  }
  std::map<std::string, std::string> arguments;
  for (int i = 2; i < argc; i += 2) {
    std::string key{argv[i]};
    if (!key.starts_with("--") || i + 1 >= argc || !arguments.emplace(key, argv[i + 1]).second) {
      throw std::invalid_argument("options require unique --name value pairs");
    }
  }
  auto take = [&](char const* key, auto& target) {
    auto node = arguments.extract(key);
    if (node.empty()) { return; }
    using type = std::remove_cvref_t<decltype(target)>;
    if constexpr (std::same_as<type, std::string>) {
      target = std::move(node.mapped());
    } else if constexpr (std::same_as<type, std::vector<int>>) {
      target = device_list(node.mapped());
    } else {
      target = number<type>(node.mapped());
    }
  };
  take("--output", result.output);
  take("--run-id", result.run_id);
  take("--sample-id", result.sample_id);
  take("--build-id", result.build_id);
  take("--process-cluster", result.process_cluster);
  take("--pair", result.pair);
  take("--order", result.order);
  take("--seed", result.seed);
  take("--warmups", result.warmups);
  take("--repetitions", result.repetitions);
  take("--devices", result.devices);
  take("--contributors", result.contributors);
  result.root = result.devices.front();
  take("--root", result.root);
  take("--source", result.source);
  take("--destination", result.destination);
  if (arguments.contains("--bytes") && arguments.contains("--bitmap-bytes")) {
    throw std::invalid_argument("use only one of --bytes and --bitmap-bytes");
  }
  take("--bytes", result.bytes);
  take("--bitmap-bytes", result.bytes);
  take("--keys", result.keys);
  take("--rows", result.rows);
  take("--cap-bytes", result.cap_bytes);
  take("--coverage", result.coverage);
  take("--profile", result.profile);
  take("--pattern", result.pattern);
  take("--issuer", result.issuer);
  take("--transport", result.transport);
  take("--database", result.database);
  take("--query-file", result.query_file);
  take("--prepare-file", result.prepare_file);
  take("--config", result.config);
  take("--query-id", result.query_id);
  take("--dataset", result.dataset);
  if (!arguments.empty()) {
    throw std::invalid_argument("unknown option: " + arguments.begin()->first);
  }
  if (result.output.empty()) { throw std::invalid_argument("--output PATH is required"); }
  if (result.repetitions == 0 || result.repetitions > 1000 || result.warmups > 1000) {
    throw std::invalid_argument("require 1..1000 repetitions and 0..1000 warmups");
  }
  if (!result.bytes || result.bytes > (2ULL << 30) || !result.cap_bytes ||
      result.cap_bytes > (2ULL << 30) || result.keys < 1 || result.keys > 2 ||
      result.rows > (1ULL << 24) || !std::isfinite(result.coverage) || result.coverage < 0) {
    throw std::invalid_argument("invalid bounded byte/key/row/coverage request");
  }
  if (std::ranges::find(result.devices, result.root) == result.devices.end()) {
    throw std::invalid_argument("root must be in --devices");
  }
  if (result.contributors.empty()) { result.contributors = result.devices; }
  for (auto device : result.contributors) {
    if (std::ranges::find(result.devices, device) == result.devices.end()) {
      throw std::invalid_argument("contributors must be in --devices");
    }
  }
  if (std::ranges::find(result.contributors, result.root) == result.contributors.end()) {
    throw std::invalid_argument("root must be a contributor");
  }
  return result;
}

void identify(options const& request, std::ostream& output)
{
  record result;
  result.set("schema_version", 2);
  result.set("mode", "identify");
  result.set("status", "ok");
  result.set("revision_role", revision_role);
  result.set("identity_source", "fixture_build_metadata");
#if SIRIUS_S4_BASELINE_SQL
  constexpr std::string_view modes[]{"identify", "sql"};
#else
  constexpr std::string_view modes[]{"identify", "transport", "builder", "sql"};
#endif
  result.array("supported_modes", std::span<std::string_view const>{modes});
  result.set("publication_metrics_supported", sql_publication_metrics_supported());
  record_sql_oracle_policy(result);
  result.set("linkage", "static_sirius_core");
  result.set("executable_path", std::filesystem::read_symlink("/proc/self/exe").string());
  result.set("bloom_block_bytes", op::bloom_bytes_per_block);
  result.set("cuda_allocation_alignment", rmm::CUDA_ALLOCATION_ALIGNMENT);
  result.set("duckdb_version", duckdb::DuckDB::LibraryVersion());
  result.set("build_id", request.build_id);
  result.write(output);
}

}  // namespace
}  // namespace sirius::test::s4

int main(int argc, char** argv)
{
  using namespace sirius::test::s4;
  if (argc < 2 || std::string_view{argv[1]} == "--help") {
    std::cout
      << "sirius_s4_experiment identify|transport|builder|sql --output PATH [--option value]\n"
         "Common: --devices CSV --root ID --warmups 3 --repetitions 3 --seed 1823\n"
         "        --run-id ID --sample-id PREFIX --build-id ID --process-cluster N --pair N "
         "--order N\n"
         "        --profile default256 --cap-bytes 268435456 --coverage 0.9\n"
         "Transport: --pattern pair|all_pairs|root_inbound|root_outbound\n"
         "           --issuer destination_read|source_write --source ID --destination ID --bytes "
         "N\n"
         "Builder: --bitmap-bytes N (--bytes alias) --keys 1|2 --rows 4096 --contributors CSV\n"
         "         --transport automatic|force_host|require_direct\n"
         "SQL: --database PATH --prepare-file PATH --query-file PATH --config PATH\n"
         "     --query-id ID --dataset NAME\n"
         "Baseline overlays support identify/sql only.\n"
         "Output is schema v2 JSONL. identify/--help make no CUDA calls. See "
         "S4_FIXTURE_NOTES.md.\n";
    return argc < 2 ? 2 : 0;
  }
  try {
    auto const request = parse(argc, argv);
    std::ofstream output(request.output);
    if (!output) { throw std::runtime_error("cannot open output " + request.output); }
    try {
      if (request.mode == "identify") {
        identify(request, output);
#if SIRIUS_S4_BASELINE_SQL
      } else if (request.mode != "sql") {
        for (std::uint64_t i = 0; i < request.warmups + request.repetitions; ++i) {
          auto sample = sample_record(request, i);
          sample.set("status", "unavailable");
          sample.set("skip_reason", "mode_not_supported_by_baseline_overlay");
          sample.write(output);
        }
#else
      } else if (request.mode == "transport") {
        run_transport(request, output);
      } else if (request.mode == "builder") {
        run_builder(request, output);
#endif
      } else {
        run_sql(request, output);
      }
    } catch (std::exception const& error) {
      auto sample = sample_record(request, request.warmups);
      sample.set("sample_id", request.sample_id + ":process_error");
      sample.null("repetition");
      sample.set("skip_reason", error.what());
      sample.write(output);
      throw;
    }
    return 0;
  } catch (std::exception const& error) {
    std::cerr << "sirius_s4_experiment: " << error.what() << '\n';
    return 1;
  }
}
