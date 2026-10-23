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

#include "utils/gpu_execution_fixture.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace sirius::test::s4 {

inline constexpr std::string_view sql_oracle_policy = "exact_except_q1_avg_disc_v1";
inline constexpr double sql_oracle_tolerance        = 1e-12;

using sql_rows = std::vector<std::vector<std::string>>;

struct sql_result {
  duckdb::vector<duckdb::LogicalType> types;
  std::vector<std::string> names;
  sql_rows rows;
};

/**
 * @brief Compares null-tagged SQL rows with exact schema and row multiplicity
 *
 * Cells use "N" for NULL and "V" followed by Value::ToString() otherwise. Q1's recognized schema
 * requires unique exact (l_returnflag, l_linestatus) keys; other results compare as multisets.
 * Only Q1's avg_disc DOUBLE column permits finite differences within the relative or absolute 1e-12
 * tolerance; every other cell remains exact.
 */
inline bool sql_results_equal(std::string_view query_id,
                              sql_result const& expected,
                              sql_result const& actual)
{
  if (expected.types != actual.types || expected.names != actual.names ||
      expected.names.size() != expected.types.size() ||
      expected.rows.size() != actual.rows.size()) {
    return false;
  }
  auto const width = expected.types.size();
  for (auto const* rows : {&expected.rows, &actual.rows}) {
    if (std::ranges::any_of(*rows, [width](auto const& row) { return row.size() != width; })) {
      return false;
    }
  }
  std::vector<std::string> const q1_names{"l_returnflag",
                                          "l_linestatus",
                                          "sum_qty",
                                          "sum_base_price",
                                          "sum_disc_price",
                                          "sum_charge",
                                          "avg_qty",
                                          "avg_price",
                                          "avg_disc",
                                          "count_order"};
  bool const q1 = query_id == "q1";
  if (q1 && (expected.names != q1_names || expected.types[0] != duckdb::LogicalType::VARCHAR ||
             expected.types[1] != duckdb::LogicalType::VARCHAR ||
             expected.types[8] != duckdb::LogicalType::DOUBLE)) {
    return false;
  }
  auto left  = expected.rows;
  auto right = actual.rows;
  if (q1) {
    auto key    = [](auto const& row) { return std::tie(row[0], row[1]); };
    auto by_key = [&](auto const& a, auto const& b) { return key(a) < key(b); };
    std::ranges::sort(left, by_key);
    std::ranges::sort(right, by_key);
    for (std::size_t row = 0; row < left.size(); ++row) {
      if (key(left[row]) != key(right[row]) ||
          (row != 0 &&
           (key(left[row - 1]) == key(left[row]) || key(right[row - 1]) == key(right[row])))) {
        return false;
      }
    }
  } else {
    std::ranges::sort(left);
    std::ranges::sort(right);
  }
  for (std::size_t row = 0; row < left.size(); ++row) {
    for (std::size_t column = 0; column < width; ++column) {
      auto const& a = left[row][column];
      auto const& b = right[row][column];
      if (a == b) { continue; }
      if (!q1 || column != 8 || !a.starts_with("V") || !b.starts_with("V") ||
          !GpuExecutionFixture::cells_equal(a.substr(1), b.substr(1), sql_oracle_tolerance)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace sirius::test::s4
