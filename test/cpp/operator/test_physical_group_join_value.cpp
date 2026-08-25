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

#include "helper/type_conversions.hpp"
#include "operator_test_utils.hpp"

#include <cudf/unary.hpp>

#include <rmm/mr/per_device_resource.hpp>
#include <rmm/mr/statistics_resource_adaptor.hpp>

#include <catch.hpp>
#include <duckdb.hpp>
#include <op/aggregate/group_join_impl.hpp>
#include <op/sirius_physical_group_join.hpp>
#include <utils/group_join_test_builder.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace duckdb;
using namespace sirius::op;
using namespace sirius::test::operator_utils;

namespace {

constexpr uint64_t k_default_max_bytes = 2ULL * 1024 * 1024 * 1024;
// Eight bytes admit no full slot set and force these tests through the sparse path.
constexpr uint64_t k_tiny_max_bytes = 8;

// nullopt denotes SQL NULL in the key or the aggregate output.
template <typename ValueT>
using value_row = std::pair<std::optional<int64_t>, std::optional<ValueT>>;

struct value_case {
  groupjoin::join_form form;
  groupjoin::agg_op op;
  sirius::logical_type output_type;
  std::optional<std::size_t> arg_idx;
  uint64_t max_state_bytes;
};

template <typename T>
consteval cudf::type_id numeric_type_id()
{
  static_assert(std::is_same_v<T, int32_t> || std::is_same_v<T, int64_t>);
  return std::is_same_v<T, int32_t> ? cudf::type_id::INT32 : cudf::type_id::INT64;
}

// KeyT/ValueT select the INT32 or INT64 carrier; braced-init call sites keep the int32-key,
// int64-value defaults.
template <typename KeyT = int32_t, typename ValueT = int64_t>
std::shared_ptr<cucascade::data_batch> make_kv_batch(cucascade::memory::memory_space& space,
                                                     const std::vector<KeyT>& keys,
                                                     const std::vector<ValueT>& values)
{
  auto key_batch   = make_numeric_batch<KeyT>(space, keys, numeric_type_id<KeyT>());
  auto value_batch = make_numeric_batch<ValueT>(space, values, numeric_type_id<ValueT>());
  return concatenate_batches_horizontal({key_batch, value_batch}, space);
}

std::shared_ptr<cucascade::data_batch> make_kv_batch_with_nulls(
  cucascade::memory::memory_space& space,
  const std::vector<int32_t>& keys,
  const std::vector<bool>& key_valids,
  const std::vector<int64_t>& values,
  const std::vector<bool>& value_valids)
{
  auto key_batch =
    make_numeric_batch_with_nulls<int32_t>(space, keys, key_valids, cudf::type_id::INT32);
  auto value_batch =
    make_numeric_batch_with_nulls<int64_t>(space, values, value_valids, cudf::type_id::INT64);
  return concatenate_batches_horizontal({key_batch, value_batch}, space);
}

template <typename KeyT = int32_t>
std::shared_ptr<cucascade::data_batch> make_decimal_kv_batch(cucascade::memory::memory_space& space,
                                                             const std::vector<KeyT>& keys,
                                                             const std::vector<int64_t>& value_reps,
                                                             int32_t scale)
{
  auto key_batch   = make_numeric_batch<KeyT>(space, keys, numeric_type_id<KeyT>());
  auto value_batch = make_decimal64_batch(space, value_reps, scale);
  return concatenate_batches_horizontal({key_batch, value_batch}, space);
}

std::shared_ptr<cucascade::data_batch> make_decimal32_kv_batch(
  cucascade::memory::memory_space& space,
  const std::vector<int32_t>& keys,
  const std::vector<int32_t>& value_reps,
  int32_t scale)
{
  auto key_batch   = make_numeric_batch<int32_t>(space, keys, cudf::type_id::INT32);
  auto value_batch = make_decimal32_batch(space, value_reps, scale);
  return concatenate_batches_horizontal({key_batch, value_batch}, space);
}

template <typename KeyT, typename ValueT>
std::vector<value_row<ValueT>> run_value_group_join(
  const std::vector<std::shared_ptr<cucascade::data_batch>>& preserved_batches,
  const std::vector<std::shared_ptr<cucascade::data_batch>>& counted_batches,
  value_case config,
  cudf::type_id expected_value_type,
  sirius_physical_group_join::strategy expected_strategy)
{
  duckdb::vector<sirius::logical_type> types;
  types.push_back(sirius::logical_type::make(
    std::is_same_v<KeyT, int32_t> ? sirius::type_id::INTEGER : sirius::type_id::BIGINT));
  types.push_back(config.output_type);
  sirius_physical_group_join op(std::move(types),
                                /*estimated_cardinality=*/16,
                                sirius::test::make_group_join_spec(config.form,
                                                                   config.op,
                                                                   /*preserved_key_idx=*/0,
                                                                   /*counted_key_idx=*/0,
                                                                   config.arg_idx,
                                                                   config.output_type,
                                                                   config.max_state_bytes));

  group_join_input input(preserved_batches, counted_batches);
  auto stream = default_stream();
  auto output = op.execute(input, stream);
  stream.synchronize();
  REQUIRE(op.last_strategy() == expected_strategy);

  auto const& out_batches =
    dynamic_cast<const pipelineable_operator_data&>(*output).get_data_batches();
  REQUIRE(out_batches.size() == 1);
  auto const view = sirius::get_cudf_table_view(*out_batches[0]);
  REQUIRE(view.num_columns() == 2);
  REQUIRE(view.column(1).type().id() == expected_value_type);

  auto const keys           = copy_column_to_host<KeyT>(view.column(0));
  auto const key_validity   = copy_validity_to_host(view.column(0));
  auto const values         = copy_column_to_host<ValueT>(view.column(1));
  auto const value_validity = copy_validity_to_host(view.column(1));

  std::vector<value_row<ValueT>> rows;
  rows.reserve(keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i) {
    rows.emplace_back(
      key_validity[i] ? std::optional<int64_t>(static_cast<int64_t>(keys[i])) : std::nullopt,
      value_validity[i] ? std::optional<ValueT>(values[i]) : std::nullopt);
  }
  // Sparse output order is unspecified; group keys are unique so ordering by key is total.
  std::sort(
    rows.begin(), rows.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
  return rows;
}

/// CPU oracle: run @p query on a plain DuckDB instance (no Sirius) after executing @p setup
/// statements, returning `[key, value]` rows ordered by key with NULLs first.
template <typename ValueT>
std::vector<value_row<ValueT>> duckdb_cpu_oracle(const std::vector<std::string>& setup,
                                                 const std::string& query)
{
  duckdb::DuckDB db(nullptr);
  duckdb::Connection con(db);
  for (auto const& statement : setup) {
    auto result = con.Query(statement);
    REQUIRE_FALSE(result->HasError());
  }
  auto result = con.Query(query);
  REQUIRE_FALSE(result->HasError());
  std::vector<value_row<ValueT>> rows;
  for (idx_t i = 0; i < result->RowCount(); ++i) {
    auto const key_value = result->GetValue(0, i);
    auto const agg_value = result->GetValue(1, i);
    rows.emplace_back(
      key_value.IsNull() ? std::nullopt : std::optional<int64_t>(key_value.GetValue<int64_t>()),
      agg_value.IsNull() ? std::nullopt : std::optional<ValueT>(agg_value.GetValue<ValueT>()));
  }
  return rows;
}

std::string insert_rows_sql(std::string const& table,
                            const std::vector<std::optional<int64_t>>& keys,
                            const std::vector<std::optional<int64_t>>& values)
{
  std::ostringstream sql;
  sql << "INSERT INTO " << table << " VALUES ";
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (i != 0) { sql << ", "; }
    sql << "(" << (keys[i] ? std::to_string(*keys[i]) : "NULL") << ", "
        << (values[i] ? std::to_string(*values[i]) : "NULL") << ")";
  }
  sql << ";";
  return sql.str();
}

template <typename ValueT>
void require_rows_match(const std::vector<value_row<ValueT>>& actual,
                        const std::vector<value_row<ValueT>>& expected)
{
  REQUIRE(actual.size() == expected.size());
  for (std::size_t i = 0; i < actual.size(); ++i) {
    REQUIRE(actual[i].first == expected[i].first);
    REQUIRE(actual[i].second.has_value() == expected[i].second.has_value());
    if (actual[i].second.has_value()) {
      if constexpr (std::is_floating_point_v<ValueT>) {
        REQUIRE(*actual[i].second == Approx(*expected[i].second));
      } else {
        REQUIRE(*actual[i].second == *expected[i].second);
      }
    }
  }
}

sirius::logical_type bigint_type() { return sirius::logical_type::make(sirius::type_id::BIGINT); }
sirius::logical_type integer_type() { return sirius::logical_type::make(sirius::type_id::INTEGER); }
sirius::logical_type double_type() { return sirius::logical_type::make(sirius::type_id::DOUBLE); }

constexpr auto DENSE  = sirius_physical_group_join::strategy::DENSE;
constexpr auto SPARSE = sirius_physical_group_join::strategy::SPARSE;

}  // namespace

TEST_CASE("group_join value: INNER SUM dense and sparse agree with the oracle", "[group_join]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  // Duplicate preserved key 7 (presence 2) exercises the Yan-Larson scaling: SUM scales by
  // presence, matching join-then-group-by semantics.
  std::vector<std::shared_ptr<cucascade::data_batch>> preserved{
    make_numeric_batch<int32_t>(*space, {7, 7, 8, 9}, cudf::type_id::INT32)};
  std::vector<std::shared_ptr<cucascade::data_batch>> counted{
    make_kv_batch(*space, {7, 7, 8, 42}, {10, 20, 30, 99})};

  auto const expected = duckdb_cpu_oracle<int64_t>(
    {"CREATE TABLE p(k INTEGER);",
     "INSERT INTO p VALUES (7), (7), (8), (9);",
     "CREATE TABLE c(k INTEGER, v BIGINT);",
     insert_rows_sql("c", {7, 7, 8, 42}, {10, 20, 30, 99})},
    "SELECT p.k, SUM(c.v) FROM p JOIN c ON p.k = c.k GROUP BY p.k ORDER BY p.k NULLS FIRST");
  REQUIRE(expected == std::vector<value_row<int64_t>>{{7, 60}, {8, 30}});

  for (auto [max_bytes, strategy] :
       {std::pair{k_default_max_bytes, DENSE}, std::pair{k_tiny_max_bytes, SPARSE}}) {
    auto rows = run_value_group_join<int32_t, int64_t>(preserved,
                                                       counted,
                                                       {groupjoin::join_form::INNER,
                                                        groupjoin::agg_op::SUM,
                                                        bigint_type(),
                                                        std::size_t{1},
                                                        max_bytes},
                                                       cudf::type_id::INT64,
                                                       strategy);
    require_rows_match(rows, expected);
  }
}

TEST_CASE("group_join value: INNER MIN/MAX are duplicate-agnostic and drop unmatched groups",
          "[group_join]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  std::vector<std::shared_ptr<cucascade::data_batch>> preserved{
    make_numeric_batch<int32_t>(*space, {7, 7, 8, 9}, cudf::type_id::INT32)};
  std::vector<std::shared_ptr<cucascade::data_batch>> counted{
    make_kv_batch(*space, {7, 7, 8, 42}, {20, 10, 30, 99})};

  SECTION("MIN takes the raw extreme, unscaled by duplicate preserved keys")
  {
    const std::vector<value_row<int64_t>> expected{{7, 10}, {8, 30}};
    for (auto [max_bytes, strategy] :
         {std::pair{k_default_max_bytes, DENSE}, std::pair{k_tiny_max_bytes, SPARSE}}) {
      auto rows = run_value_group_join<int32_t, int64_t>(preserved,
                                                         counted,
                                                         {groupjoin::join_form::INNER,
                                                          groupjoin::agg_op::MIN,
                                                          bigint_type(),
                                                          std::size_t{1},
                                                          max_bytes},
                                                         cudf::type_id::INT64,
                                                         strategy);
      require_rows_match(rows, expected);
    }
  }
  SECTION("MAX")
  {
    const std::vector<value_row<int64_t>> expected{{7, 20}, {8, 30}};
    for (auto [max_bytes, strategy] :
         {std::pair{k_default_max_bytes, DENSE}, std::pair{k_tiny_max_bytes, SPARSE}}) {
      auto rows = run_value_group_join<int32_t, int64_t>(preserved,
                                                         counted,
                                                         {groupjoin::join_form::INNER,
                                                          groupjoin::agg_op::MAX,
                                                          bigint_type(),
                                                          std::size_t{1},
                                                          max_bytes},
                                                         cudf::type_id::INT64,
                                                         strategy);
      require_rows_match(rows, expected);
    }
  }
  SECTION("all groups unmatched: the MIN sentinel never leaks into the output")
  {
    std::vector<std::shared_ptr<cucascade::data_batch>> unmatched{
      make_kv_batch(*space, {42, 43}, {1, 2})};
    auto rows = run_value_group_join<int32_t, int64_t>(preserved,
                                                       unmatched,
                                                       {groupjoin::join_form::INNER,
                                                        groupjoin::agg_op::MIN,
                                                        bigint_type(),
                                                        std::size_t{1},
                                                        k_default_max_bytes},
                                                       cudf::type_id::INT64,
                                                       DENSE);
    REQUIRE(rows.empty());
  }
  SECTION("MIN with INTEGER declared output narrows the int64 payload")
  {
    auto rows = run_value_group_join<int32_t, int32_t>(preserved,
                                                       counted,
                                                       {groupjoin::join_form::INNER,
                                                        groupjoin::agg_op::MIN,
                                                        integer_type(),
                                                        std::size_t{1},
                                                        k_default_max_bytes},
                                                       cudf::type_id::INT32,
                                                       DENSE);
    require_rows_match(rows, std::vector<value_row<int32_t>>{{7, 10}, {8, 30}});
  }
}

TEST_CASE("group_join value: INNER COUNT under the relaxed gate column", "[group_join]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  std::vector<std::shared_ptr<cucascade::data_batch>> preserved{
    make_numeric_batch<int32_t>(*space, {7, 7, 8, 9}, cudf::type_id::INT32)};
  std::vector<std::shared_ptr<cucascade::data_batch>> counted{
    make_kv_batch(*space, {7, 7, 8, 42}, {10, 20, 30, 99})};

  // INNER drops the unmatched group 9 (no COUNT-bug row) and scales by duplicate presence.
  const std::vector<value_row<int64_t>> expected{{7, 4}, {8, 1}};
  for (auto op : {groupjoin::agg_op::COUNT_STAR, groupjoin::agg_op::COUNT_VALID}) {
    for (auto [max_bytes, strategy] :
         {std::pair{k_default_max_bytes, DENSE}, std::pair{k_tiny_max_bytes, SPARSE}}) {
      auto rows = run_value_group_join<int32_t, int64_t>(
        preserved,
        counted,
        {groupjoin::join_form::INNER,
         op,
         bigint_type(),
         op == groupjoin::agg_op::COUNT_STAR ? std::nullopt : std::optional<std::size_t>{1},
         max_bytes},
        cudf::type_id::INT64,
        strategy);
      require_rows_match(rows, expected);
    }
  }
}

TEST_CASE("group_join value: INNER AVG finalize branches", "[group_join]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  SECTION("FLOAT64 branch (integer arguments), AVG invariant under duplicate preserved keys")
  {
    std::vector<std::shared_ptr<cucascade::data_batch>> preserved{
      make_numeric_batch<int32_t>(*space, {7, 7, 8}, cudf::type_id::INT32)};
    std::vector<std::shared_ptr<cucascade::data_batch>> counted{
      make_kv_batch(*space, {7, 7, 8}, {10, 21, 30})};
    const std::vector<value_row<double>> expected{{7, 15.5}, {8, 30.0}};
    for (auto [max_bytes, strategy] :
         {std::pair{k_default_max_bytes, DENSE}, std::pair{k_tiny_max_bytes, SPARSE}}) {
      auto rows = run_value_group_join<int32_t, double>(preserved,
                                                        counted,
                                                        {groupjoin::join_form::INNER,
                                                         groupjoin::agg_op::AVG,
                                                         double_type(),
                                                         std::size_t{1},
                                                         max_bytes},
                                                        cudf::type_id::FLOAT64,
                                                        strategy);
      require_rows_match(rows, expected);
    }
  }
  SECTION("DECIMAL fixed-point branch")
  {
    std::vector<std::shared_ptr<cucascade::data_batch>> preserved{
      make_numeric_batch<int32_t>(*space, {1, 2}, cudf::type_id::INT32)};
    // DECIMAL(15,2) reps: key 1 -> {10.00, 20.00}, key 2 -> {5.50}.
    std::vector<std::shared_ptr<cucascade::data_batch>> counted{
      make_decimal_kv_batch(*space, {1, 1, 2}, {1000, 2000, 550}, -2)};
    // Declared DECIMAL(15,2): reps 1500 (15.00) and 550 (5.50).
    const std::vector<value_row<int64_t>> expected{{1, 1500}, {2, 550}};
    for (auto [max_bytes, strategy] :
         {std::pair{k_default_max_bytes, DENSE}, std::pair{k_tiny_max_bytes, SPARSE}}) {
      auto rows = run_value_group_join<int32_t, int64_t>(preserved,
                                                         counted,
                                                         {groupjoin::join_form::INNER,
                                                          groupjoin::agg_op::AVG,
                                                          sirius::logical_type::make_decimal(15, 2),
                                                          std::size_t{1},
                                                          max_bytes},
                                                         cudf::type_id::DECIMAL64,
                                                         strategy);
      require_rows_match(rows, expected);
    }
  }
}

TEST_CASE("group_join value: SUM over DECIMAL arguments casts to the declared type", "[group_join]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  std::vector<std::shared_ptr<cucascade::data_batch>> preserved{
    make_numeric_batch<int32_t>(*space, {1, 2}, cudf::type_id::INT32)};
  std::vector<std::shared_ptr<cucascade::data_batch>> counted{
    make_decimal_kv_batch(*space, {1, 1, 2}, {1000, 2000, 550}, -2)};

  SECTION("declared DECIMAL(18,2) stays a zero-copy DECIMAL64 retype")
  {
    const std::vector<value_row<int64_t>> expected{{1, 3000}, {2, 550}};
    for (auto [max_bytes, strategy] :
         {std::pair{k_default_max_bytes, DENSE}, std::pair{k_tiny_max_bytes, SPARSE}}) {
      auto rows = run_value_group_join<int32_t, int64_t>(preserved,
                                                         counted,
                                                         {groupjoin::join_form::INNER,
                                                          groupjoin::agg_op::SUM,
                                                          sirius::logical_type::make_decimal(18, 2),
                                                          std::size_t{1},
                                                          max_bytes},
                                                         cudf::type_id::DECIMAL64,
                                                         strategy);
      require_rows_match(rows, expected);
    }
  }
  SECTION("declared DECIMAL(38,2) widens to DECIMAL128, the DuckDB SUM convention")
  {
    duckdb::vector<sirius::logical_type> types;
    types.push_back(integer_type());
    types.push_back(sirius::logical_type::make_decimal(38, 2));
    sirius_physical_group_join op(
      std::move(types),
      /*estimated_cardinality=*/16,
      sirius::test::make_group_join_spec(groupjoin::join_form::INNER,
                                         groupjoin::agg_op::SUM,
                                         0,
                                         0,
                                         std::size_t{1},
                                         sirius::logical_type::make_decimal(38, 2),
                                         k_default_max_bytes));
    group_join_input input(preserved, counted);
    auto stream = default_stream();
    auto output = op.execute(input, stream);
    stream.synchronize();
    REQUIRE(op.last_strategy() == DENSE);
    auto const& out_batches =
      dynamic_cast<const pipelineable_operator_data&>(*output).get_data_batches();
    auto const view = sirius::get_cudf_table_view(*out_batches[0]);
    REQUIRE(view.column(1).type().id() == cudf::type_id::DECIMAL128);
    // Compare reps after narrowing back to DECIMAL64 on device.
    auto narrowed  = cudf::cast(view.column(1),
                               cudf::data_type{cudf::type_id::DECIMAL64, -2},
                               stream,
                               get_resource_ref(*space));
    auto const key = copy_column_to_host<int32_t>(view.column(0));
    auto const rep = copy_column_to_host<int64_t>(narrowed->view());
    REQUIRE(key == std::vector<int32_t>{1, 2});
    REQUIRE(rep == std::vector<int64_t>{3000, 550});
  }
}

TEST_CASE("group_join value: BIGINT keys run the int64-key instantiations", "[group_join]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  // Keys beyond the INT32 range prove the KeyT=int64 dispatch on both strategies -- the
  // production tpch-ext pathway keys are all BIGINT. Key 5000000000 is duplicated, 5000000003 is
  // unmatched, and the counted key 4999999999 lies outside the preserved domain (the int64
  // bounds check drops it).
  std::vector<std::shared_ptr<cucascade::data_batch>> preserved{make_numeric_batch<int64_t>(
    *space, {5'000'000'000, 5'000'000'000, 5'000'000'002, 5'000'000'003}, cudf::type_id::INT64)};

  SECTION("SUM over BIGINT arguments matches the oracle")
  {
    std::vector<std::shared_ptr<cucascade::data_batch>> counted{make_kv_batch(
      *space,
      std::vector<int64_t>{5'000'000'000, 5'000'000'000, 5'000'000'002, 4'999'999'999},
      std::vector<int64_t>{10, 20, 30, 99})};
    auto const expected = duckdb_cpu_oracle<int64_t>(
      {"CREATE TABLE p(k BIGINT);",
       "INSERT INTO p VALUES (5000000000), (5000000000), (5000000002), (5000000003);",
       "CREATE TABLE c(k BIGINT, v BIGINT);",
       insert_rows_sql(
         "c", {5'000'000'000, 5'000'000'000, 5'000'000'002, 4'999'999'999}, {10, 20, 30, 99})},
      "SELECT p.k, SUM(c.v) FROM p JOIN c ON p.k = c.k GROUP BY p.k ORDER BY p.k NULLS FIRST");
    REQUIRE(expected == std::vector<value_row<int64_t>>{{5'000'000'000, 60}, {5'000'000'002, 30}});
    for (auto [max_bytes, strategy] :
         {std::pair{k_default_max_bytes, DENSE}, std::pair{k_tiny_max_bytes, SPARSE}}) {
      auto rows = run_value_group_join<int64_t, int64_t>(preserved,
                                                         counted,
                                                         {groupjoin::join_form::INNER,
                                                          groupjoin::agg_op::SUM,
                                                          bigint_type(),
                                                          std::size_t{1},
                                                          max_bytes},
                                                         cudf::type_id::INT64,
                                                         strategy);
      require_rows_match(rows, expected);
    }
  }
  SECTION("AVG over DECIMAL(15,2) arguments, the q17 production shape")
  {
    // Reps: key 5000000000 -> {10.00, 20.00} (AVG 15.00), key 5000000002 -> {5.50}.
    std::vector<std::shared_ptr<cucascade::data_batch>> counted{
      make_decimal_kv_batch(*space,
                            std::vector<int64_t>{5'000'000'000, 5'000'000'000, 5'000'000'002},
                            {1000, 2000, 550},
                            -2)};
    const std::vector<value_row<int64_t>> expected{{5'000'000'000, 1500}, {5'000'000'002, 550}};
    for (auto [max_bytes, strategy] :
         {std::pair{k_default_max_bytes, DENSE}, std::pair{k_tiny_max_bytes, SPARSE}}) {
      auto rows = run_value_group_join<int64_t, int64_t>(preserved,
                                                         counted,
                                                         {groupjoin::join_form::INNER,
                                                          groupjoin::agg_op::AVG,
                                                          sirius::logical_type::make_decimal(15, 2),
                                                          std::size_t{1},
                                                          max_bytes},
                                                         cudf::type_id::DECIMAL64,
                                                         strategy);
      require_rows_match(rows, expected);
    }
  }
}

TEST_CASE("group_join value: INT32 and DECIMAL32 arguments run the int32-argument instantiations",
          "[group_join]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  std::vector<std::shared_ptr<cucascade::data_batch>> preserved{
    make_numeric_batch<int32_t>(*space, {1, 2, 4}, cudf::type_id::INT32)};

  SECTION("MIN over INT32 arguments with INTEGER output matches the oracle")
  {
    std::vector<std::shared_ptr<cucascade::data_batch>> counted{
      make_kv_batch(*space, std::vector<int32_t>{1, 1, 2}, std::vector<int32_t>{30, 10, 7})};
    auto const expected =
      duckdb_cpu_oracle<int32_t>({"CREATE TABLE p(k INTEGER);",
                                  "INSERT INTO p VALUES (1), (2), (4);",
                                  "CREATE TABLE c(k INTEGER, v INTEGER);",
                                  insert_rows_sql("c", {1, 1, 2}, {30, 10, 7})},
                                 "SELECT p.k, MIN(c.v) FROM p JOIN c ON p.k = c.k GROUP BY p.k "
                                 "ORDER BY p.k NULLS FIRST");
    REQUIRE(expected == std::vector<value_row<int32_t>>{{1, 10}, {2, 7}});
    for (auto [max_bytes, strategy] :
         {std::pair{k_default_max_bytes, DENSE}, std::pair{k_tiny_max_bytes, SPARSE}}) {
      auto rows = run_value_group_join<int32_t, int32_t>(preserved,
                                                         counted,
                                                         {groupjoin::join_form::INNER,
                                                          groupjoin::agg_op::MIN,
                                                          integer_type(),
                                                          std::size_t{1},
                                                          max_bytes},
                                                         cudf::type_id::INT32,
                                                         strategy);
      require_rows_match(rows, expected);
    }
  }
  SECTION("SUM over DECIMAL32 arguments rescales into the declared DECIMAL64 output")
  {
    // DECIMAL(9,2) reps: key 1 -> {10.00, 20.00}, key 2 -> {5.50}; declared DECIMAL(18,2).
    std::vector<std::shared_ptr<cucascade::data_batch>> counted{
      make_decimal32_kv_batch(*space, {1, 1, 2}, {1000, 2000, 550}, -2)};
    const std::vector<value_row<int64_t>> expected{{1, 3000}, {2, 550}};
    for (auto [max_bytes, strategy] :
         {std::pair{k_default_max_bytes, DENSE}, std::pair{k_tiny_max_bytes, SPARSE}}) {
      auto rows = run_value_group_join<int32_t, int64_t>(preserved,
                                                         counted,
                                                         {groupjoin::join_form::INNER,
                                                          groupjoin::agg_op::SUM,
                                                          sirius::logical_type::make_decimal(18, 2),
                                                          std::size_t{1},
                                                          max_bytes},
                                                         cudf::type_id::DECIMAL64,
                                                         strategy);
      require_rows_match(rows, expected);
    }
  }
}

TEST_CASE("group_join value: DIRECT forms group NULL keys as a real group", "[group_join]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  // keys [1, NULL, NULL, 1], args [10, 20, 30, 5].
  std::vector<std::shared_ptr<cucascade::data_batch>> counted{make_kv_batch_with_nulls(
    *space, {1, 0, 0, 1}, {true, false, false, true}, {10, 20, 30, 5}, {true, true, true, true})};

  SECTION("SUM")
  {
    const std::vector<value_row<int64_t>> expected{{std::nullopt, 50}, {1, 15}};
    for (auto [max_bytes, strategy] :
         {std::pair{k_default_max_bytes, DENSE}, std::pair{k_tiny_max_bytes, SPARSE}}) {
      auto rows = run_value_group_join<int32_t, int64_t>({},
                                                         counted,
                                                         {groupjoin::join_form::DIRECT,
                                                          groupjoin::agg_op::SUM,
                                                          bigint_type(),
                                                          std::size_t{1},
                                                          max_bytes},
                                                         cudf::type_id::INT64,
                                                         strategy);
      require_rows_match(rows, expected);
    }
  }
  SECTION("COUNT(*) counts the NULL-group rows")
  {
    const std::vector<value_row<int64_t>> expected{{std::nullopt, 2}, {1, 2}};
    // The payload-less COUNT state is 2 u32 slots (8 bytes), so forcing sparse needs a budget
    // below even that.
    for (auto [max_bytes, strategy] :
         {std::pair{k_default_max_bytes, DENSE}, std::pair{uint64_t{4}, SPARSE}}) {
      auto rows = run_value_group_join<int32_t, int64_t>({},
                                                         counted,
                                                         {groupjoin::join_form::DIRECT,
                                                          groupjoin::agg_op::COUNT_STAR,
                                                          bigint_type(),
                                                          std::nullopt,
                                                          max_bytes},
                                                         cudf::type_id::INT64,
                                                         strategy);
      require_rows_match(rows, expected);
    }
  }
  SECTION("MIN reaches the dense sentinel machinery for the NULL-group slot")
  {
    const std::vector<value_row<int64_t>> expected{{std::nullopt, 20}, {1, 5}};
    for (auto [max_bytes, strategy] :
         {std::pair{k_default_max_bytes, DENSE}, std::pair{k_tiny_max_bytes, SPARSE}}) {
      auto rows = run_value_group_join<int32_t, int64_t>({},
                                                         counted,
                                                         {groupjoin::join_form::DIRECT,
                                                          groupjoin::agg_op::MIN,
                                                          bigint_type(),
                                                          std::size_t{1},
                                                          max_bytes},
                                                         cudf::type_id::INT64,
                                                         strategy);
      require_rows_match(rows, expected);
    }
  }
  SECTION("AVG")
  {
    const std::vector<value_row<double>> expected{{std::nullopt, 25.0}, {1, 7.5}};
    for (auto [max_bytes, strategy] :
         {std::pair{k_default_max_bytes, DENSE}, std::pair{k_tiny_max_bytes, SPARSE}}) {
      auto rows = run_value_group_join<int32_t, double>({},
                                                        counted,
                                                        {groupjoin::join_form::DIRECT,
                                                         groupjoin::agg_op::AVG,
                                                         double_type(),
                                                         std::size_t{1},
                                                         max_bytes},
                                                        cudf::type_id::FLOAT64,
                                                        strategy);
      require_rows_match(rows, expected);
    }
  }
}

TEST_CASE("group_join value: COUNT accepts narrow argument carriers as validity-only input",
          "[group_join]")
{
  // The narrowing policy may leave a COUNT argument on a narrow carrier (INT8 here): COUNT reads
  // only the validity mask, so both strategies must accept the column untouched.
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  auto make_narrow_arg_batch = [&](const std::vector<int32_t>& keys,
                                   const std::vector<int8_t>& args,
                                   const std::vector<bool>& arg_valids) {
    auto key_batch = make_numeric_batch<int32_t>(*space, keys, cudf::type_id::INT32);
    auto arg_batch =
      make_numeric_batch_with_nulls<int8_t>(*space, args, arg_valids, cudf::type_id::INT8);
    return concatenate_batches_horizontal({key_batch, arg_batch}, *space);
  };

  SECTION("DIRECT: NULL-free narrow arguments stay dense; a NULL argument routes to sparse")
  {
    std::vector<std::shared_ptr<cucascade::data_batch>> dense_input{
      make_narrow_arg_batch({1, 2, 1, 2, 2}, {1, 2, 3, 4, 5}, {true, true, true, true, true})};
    auto rows = run_value_group_join<int32_t, int64_t>({},
                                                       dense_input,
                                                       {groupjoin::join_form::DIRECT,
                                                        groupjoin::agg_op::COUNT_VALID,
                                                        bigint_type(),
                                                        std::size_t{1},
                                                        k_default_max_bytes},
                                                       cudf::type_id::INT64,
                                                       DENSE);
    require_rows_match(rows, {{1, 2}, {2, 3}});

    std::vector<std::shared_ptr<cucascade::data_batch>> nullable_input{
      make_narrow_arg_batch({1, 2, 1, 2, 2}, {1, 2, 3, 4, 5}, {true, false, true, true, false})};
    auto sparse_rows = run_value_group_join<int32_t, int64_t>({},
                                                              nullable_input,
                                                              {groupjoin::join_form::DIRECT,
                                                               groupjoin::agg_op::COUNT_VALID,
                                                               bigint_type(),
                                                               std::size_t{1},
                                                               k_default_max_bytes},
                                                              cudf::type_id::INT64,
                                                              SPARSE);
    require_rows_match(sparse_rows, {{1, 2}, {2, 1}});
  }

  SECTION("INNER: the narrow argument multiplies through the Yan-Larson scaling")
  {
    std::vector<std::shared_ptr<cucascade::data_batch>> preserved{
      make_kv_batch(*space, {1, 2, 2}, {0, 0, 0})};
    std::vector<std::shared_ptr<cucascade::data_batch>> counted{
      make_narrow_arg_batch({1, 2, 1, 3}, {7, 8, 9, 1}, {true, true, true, true})};
    auto rows = run_value_group_join<int32_t, int64_t>(preserved,
                                                       counted,
                                                       {groupjoin::join_form::INNER,
                                                        groupjoin::agg_op::COUNT_VALID,
                                                        bigint_type(),
                                                        std::size_t{1},
                                                        k_default_max_bytes},
                                                       cudf::type_id::INT64,
                                                       DENSE);
    require_rows_match(rows, {{1, 2}, {2, 2}});
  }
}

TEST_CASE("group_join value: argument-validity gate routes NULL arguments to sparse",
          "[group_join]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  SECTION("INNER: key matches with all-NULL arguments keep the row, SUM NULL and COUNT(col) 0")
  {
    std::vector<std::shared_ptr<cucascade::data_batch>> preserved{
      make_numeric_batch<int32_t>(*space, {1, 2, 2, 3}, cudf::type_id::INT32)};
    // key 1 -> mixed NULLs, key 2 -> all NULL, key 3 -> unmatched.
    std::vector<std::shared_ptr<cucascade::data_batch>> counted{make_kv_batch_with_nulls(
      *space, {1, 1, 2, 2}, {true, true, true, true}, {10, 0, 0, 0}, {true, false, false, false})};

    auto const setup = std::vector<std::string>{
      "CREATE TABLE p(k INTEGER);",
      "INSERT INTO p VALUES (1), (2), (2), (3);",
      "CREATE TABLE c(k INTEGER, v BIGINT);",
      insert_rows_sql(
        "c", {1, 1, 2, 2}, {std::optional<int64_t>{10}, std::nullopt, std::nullopt, std::nullopt})};

    auto sum_rows     = run_value_group_join<int32_t, int64_t>(preserved,
                                                           counted,
                                                               {groupjoin::join_form::INNER,
                                                                groupjoin::agg_op::SUM,
                                                                bigint_type(),
                                                                std::size_t{1},
                                                                k_default_max_bytes},
                                                           cudf::type_id::INT64,
                                                           SPARSE);
    auto sum_expected = duckdb_cpu_oracle<int64_t>(
      setup,
      "SELECT p.k, SUM(c.v) FROM p JOIN c ON p.k = c.k GROUP BY p.k ORDER BY p.k NULLS FIRST");
    // NULL x presence = NULL: the duplicated preserved key 2 keeps its NULL sum.
    REQUIRE(sum_expected == std::vector<value_row<int64_t>>{{1, 10}, {2, std::nullopt}});
    require_rows_match(sum_rows, sum_expected);

    auto count_rows = run_value_group_join<int32_t, int64_t>(preserved,
                                                             counted,
                                                             {groupjoin::join_form::INNER,
                                                              groupjoin::agg_op::COUNT_VALID,
                                                              bigint_type(),
                                                              std::size_t{1},
                                                              k_default_max_bytes},
                                                             cudf::type_id::INT64,
                                                             SPARSE);
    // Key 1 joins one valid and one NULL argument row (COUNT(col) 1); key 2's duplicated
    // preserved rows join only NULL-argument rows (kept with COUNT(col) 0).
    require_rows_match(count_rows, std::vector<value_row<int64_t>>{{1, 1}, {2, 0}});

    auto min_rows = run_value_group_join<int32_t, int64_t>(preserved,
                                                           counted,
                                                           {groupjoin::join_form::INNER,
                                                            groupjoin::agg_op::MIN,
                                                            bigint_type(),
                                                            std::size_t{1},
                                                            k_default_max_bytes},
                                                           cudf::type_id::INT64,
                                                           SPARSE);
    require_rows_match(min_rows, std::vector<value_row<int64_t>>{{1, 10}, {2, std::nullopt}});

    auto avg_rows = run_value_group_join<int32_t, double>(preserved,
                                                          counted,
                                                          {groupjoin::join_form::INNER,
                                                           groupjoin::agg_op::AVG,
                                                           double_type(),
                                                           std::size_t{1},
                                                           k_default_max_bytes},
                                                          cudf::type_id::FLOAT64,
                                                          SPARSE);
    require_rows_match(avg_rows, std::vector<value_row<double>>{{1, 10.0}, {2, std::nullopt}});
  }

  SECTION("DIRECT over outer-join-shaped data: padded groups emit NULL")
  {
    // The materialized output of `customer LEFT JOIN orders`: customers 4 and 5 have no orders,
    // so their padded rows carry NULL arguments.
    std::vector<std::shared_ptr<cucascade::data_batch>> counted{make_kv_batch_with_nulls(
      *space, {1, 1, 4, 5}, {true, true, true, true}, {10, 20, 0, 0}, {true, true, false, false})};
    auto rows     = run_value_group_join<int32_t, int64_t>({},
                                                       counted,
                                                           {groupjoin::join_form::DIRECT,
                                                            groupjoin::agg_op::SUM,
                                                            bigint_type(),
                                                            std::size_t{1},
                                                            k_default_max_bytes},
                                                       cudf::type_id::INT64,
                                                       SPARSE);
    auto expected = duckdb_cpu_oracle<int64_t>(
      {"CREATE TABLE c(k INTEGER, v BIGINT);",
       insert_rows_sql(
         "c",
         {1, 1, 4, 5},
         {std::optional<int64_t>{10}, std::optional<int64_t>{20}, std::nullopt, std::nullopt})},
      "SELECT k, SUM(v) FROM c GROUP BY k ORDER BY k NULLS FIRST");
    REQUIRE(expected ==
            std::vector<value_row<int64_t>>{{1, 30}, {4, std::nullopt}, {5, std::nullopt}});
    require_rows_match(rows, expected);
  }
}

TEST_CASE("group_join value: multiple counted batches accumulate (dense) and merge (sparse)",
          "[group_join]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  std::vector<std::shared_ptr<cucascade::data_batch>> preserved{
    make_numeric_batch<int32_t>(*space, {1, 2, 3, 4}, cudf::type_id::INT32)};
  // Three batches exercise multi-batch dense accumulation and the odd-count carry of the
  // pairwise sparse merge (batch 3 rides unmerged into the second round).
  std::vector<std::shared_ptr<cucascade::data_batch>> counted{
    make_kv_batch(*space, {1, 2}, {10, 20}),
    make_kv_batch(*space, {1, 4}, {5, 40}),
    make_kv_batch(*space, {2}, {7})};

  auto const expected = duckdb_cpu_oracle<int64_t>(
    {"CREATE TABLE p(k INTEGER);",
     "INSERT INTO p VALUES (1), (2), (3), (4);",
     "CREATE TABLE c(k INTEGER, v BIGINT);",
     insert_rows_sql("c", {1, 2, 1, 4, 2}, {10, 20, 5, 40, 7})},
    "SELECT p.k, SUM(c.v) FROM p JOIN c ON p.k = c.k GROUP BY p.k ORDER BY p.k NULLS FIRST");
  REQUIRE(expected == std::vector<value_row<int64_t>>{{1, 15}, {2, 27}, {4, 40}});

  for (auto [max_bytes, strategy] :
       {std::pair{k_default_max_bytes, DENSE}, std::pair{k_tiny_max_bytes, SPARSE}}) {
    auto rows = run_value_group_join<int32_t, int64_t>(preserved,
                                                       counted,
                                                       {groupjoin::join_form::INNER,
                                                        groupjoin::agg_op::SUM,
                                                        bigint_type(),
                                                        std::size_t{1},
                                                        max_bytes},
                                                       cudf::type_id::INT64,
                                                       strategy);
    require_rows_match(rows, expected);
  }
}

TEST_CASE("group_join value: sparse merge keeps all-NULL-argument groups NULL across batches",
          "[group_join]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  std::vector<std::shared_ptr<cucascade::data_batch>> preserved{
    make_numeric_batch<int32_t>(*space, {1, 2, 3, 4}, cudf::type_id::INT32)};
  // Key 1 is valid in every batch, key 2 is all-NULL in every batch, key 3 mixes an all-NULL
  // batch with a valid one, and key 4 appears once. The NULL arguments route execution to the
  // sparse strategy; three batches exercise the odd-count carry of the pairwise merge.
  std::vector<std::shared_ptr<cucascade::data_batch>> counted{
    make_kv_batch_with_nulls(
      *space, {1, 2, 3}, {true, true, true}, {10, 0, 0}, {true, false, false}),
    make_kv_batch_with_nulls(
      *space, {1, 2, 4}, {true, true, true}, {5, 0, 40}, {true, false, true}),
    make_kv_batch_with_nulls(*space, {2, 3}, {true, true}, {0, 30}, {false, true})};

  auto const setup = std::vector<std::string>{"CREATE TABLE p(k INTEGER);",
                                              "INSERT INTO p VALUES (1), (2), (3), (4);",
                                              "CREATE TABLE c(k INTEGER, v BIGINT);",
                                              insert_rows_sql("c",
                                                              {1, 2, 3, 1, 2, 4, 2, 3},
                                                              {std::optional<int64_t>{10},
                                                               std::nullopt,
                                                               std::nullopt,
                                                               std::optional<int64_t>{5},
                                                               std::nullopt,
                                                               std::optional<int64_t>{40},
                                                               std::nullopt,
                                                               std::optional<int64_t>{30}})};

  SECTION("SUM: a valid partial survives NULL partials, all-NULL stays NULL")
  {
    auto const expected = duckdb_cpu_oracle<int64_t>(
      setup,
      "SELECT p.k, SUM(c.v) FROM p JOIN c ON p.k = c.k GROUP BY p.k ORDER BY p.k NULLS FIRST");
    REQUIRE(expected ==
            std::vector<value_row<int64_t>>{{1, 15}, {2, std::nullopt}, {3, 30}, {4, 40}});
    auto rows = run_value_group_join<int32_t, int64_t>(preserved,
                                                       counted,
                                                       {groupjoin::join_form::INNER,
                                                        groupjoin::agg_op::SUM,
                                                        bigint_type(),
                                                        std::size_t{1},
                                                        k_default_max_bytes},
                                                       cudf::type_id::INT64,
                                                       SPARSE);
    require_rows_match(rows, expected);
  }
  SECTION("AVG: the sum and valid-count columns merge independently")
  {
    auto const expected = duckdb_cpu_oracle<double>(
      setup,
      "SELECT p.k, AVG(c.v) FROM p JOIN c ON p.k = c.k GROUP BY p.k ORDER BY p.k NULLS FIRST");
    REQUIRE(expected ==
            std::vector<value_row<double>>{{1, 7.5}, {2, std::nullopt}, {3, 30.0}, {4, 40.0}});
    auto rows = run_value_group_join<int32_t, double>(preserved,
                                                      counted,
                                                      {groupjoin::join_form::INNER,
                                                       groupjoin::agg_op::AVG,
                                                       double_type(),
                                                       std::size_t{1},
                                                       k_default_max_bytes},
                                                      cudf::type_id::FLOAT64,
                                                      SPARSE);
    require_rows_match(rows, expected);
  }
}

TEST_CASE("group_join value: SUM overflow bound declines dense to the exact sparse path",
          "[group_join]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  std::vector<std::shared_ptr<cucascade::data_batch>> preserved{
    make_numeric_batch<int32_t>(*space, {1}, cudf::type_id::INT32)};

  SECTION("inconclusive bound: 2 rows x 6e18 magnitude exceeds INT64_MAX")
  {
    std::vector<std::shared_ptr<cucascade::data_batch>> counted{
      make_kv_batch(*space, {1, 1}, {6'000'000'000'000'000'000LL, -5'000'000'000'000'000'000LL})};
    auto rows = run_value_group_join<int32_t, int64_t>(preserved,
                                                       counted,
                                                       {groupjoin::join_form::INNER,
                                                        groupjoin::agg_op::SUM,
                                                        bigint_type(),
                                                        std::size_t{1},
                                                        k_default_max_bytes},
                                                       cudf::type_id::INT64,
                                                       SPARSE);
    require_rows_match(rows, std::vector<value_row<int64_t>>{{1, 1'000'000'000'000'000'000LL}});
  }
  SECTION("conclusive bound: 2 rows x 4e18 magnitude fits, dense stays")
  {
    std::vector<std::shared_ptr<cucascade::data_batch>> counted{
      make_kv_batch(*space, {1, 1}, {4'000'000'000'000'000'000LL, -3'000'000'000'000'000'000LL})};
    auto rows = run_value_group_join<int32_t, int64_t>(preserved,
                                                       counted,
                                                       {groupjoin::join_form::INNER,
                                                        groupjoin::agg_op::SUM,
                                                        bigint_type(),
                                                        std::size_t{1},
                                                        k_default_max_bytes},
                                                       cudf::type_id::INT64,
                                                       DENSE);
    require_rows_match(rows, std::vector<value_row<int64_t>>{{1, 1'000'000'000'000'000'000LL}});
  }
}

TEST_CASE("group_join value: INNER COUNT product overflow throws on both strategies",
          "[group_join]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  // The exact product validation arms only when preserved_rows x counted_rows can top BIGINT max,
  // so both sides repeat one single-key 15.5M-row batch 200 times: 3.1e9 rows per side arm the
  // host gate from 62 MB of device memory, and the lone group's presence x matched product
  // (~9.61e18) exceeds INT64_MAX on the dense flag and the sparse validator alike. The u32 slot
  // widths hold (3.1e9 < 2^32), so only the product overflows, not the counters; the modest batch
  // size keeps the sparse strategy's per-batch groupby scratch inside the 2 GiB test GPU space.
  constexpr int32_t batch_rows  = 15'500'000;
  constexpr std::size_t repeats = 200;
  auto batch =
    make_numeric_batch<int32_t>(*space, std::vector<int32_t>(batch_rows, 7), cudf::type_id::INT32);
  std::vector<std::shared_ptr<cucascade::data_batch>> side(repeats, batch);

  // The payload-less COUNT state is 2 u32 slots (8 bytes), so forcing sparse needs a budget below
  // even that.
  for (auto [max_bytes, strategy] :
       {std::pair{k_default_max_bytes, DENSE}, std::pair{uint64_t{4}, SPARSE}}) {
    duckdb::vector<sirius::logical_type> types;
    types.push_back(integer_type());
    types.push_back(bigint_type());
    sirius_physical_group_join op(std::move(types),
                                  /*estimated_cardinality=*/1,
                                  sirius::test::make_group_join_spec(groupjoin::join_form::INNER,
                                                                     groupjoin::agg_op::COUNT_STAR,
                                                                     /*preserved_key_idx=*/0,
                                                                     /*counted_key_idx=*/0,
                                                                     std::nullopt,
                                                                     bigint_type(),
                                                                     max_bytes));
    group_join_input input(side, side);
    REQUIRE_THROWS_WITH(op.execute(input, default_stream()),
                        Catch::Contains("COUNT result exceeds BIGINT max"));
    REQUIRE(op.last_strategy() == strategy);
  }
}

TEST_CASE("group_join value: NULL keys on the INNER form never match", "[group_join]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  std::vector<std::shared_ptr<cucascade::data_batch>> preserved{
    make_numeric_batch_with_nulls<int32_t>(*space, {1, 0}, {true, false}, cudf::type_id::INT32)};
  std::vector<std::shared_ptr<cucascade::data_batch>> counted{
    make_kv_batch_with_nulls(*space, {1, 0}, {true, false}, {5, 7}, {true, true})};

  const std::vector<value_row<int64_t>> expected{{1, 5}};
  for (auto [max_bytes, strategy] :
       {std::pair{k_default_max_bytes, DENSE}, std::pair{k_tiny_max_bytes, SPARSE}}) {
    auto rows = run_value_group_join<int32_t, int64_t>(preserved,
                                                       counted,
                                                       {groupjoin::join_form::INNER,
                                                        groupjoin::agg_op::SUM,
                                                        bigint_type(),
                                                        std::size_t{1},
                                                        max_bytes},
                                                       cudf::type_id::INT64,
                                                       strategy);
    require_rows_match(rows, expected);
  }
}

TEST_CASE("group_join value: empty inputs emit empty outputs", "[group_join]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  std::vector<std::shared_ptr<cucascade::data_batch>> preserved{
    make_numeric_batch<int32_t>(*space, {1, 2}, cudf::type_id::INT32)};
  std::vector<std::shared_ptr<cucascade::data_batch>> counted{
    make_kv_batch(*space, {1, 2}, {10, 20})};

  SECTION("INNER: empty counted side drops every group")
  {
    auto rows = run_value_group_join<int32_t, int64_t>(preserved,
                                                       {},
                                                       {groupjoin::join_form::INNER,
                                                        groupjoin::agg_op::SUM,
                                                        bigint_type(),
                                                        std::size_t{1},
                                                        k_default_max_bytes},
                                                       cudf::type_id::INT64,
                                                       DENSE);
    REQUIRE(rows.empty());
  }
  SECTION("INNER: empty preserved side emits no groups")
  {
    auto rows = run_value_group_join<int32_t, int64_t>({},
                                                       counted,
                                                       {groupjoin::join_form::INNER,
                                                        groupjoin::agg_op::SUM,
                                                        bigint_type(),
                                                        std::size_t{1},
                                                        k_default_max_bytes},
                                                       cudf::type_id::INT64,
                                                       DENSE);
    REQUIRE(rows.empty());
  }
  SECTION("both sides empty: no task input exists, and batch-less execution fails closed")
  {
    // With zero batches on both ports get_next_task_input_data returns nullptr and no task ever
    // runs; a batch-less execute has no memory space to allocate from and must throw rather than
    // fabricate output.
    duckdb::vector<sirius::logical_type> types;
    types.push_back(integer_type());
    types.push_back(bigint_type());
    sirius_physical_group_join op(std::move(types),
                                  /*estimated_cardinality=*/1,
                                  sirius::test::make_group_join_spec(groupjoin::join_form::INNER,
                                                                     groupjoin::agg_op::SUM,
                                                                     0,
                                                                     0,
                                                                     std::size_t{1},
                                                                     bigint_type(),
                                                                     k_default_max_bytes));
    group_join_input input({}, {});
    REQUIRE_THROWS_WITH(op.execute(input, default_stream()), Catch::Contains("no memory space"));
  }
  SECTION("DIRECT: empty-batch input rows mean no groups")
  {
    std::vector<std::shared_ptr<cucascade::data_batch>> empty_batch{make_kv_batch(*space, {}, {})};
    auto rows = run_value_group_join<int32_t, int64_t>({},
                                                       empty_batch,
                                                       {groupjoin::join_form::DIRECT,
                                                        groupjoin::agg_op::SUM,
                                                        bigint_type(),
                                                        std::size_t{1},
                                                        k_default_max_bytes},
                                                       cudf::type_id::INT64,
                                                       DENSE);
    REQUIRE(rows.empty());
  }
  SECTION("DIRECT: all-NULL keys still form the NULL group (via sparse)")
  {
    std::vector<std::shared_ptr<cucascade::data_batch>> all_null{
      make_kv_batch_with_nulls(*space, {0, 0}, {false, false}, {4, 6}, {true, true})};
    auto rows = run_value_group_join<int32_t, int64_t>({},
                                                       all_null,
                                                       {groupjoin::join_form::DIRECT,
                                                        groupjoin::agg_op::SUM,
                                                        bigint_type(),
                                                        std::size_t{1},
                                                        k_default_max_bytes},
                                                       cudf::type_id::INT64,
                                                       SPARSE);
    require_rows_match(rows, std::vector<value_row<int64_t>>{{std::nullopt, 10}});
  }
}

TEST_CASE("group_join value: wide slot widths match the narrow result", "[group_join]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);
  auto mr     = get_resource_ref(*space);
  auto stream = default_stream();

  auto preserved = make_numeric_batch<int32_t>(*space, {5, 6, 6, 8}, cudf::type_id::INT32);
  auto counted   = make_kv_batch(*space, {6, 6, 6, 9, 4}, {3, 1, 2, 99, 99});
  std::vector<cudf::column_view> const preserved_keys{
    sirius::get_cudf_table_view(*preserved).column(0)};
  std::vector<cudf::column_view> const counted_keys{
    sirius::get_cudf_table_view(*counted).column(0)};
  std::vector<cudf::column_view> const counted_args{
    sirius::get_cudf_table_view(*counted).column(1)};

  auto read_pairs = [&](std::unique_ptr<cudf::table> table) {
    auto const keys   = copy_column_to_host<int32_t>(table->view().column(0));
    auto const values = copy_column_to_host<int64_t>(table->view().column(1));
    std::vector<std::pair<int32_t, int64_t>> rows;
    for (std::size_t i = 0; i < keys.size(); ++i) {
      rows.emplace_back(keys[i], values[i]);
    }
    return rows;
  };

  SECTION("INNER SUM across all four width combinations")
  {
    // Key 6 has presence 2 and sum 6 -> scaled 12.
    const std::vector<std::pair<int32_t, int64_t>> expected{{6, 12}};
    auto run = [&]<typename PresenceT, typename MatchedT>() {
      return read_pairs(group_join_dense_inner<int32_t, PresenceT, MatchedT, int64_t>(
        groupjoin::dense_value_op::SUM,
        /*min_key=*/5,
        /*range=*/4,
        preserved_keys,
        counted_keys,
        counted_args,
        cudf::data_type{cudf::type_id::INT32},
        /*check_count_product_overflow=*/false,
        stream,
        mr));
    };
    REQUIRE(run.template operator()<uint32_t, uint32_t>() == expected);
    REQUIRE(run.template operator()<uint32_t, uint64_t>() == expected);
    REQUIRE(run.template operator()<uint64_t, uint32_t>() == expected);
    REQUIRE(run.template operator()<uint64_t, uint64_t>() == expected);
  }
  SECTION("DIRECT MIN across matched widths")
  {
    const std::vector<std::pair<int32_t, int64_t>> expected{{4, 99}, {6, 1}, {9, 99}};
    auto run = [&]<typename MatchedT>() {
      auto rows = read_pairs(
        group_join_dense_direct<int32_t, MatchedT, int64_t>(groupjoin::dense_value_op::MIN,
                                                            /*min_key=*/4,
                                                            /*range=*/6,
                                                            counted_keys,
                                                            counted_args,
                                                            cudf::data_type{cudf::type_id::INT32},
                                                            /*has_null_group=*/false,
                                                            stream,
                                                            mr));
      std::sort(rows.begin(), rows.end());
      return rows;
    };
    REQUIRE(run.template operator()<uint32_t>() == expected);
    REQUIRE(run.template operator()<uint64_t>() == expected);
  }
}

TEST_CASE("group_join value: peak estimate covers observed dense driver allocations",
          "[group_join][no_history_peak_memory_estimate]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);
  auto stream = default_stream();

  // A dense-eligible shape: contiguous keys 0..N-1 on both sides.
  constexpr int32_t num_keys = 50'000;
  std::vector<int32_t> keys(num_keys);
  std::vector<int64_t> args(num_keys);
  for (int32_t i = 0; i < num_keys; ++i) {
    keys[i] = i;
    args[i] = i % 97;
  }
  auto preserved = make_numeric_batch<int32_t>(*space, keys, cudf::type_id::INT32);
  auto counted   = make_kv_batch(*space, keys, args);
  std::vector<cudf::column_view> const preserved_keys{
    sirius::get_cudf_table_view(*preserved).column(0)};
  std::vector<cudf::column_view> const counted_keys{
    sirius::get_cudf_table_view(*counted).column(0)};
  std::vector<cudf::column_view> const counted_args{
    sirius::get_cudf_table_view(*counted).column(1)};

  std::size_t const input_bytes =
    static_cast<std::size_t>(num_keys) * (sizeof(int32_t) * 2 + sizeof(int64_t));

  auto estimate_for = [&](groupjoin::join_form form,
                          groupjoin::agg_op op,
                          std::optional<std::size_t> arg_idx,
                          sirius::logical_type output_type) {
    duckdb::vector<sirius::logical_type> types;
    types.push_back(integer_type());
    types.push_back(output_type);
    sirius_physical_group_join gj(std::move(types),
                                  /*estimated_cardinality=*/num_keys,
                                  sirius::test::make_group_join_spec(
                                    form, op, 0, 0, arg_idx, output_type, k_default_max_bytes));
    return gj.no_history_peak_memory_estimate({2, input_bytes});
  };

  // A fresh statistics adaptor per driver run measures that run's true allocation peak.
  auto observed_peak = [&](auto&& run_driver) {
    rmm::mr::statistics_resource_adaptor stats_mr{rmm::mr::get_current_device_resource_ref()};
    auto table = run_driver(stats_mr);
    stream.synchronize();
    table.reset();
    return static_cast<std::size_t>(stats_mr.get_bytes_counter().peak);
  };

  auto require_covered = [&](groupjoin::dense_value_op dense_op,
                             groupjoin::agg_op op,
                             std::optional<std::size_t> arg_idx,
                             sirius::logical_type output_type) {
    auto const peak = observed_peak([&](auto& stats_mr) {
      return group_join_dense_inner<int32_t, uint32_t, uint32_t, int64_t>(
        dense_op,
        0,
        num_keys,
        preserved_keys,
        counted_keys,
        counted_args,
        cudf::data_type{cudf::type_id::INT32},
        false,
        stream,
        stats_mr);
    });
    REQUIRE(estimate_for(groupjoin::join_form::INNER, op, arg_idx, output_type) >= peak);
  };
  require_covered(
    groupjoin::dense_value_op::COUNT, groupjoin::agg_op::COUNT_STAR, std::nullopt, bigint_type());
  require_covered(
    groupjoin::dense_value_op::SUM, groupjoin::agg_op::SUM, std::size_t{1}, bigint_type());
  require_covered(
    groupjoin::dense_value_op::MIN, groupjoin::agg_op::MIN, std::size_t{1}, bigint_type());
  require_covered(
    groupjoin::dense_value_op::AVG, groupjoin::agg_op::AVG, std::size_t{1}, double_type());

  auto const direct_peak = observed_peak([&](auto& stats_mr) {
    return group_join_dense_direct<int32_t, uint32_t, int64_t>(
      groupjoin::dense_value_op::SUM,
      0,
      num_keys,
      counted_keys,
      counted_args,
      cudf::data_type{cudf::type_id::INT32},
      /*has_null_group=*/false,
      stream,
      stats_mr);
  });
  REQUIRE(estimate_for(
            groupjoin::join_form::DIRECT, groupjoin::agg_op::SUM, std::size_t{1}, bigint_type()) >=
          direct_peak);
}

TEST_CASE("group_join value: spec validation fails closed", "[group_join][validation]")
{
  auto make_op = [](groupjoin::group_join_spec spec, duckdb::vector<sirius::logical_type> types) {
    return std::make_unique<sirius_physical_group_join>(std::move(types),
                                                        /*estimated_cardinality=*/1,
                                                        std::move(spec));
  };
  auto two_types = [](sirius::logical_type value_type) {
    duckdb::vector<sirius::logical_type> types;
    types.push_back(sirius::logical_type::make(sirius::type_id::INTEGER));
    types.push_back(std::move(value_type));
    return types;
  };

  SECTION("value bundles over OUTER_PRESERVING are rejected (the value-over-outer seam)")
  {
    auto spec = sirius::test::make_group_join_spec(groupjoin::join_form::OUTER_PRESERVING,
                                                   groupjoin::agg_op::SUM,
                                                   0,
                                                   0,
                                                   std::size_t{1},
                                                   bigint_type(),
                                                   k_default_max_bytes);
    REQUIRE_THROWS_WITH(make_op(std::move(spec), two_types(bigint_type())),
                        Catch::Contains("value bundles over OUTER_PRESERVING"));
  }
  SECTION("multi-slot specs are rejected")
  {
    auto spec = sirius::test::make_group_join_spec(groupjoin::join_form::INNER,
                                                   groupjoin::agg_op::SUM,
                                                   0,
                                                   0,
                                                   std::size_t{1},
                                                   bigint_type(),
                                                   k_default_max_bytes);
    spec.slots.push_back(spec.slots[0]);
    REQUIRE_THROWS_WITH(make_op(std::move(spec), two_types(bigint_type())),
                        Catch::Contains("exactly 1 aggregate slot"));
  }
  SECTION("COUNT(*) with an argument and SUM without one are rejected")
  {
    auto starred = sirius::test::make_group_join_spec(groupjoin::join_form::INNER,
                                                      groupjoin::agg_op::COUNT_STAR,
                                                      0,
                                                      0,
                                                      std::size_t{1},
                                                      bigint_type(),
                                                      k_default_max_bytes);
    REQUIRE_THROWS_WITH(make_op(std::move(starred), two_types(bigint_type())),
                        Catch::Contains("COUNT(*) forbids an argument column"));
    auto argless = sirius::test::make_group_join_spec(groupjoin::join_form::INNER,
                                                      groupjoin::agg_op::SUM,
                                                      0,
                                                      0,
                                                      std::nullopt,
                                                      bigint_type(),
                                                      k_default_max_bytes);
    REQUIRE_THROWS_WITH(make_op(std::move(argless), two_types(bigint_type())),
                        Catch::Contains("requires one"));
  }
  SECTION("declared output types are constrained per op")
  {
    auto avg_bigint = sirius::test::make_group_join_spec(groupjoin::join_form::INNER,
                                                         groupjoin::agg_op::AVG,
                                                         0,
                                                         0,
                                                         std::size_t{1},
                                                         bigint_type(),
                                                         k_default_max_bytes);
    REQUIRE_THROWS_WITH(make_op(std::move(avg_bigint), two_types(bigint_type())),
                        Catch::Contains("cannot produce declared output type"));
    auto sum_double = sirius::test::make_group_join_spec(groupjoin::join_form::INNER,
                                                         groupjoin::agg_op::SUM,
                                                         0,
                                                         0,
                                                         std::size_t{1},
                                                         double_type(),
                                                         k_default_max_bytes);
    REQUIRE_THROWS_WITH(make_op(std::move(sum_double), two_types(double_type())),
                        Catch::Contains("cannot produce declared output type"));
  }
  SECTION("the slot output type must match the declared output column")
  {
    auto spec = sirius::test::make_group_join_spec(groupjoin::join_form::INNER,
                                                   groupjoin::agg_op::SUM,
                                                   0,
                                                   0,
                                                   std::size_t{1},
                                                   bigint_type(),
                                                   k_default_max_bytes);
    REQUIRE_THROWS_WITH(
      make_op(std::move(spec), two_types(sirius::logical_type::make_decimal(18, 2))),
      Catch::Contains("does not match the declared output column type"));
  }
  SECTION("output schemas beyond [key, aggregate] are rejected")
  {
    auto spec  = sirius::test::make_group_join_spec(groupjoin::join_form::INNER,
                                                   groupjoin::agg_op::SUM,
                                                   0,
                                                   0,
                                                   std::size_t{1},
                                                   bigint_type(),
                                                   k_default_max_bytes);
    auto types = two_types(bigint_type());
    types.push_back(bigint_type());
    REQUIRE_THROWS_WITH(make_op(std::move(spec), std::move(types)),
                        Catch::Contains("expected [key, aggregate] output schema"));
  }
  SECTION("whitelisted INNER and DIRECT specs construct")
  {
    REQUIRE_NOTHROW(make_op(sirius::test::make_group_join_spec(groupjoin::join_form::INNER,
                                                               groupjoin::agg_op::AVG,
                                                               0,
                                                               0,
                                                               std::size_t{1},
                                                               double_type(),
                                                               k_default_max_bytes),
                            two_types(double_type())));
    REQUIRE_NOTHROW(make_op(sirius::test::make_group_join_spec(groupjoin::join_form::DIRECT,
                                                               groupjoin::agg_op::MIN,
                                                               0,
                                                               0,
                                                               std::size_t{1},
                                                               integer_type(),
                                                               k_default_max_bytes),
                            two_types(integer_type())));
  }
}

TEST_CASE("group_join value: DIRECT rejects preserved-side batches", "[group_join][validation]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  auto batch = make_kv_batch(*space, {1, 2}, {10, 20});
  duckdb::vector<sirius::logical_type> types;
  types.push_back(integer_type());
  types.push_back(bigint_type());
  sirius_physical_group_join op(std::move(types),
                                /*estimated_cardinality=*/2,
                                sirius::test::make_group_join_spec(groupjoin::join_form::DIRECT,
                                                                   groupjoin::agg_op::SUM,
                                                                   0,
                                                                   0,
                                                                   std::size_t{1},
                                                                   bigint_type(),
                                                                   k_default_max_bytes));
  group_join_input input({batch}, {batch});
  REQUIRE_THROWS_WITH(op.execute(input, default_stream()),
                      Catch::Contains("DIRECT form takes a single counted input"));
}
