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

#pragma once

#include "yyjson.hpp"

#include <chrono>
#include <concepts>
#include <cstdint>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief Native S4 experiment requests and JSONL output shared by the three fixture modes
 *
 * Device IDs are visible CUDA ordinals, not positions in `options::devices`. Python supplies
 * process and pair identities; `sample_record` adds the repetition identity. `run_builder` calls
 * `accumulated_bloom_builder`, and `run_sql` reads the registered `SiriusContext` after normal SQL
 * returns.
 */
namespace sirius::test::s4 {

using clock = std::chrono::steady_clock;

struct options {
  std::string mode;
  std::string output;
  std::string run_id            = "manual";
  std::string sample_id         = "sample";
  std::string build_id          = "unspecified";
  std::uint64_t process_cluster = 0;
  std::uint64_t pair            = 0;
  std::uint64_t order           = 0;
  std::uint64_t seed            = 1823;
  std::uint64_t warmups         = 3;
  std::uint64_t repetitions     = 3;
  std::vector<int> devices{0};
  std::vector<int> contributors;
  int root                = 0;
  int source              = 0;
  int destination         = 1;
  std::uint64_t bytes     = (1ULL << 20) + 256;
  std::uint64_t keys      = 1;
  std::uint64_t rows      = 4096;
  std::uint64_t cap_bytes = 256ULL << 20;
  double coverage         = 0.9;
  std::string profile     = "default256";
  std::string pattern     = "pair";
  std::string issuer      = "destination_read";
  std::string transport   = "automatic";
  std::string database    = ":memory:";
  std::string query_file;
  std::string prepare_file;
  std::string config;
  std::string query_id;
  std::string dataset;
};

/**
 * @brief Owns one JSON object, including copied keys and string values
 */
class record {
 public:
  record();
  void set(std::string_view key, std::string_view value);
  void set(std::string_view key, char const* value) { set(key, std::string_view{value}); }
  void set(std::string_view key, bool value);
  void set(std::string_view key, double value);
  void null(std::string_view key);

  template <std::integral T>
  void set(std::string_view key, T value)
  {
    if constexpr (std::signed_integral<T>) {
      put(key, duckdb_yyjson::yyjson_mut_sint(doc_.get(), value));
    } else {
      put(key, duckdb_yyjson::yyjson_mut_uint(doc_.get(), value));
    }
  }

  template <std::integral T>
  void array(std::string_view key, std::span<T const> values)
  {
    auto* result = duckdb_yyjson::yyjson_mut_arr(doc_.get());
    if (!result) { throw std::bad_alloc{}; }
    for (auto value : values) {
      auto* element = [&] {
        if constexpr (std::signed_integral<T>) {
          return duckdb_yyjson::yyjson_mut_sint(doc_.get(), value);
        } else {
          return duckdb_yyjson::yyjson_mut_uint(doc_.get(), value);
        }
      }();
      if (!element || !duckdb_yyjson::yyjson_mut_arr_append(result, element)) {
        throw std::bad_alloc{};
      }
    }
    put(key, result);
  }

  void write(std::ostream& output) const;

 private:
  void put(std::string_view key, duckdb_yyjson::yyjson_mut_val* value);
  std::unique_ptr<duckdb_yyjson::yyjson_mut_doc, decltype(&duckdb_yyjson::yyjson_mut_doc_free)>
    doc_;
};

[[nodiscard]] record sample_record(options const& request, std::uint64_t index);
[[nodiscard]] std::uint64_t elapsed_ns(clock::time_point start);
[[nodiscard]] std::uint64_t checked_product(std::uint64_t lhs, std::uint64_t rhs);
[[nodiscard]] std::string read_text(std::string const& path);
void passed(record& sample);
void skipped(record& sample, std::string_view reason);
void run_transport(options const& request, std::ostream& output);
void run_builder(options const& request, std::ostream& output);
void run_sql(options const& request, std::ostream& output);

}  // namespace sirius::test::s4
