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

#include "s4_experiment_sql_oracle.hpp"

#include <catch.hpp>

#include <algorithm>
#include <string>

namespace {

using sirius::test::s4::sql_result;
using sirius::test::s4::sql_results_equal;

sql_result q1_result()
{
  using type = duckdb::LogicalType;
  return {{type::VARCHAR,
           type::VARCHAR,
           type::DECIMAL(38, 2),
           type::DECIMAL(38, 2),
           type::DECIMAL(38, 4),
           type::DECIMAL(38, 6),
           type::DOUBLE,
           type::DOUBLE,
           type::DOUBLE,
           type::BIGINT},
          {"l_returnflag",
           "l_linestatus",
           "sum_qty",
           "sum_base_price",
           "sum_disc_price",
           "sum_charge",
           "avg_qty",
           "avg_price",
           "avg_disc",
           "count_order"},
          {{"VN",
            "VO",
            "V400806339.00",
            "V600956992831.14",
            "V570912959258.9055",
            "V593749593950.876604",
            "V25.502955700004975",
            "V38238.36619954449",
            "V0.04999417093607475",
            "V15716074"},
           {"VR",
            "VF",
            "V3775724970.00",
            "V5661603032745.34",
            "V5378513563915.4097",
            "V5593662252666.916161",
            "V25.50006628406532",
            "V38236.697258452965",
            "V0.05000130433965413",
            "V148067261"}}};
}

}  // namespace

TEST_CASE("S4 SQL oracle accepts the measured Q1 avg_disc roundoff", "[s4][sql_oracle]")
{
  auto expected     = q1_result();
  auto actual       = expected;
  actual.rows[0][8] = "V0.04999417093607474";
  actual.rows[1][8] = "V0.05000130433965412";
  std::ranges::reverse(actual.rows);
  REQUIRE(sql_results_equal("q1", expected, actual));
}

TEST_CASE("S4 SQL oracle restricts tolerance to the recognized Q1 column", "[s4][sql_oracle]")
{
  auto expected     = q1_result();
  auto actual       = expected;
  actual.rows[0][8] = "V0.04999417093607474";
  CHECK_FALSE(sql_results_equal("q4", expected, actual));
  expected.names[8] = actual.names[8] = "float_key";
  CHECK_FALSE(sql_results_equal("q1", expected, actual));
  expected = actual = q1_result();
  expected.types[8] = actual.types[8] = duckdb::LogicalType::FLOAT;
  actual.rows[0][8]                   = "V0.04999417093607474";
  CHECK_FALSE(sql_results_equal("q1", expected, actual));
  for (std::size_t column : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 9U}) {
    expected = actual = q1_result();
    actual.rows[0][column] += "1";
    CAPTURE(column);
    CHECK_FALSE(sql_results_equal("q1", expected, actual));
  }
  expected = actual   = q1_result();
  expected.rows[0][9] = "V1000000000000000";
  actual.rows[0][9]   = "V1000000000000001";
  CHECK_FALSE(sql_results_equal("q1", expected, actual));
}

TEST_CASE("S4 SQL oracle keeps schema shape and group multiplicity exact", "[s4][sql_oracle]")
{
  auto expected = q1_result();
  auto actual   = expected;
  SECTION("types") { actual.types[8] = duckdb::LogicalType::FLOAT; }
  SECTION("decimal scale") { actual.types[2] = duckdb::LogicalType::DECIMAL(38, 3); }
  SECTION("names") { actual.names[0] = "another_key"; }
  SECTION("malformed Q1 schema on both sides")
  {
    expected.names[8] = actual.names[8] = "float_key";
  }
  SECTION("missing row") { actual.rows.pop_back(); }
  SECTION("extra row") { actual.rows.push_back(actual.rows.front()); }
  SECTION("short row") { actual.rows[0].pop_back(); }
  SECTION("wide row") { actual.rows[0].push_back("V0"); }
  SECTION("missing type") { actual.types.pop_back(); }
  SECTION("duplicate key")
  {
    actual.rows[1][0] = actual.rows[0][0];
    actual.rows[1][1] = actual.rows[0][1];
  }
  SECTION("duplicate keys on both sides")
  {
    expected.rows.push_back(expected.rows.front());
    actual = expected;
  }
  CHECK_FALSE(sql_results_equal("q1", expected, actual));
}

TEST_CASE("S4 SQL oracle handles nulls and nonfinite values exactly", "[s4][sql_oracle]")
{
  auto expected = q1_result();
  auto actual   = expected;
  for (auto const* value : {"N", "Vinf", "V-inf", "Vnan"}) {
    expected.rows[0][8] = actual.rows[0][8] = value;
    CHECK(sql_results_equal("q1", expected, actual));
    actual.rows[0][8] = "V0";
    CHECK_FALSE(sql_results_equal("q1", expected, actual));
  }
  expected.rows[0][8] = "Vinf";
  actual.rows[0][8]   = "V-inf";
  CHECK_FALSE(sql_results_equal("q1", expected, actual));
  expected = actual   = q1_result();
  expected.rows[0][0] = "N";
  actual.rows[0][0]   = "VNULL";
  CHECK_FALSE(sql_results_equal("q1", expected, actual));
}

TEST_CASE("S4 SQL oracle bounds both absolute and relative floating error", "[s4][sql_oracle]")
{
  auto expected       = q1_result();
  auto actual         = expected;
  expected.rows[0][8] = "V0";
  actual.rows[0][8]   = "V0.0000000000005";
  CHECK(sql_results_equal("q1", expected, actual));
  actual.rows[0][8] = "V0.000000000002";
  CHECK_FALSE(sql_results_equal("q1", expected, actual));
  expected.rows[0][8] = "V1000000000";
  actual.rows[0][8]   = "V1000000000.0005";
  CHECK(sql_results_equal("q1", expected, actual));
  actual.rows[0][8] = "V1000000000.002";
  CHECK_FALSE(sql_results_equal("q1", expected, actual));
  actual.rows[0][8] = "V1000000000oops";
  CHECK_FALSE(sql_results_equal("q1", expected, actual));
}

TEST_CASE("S4 SQL oracle preserves exact multisets outside Q1", "[s4][sql_oracle]")
{
  auto expected = q1_result();
  expected.rows.push_back(expected.rows.front());
  auto actual = expected;
  std::ranges::reverse(actual.rows);
  CHECK(sql_results_equal("q7", expected, actual));
  actual.rows.back() = actual.rows[1];
  CHECK_FALSE(sql_results_equal("q7", expected, actual));
  expected.rows.clear();
  actual.rows.clear();
  CHECK(sql_results_equal("q7", expected, actual));
}
