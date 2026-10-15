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

#include "duckdb.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" void sirius_order_diag_reset() __attribute__((weak));
extern "C" void sirius_order_diag_dump(char const*) __attribute__((weak));

namespace {
std::string read_file(std::filesystem::path const& path)
{
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot read " + path.string());
  return {std::istreambuf_iterator<char>{input}, {}};
}

std::string escaped(std::string const& value)
{
  std::string result;
  for (char c : value) {
    if (c == '\\')
      result += "\\\\";
    else if (c == '\t')
      result += "\\t";
    else if (c == '\n')
      result += "\\n";
    else if (c == '\r')
      result += "\\r";
    else
      result += c;
  }
  return result;
}

std::unique_ptr<duckdb::MaterializedQueryResult> execute(duckdb::Connection& connection,
                                                         std::string const& sql)
{
  auto result = connection.Query(sql);
  if (!result || result->HasError()) {
    throw std::runtime_error(result ? result->GetError() : "query produced no result");
  }
  return result;
}

void save_result(duckdb::MaterializedQueryResult& result, std::filesystem::path const& path)
{
  std::ofstream output(path);
  for (std::size_t column = 0; column < result.ColumnCount(); ++column) {
    if (column) output << '\t';
    output << escaped(result.names[column]);
  }
  output << '\n';
  for (std::size_t column = 0; column < result.ColumnCount(); ++column) {
    if (column) output << '\t';
    output << escaped(result.types[column].ToString());
  }
  output << '\n';
  for (std::size_t row = 0; row < result.RowCount(); ++row) {
    for (std::size_t column = 0; column < result.ColumnCount(); ++column) {
      if (column) output << '\t';
      auto value = result.GetValue(column, row);
      output << (value.IsNull() ? "\\N" : escaped(value.ToString()));
    }
    output << '\n';
  }
  if (!output) throw std::runtime_error("failed writing " + path.string());
}
}  // namespace

int main(int argc, char** argv)
{
  try {
    if (argc < 5)
      throw std::invalid_argument(
        "usage: runner WORKLOAD OUTPUT FRESH_COUNT RETAINED_COUNT [--cpu] [--profile]");
    std::filesystem::path workload = argv[1];
    std::filesystem::path output   = argv[2];
    auto fresh                     = std::stoi(argv[3]);
    auto retained                  = std::stoi(argv[4]);
    if (fresh < 0 || retained < 0) throw std::invalid_argument("sample counts must be nonnegative");
    bool cpu = false, profile = false;
    for (int i = 5; i < argc; ++i) {
      std::string argument = argv[i];
      if (argument == "--cpu")
        cpu = true;
      else if (argument == "--profile")
        profile = true;
      else
        throw std::invalid_argument("unknown argument " + argument);
    }
    if (!std::filesystem::create_directories(output))
      throw std::runtime_error("output directory must be new");
    std::ofstream timings(output / "timings.csv");
    timings << "query,regime,iteration,elapsed_ms,rows,columns,capture_ordinal\n";
    std::istringstream manifest(read_file(workload / "queries.tsv"));
    std::string line;
    int ordinal = 0;
    while (std::getline(manifest, line)) {
      if (line.empty()) continue;
      auto split = line.find('\t');
      if (split == std::string::npos) throw std::runtime_error("queries.tsv needs label<TAB>file");
      auto label = line.substr(0, split);
      if (label.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
          std::string::npos)
        throw std::runtime_error("unsafe query label");
      std::filesystem::path query_path = line.substr(split + 1);
      if (!query_path.is_absolute()) query_path = workload / query_path;
      auto sql = read_file(query_path);
      duckdb::DuckDB database(nullptr);
      duckdb::Connection connection(database);
      execute(connection, "SET gpu_execution = false");
      execute(connection, read_file(workload / "setup.sql"));
      execute(connection, "SET enable_duckdb_fallback = false");
      execute(connection, cpu ? "SET gpu_execution = false" : "SET gpu_execution = true");
      for (int index = 0; index < 1 + fresh + retained; ++index) {
        std::string regime = index == 0 ? "context_cold" : index <= fresh ? "fresh" : "retained";
        int iteration      = index == 0 ? 0 : index <= fresh ? index - 1 : index - fresh - 1;
        auto identity      = label + "-" + regime + "-" + std::to_string(iteration);
        if (!cpu && regime == "fresh") execute(connection, "CALL reset_sirius_cache()");
        if (sirius_order_diag_reset) sirius_order_diag_reset();
        if (profile) execute(connection, "CALL profiler_start()");
        if (!cpu) execute(connection, "CALL sirius_set_query_label('" + identity + "')");
        auto begin  = std::chrono::steady_clock::now();
        auto result = execute(connection, sql);
        auto end    = std::chrono::steady_clock::now();
        if (profile) execute(connection, "CALL profiler_stop()");
        auto elapsed = std::chrono::duration<double, std::milli>(end - begin).count();
        timings << label << ',' << regime << ',' << iteration << ',' << elapsed << ','
                << result->RowCount() << ',' << result->ColumnCount() << ',' << ordinal++ << '\n';
        timings.flush();
        save_result(*result, output / (identity + ".tsv"));
        if (sirius_order_diag_dump) sirius_order_diag_dump((output / (identity + ".json")).c_str());
        std::cout << identity << ' ' << elapsed << " ms, " << result->RowCount() << " rows\n"
                  << std::flush;
      }
    }
    if (!timings) throw std::runtime_error("timing output failed");
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
