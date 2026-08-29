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

#include "aggregate_test_utils.hpp"
#include "data/data_batch_utils.hpp"
#include "op/aggregate/gpu_aggregate_impl.hpp"
#include "op/aggregate/tiny_domain_grouped_aggregate_impl.hpp"
#include "op/sirius_physical_grouped_aggregate.hpp"
#include "operator/operator_test_utils.hpp"
#include "operator/operator_type_traits.hpp"
#include "utils/data_utils.hpp"

#include <cudf/aggregation.hpp>
#include <cudf/binaryop.hpp>
#include <cudf/column/column_factories.hpp>
#include <cudf/concatenate.hpp>
#include <cudf/copying.hpp>
#include <cudf/null_mask.hpp>
#include <cudf/reduction.hpp>
#include <cudf/scalar/scalar.hpp>
#include <cudf/sorting.hpp>
#include <cudf/table/table.hpp>
#include <cudf/utilities/default_stream.hpp>
#include <cudf/utilities/memory_resource.hpp>

#include <rmm/resource_ref.hpp>

#include <cuda_runtime_api.h>

#include <catch.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using sirius::op::try_tiny_domain_grouped_aggregate;
using sirius::test::vector_to_cudf_column;
using sirius::test::operator_utils::gpu_type_traits;
using sirius::test::operator_utils::string_tag;

template <typename T>
std::unique_ptr<cudf::column> make_fixed(cudf::data_type type,
                                         std::vector<T> const& values,
                                         std::vector<bool> const& validity,
                                         rmm::cuda_stream_view stream,
                                         rmm::device_async_resource_ref mr)
{
  REQUIRE((validity.empty() || validity.size() == values.size()));
  auto column = cudf::make_fixed_width_column(
    type, static_cast<cudf::size_type>(values.size()), cudf::mask_state::UNALLOCATED, stream, mr);
  if (!values.empty()) {
    REQUIRE(cudaMemcpyAsync(column->mutable_view().head<T>(),
                            values.data(),
                            values.size() * sizeof(T),
                            cudaMemcpyHostToDevice,
                            stream.value()) == cudaSuccess);
  }
  if (!validity.empty()) {
    auto mask = cudf::create_null_mask(column->size(), cudf::mask_state::ALL_VALID, stream, mr);
    cudf::size_type null_count{0};
    for (cudf::size_type row = 0; row < column->size(); ++row) {
      if (validity[static_cast<std::size_t>(row)]) { continue; }
      cudf::set_null_mask(
        static_cast<cudf::bitmask_type*>(mask.data()), row, row + 1, false, stream);
      ++null_count;
    }
    column->set_null_mask(std::move(mask), null_count);
  }
  return column;
}

template <typename T>
std::unique_ptr<cudf::column> make_fixed(cudf::data_type type,
                                         std::vector<T> const& values,
                                         rmm::cuda_stream_view stream,
                                         rmm::device_async_resource_ref mr)
{
  return make_fixed(type, values, {}, stream, mr);
}

std::unique_ptr<cudf::column> with_nulls(std::unique_ptr<cudf::column> column,
                                         std::vector<bool> const& validity,
                                         rmm::cuda_stream_view stream,
                                         rmm::device_async_resource_ref mr)
{
  REQUIRE(static_cast<std::size_t>(column->size()) == validity.size());
  auto mask = cudf::create_null_mask(column->size(), cudf::mask_state::ALL_VALID, stream, mr);
  cudf::size_type null_count{0};
  for (cudf::size_type row = 0; row < column->size(); ++row) {
    if (validity[static_cast<std::size_t>(row)]) { continue; }
    cudf::set_null_mask(static_cast<cudf::bitmask_type*>(mask.data()), row, row + 1, false, stream);
    ++null_count;
  }
  column->set_null_mask(std::move(mask), null_count);
  return column;
}

std::unique_ptr<cudf::column> make_strings(std::vector<std::string> const& values,
                                           rmm::cuda_stream_view stream,
                                           rmm::device_async_resource_ref mr)
{
  return vector_to_cudf_column<gpu_type_traits<string_tag>>(values, stream, mr);
}

std::unique_ptr<cudf::table> make_table(std::vector<std::unique_ptr<cudf::column>> columns)
{
  return std::make_unique<cudf::table>(std::move(columns));
}

void require_columns_equal(cudf::column_view const& actual,
                           cudf::column_view const& expected,
                           rmm::cuda_stream_view stream,
                           rmm::device_async_resource_ref mr)
{
  REQUIRE(actual.type() == expected.type());
  REQUIRE(actual.size() == expected.size());
  REQUIRE(actual.null_count() == expected.null_count());
  if (actual.is_empty()) { return; }

  auto equal           = cudf::binary_operation(actual,
                                      expected,
                                      cudf::binary_operator::NULL_EQUALS,
                                      cudf::data_type{cudf::type_id::BOOL8},
                                      stream,
                                      mr);
  auto all_aggregation = cudf::make_all_aggregation<cudf::reduce_aggregation>();
  auto all_equal       = cudf::reduce(
    equal->view(), *all_aggregation, cudf::data_type{cudf::type_id::BOOL8}, stream, mr);
  REQUIRE(static_cast<cudf::numeric_scalar<bool>&>(*all_equal).value(stream));
}

void require_tables_equal(cudf::table_view const& actual,
                          cudf::table_view const& expected,
                          rmm::cuda_stream_view stream,
                          rmm::device_async_resource_ref mr)
{
  REQUIRE(actual.num_columns() == expected.num_columns());
  REQUIRE(actual.num_rows() == expected.num_rows());
  for (cudf::size_type column = 0; column < actual.num_columns(); ++column) {
    require_columns_equal(actual.column(column), expected.column(column), stream, mr);
  }
}

template <typename T>
std::vector<T> copy_values(cudf::column_view column, rmm::cuda_stream_view stream)
{
  std::vector<T> host(static_cast<std::size_t>(column.size()));
  if (!host.empty()) {
    REQUIRE(cudaMemcpyAsync(host.data(),
                            column.data<T>(),
                            host.size() * sizeof(T),
                            cudaMemcpyDeviceToHost,
                            stream.value()) == cudaSuccess);
    stream.synchronize();
  }
  return host;
}

struct tiny_int8_traits {
  using type                = int8_t;
  using agg_output_type     = int64_t;
  using min_max_output_type = int8_t;
  static duckdb::LogicalType logical_type() { return duckdb::LogicalType::TINYINT; }
  static constexpr cudf::type_id cudf_type = cudf::type_id::INT8;
  static constexpr bool is_decimal         = false;
  static constexpr bool is_string          = false;
  static constexpr bool is_ts              = false;
};

}  // namespace

TEST_CASE("tiny-domain aggregate preserves sliced STRING offsets and NULL semantics",
          "[physical_grouped_aggregate][tiny_domain]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(with_nulls(make_strings({"P", "A", "A", "B", "B", "S"}, stream, mr),
                               {true, true, true, false, false, true},
                               stream,
                               mr));
  columns.push_back(make_fixed<int32_t>(cudf::data_type{cudf::type_id::INT32},
                                        {999, 10, 0, 0, 0, 999},
                                        {true, true, false, false, false, true},
                                        stream,
                                        mr));
  auto input       = make_table(std::move(columns));
  auto input_slice = cudf::slice(input->view(), {1, 5}, stream).front();
  REQUIRE(input_slice.column(0).offset() == 1);
  REQUIRE(input_slice.column(1).offset() == 1);

  auto attempt = try_tiny_domain_grouped_aggregate(input_slice,
                                                   {0},
                                                   {cudf::aggregation::Kind::SUM,
                                                    cudf::aggregation::Kind::COUNT_VALID,
                                                    cudf::aggregation::Kind::COUNT_ALL,
                                                    cudf::aggregation::Kind::MIN,
                                                    cudf::aggregation::Kind::MAX},
                                                   {1, 1, 0, 1, 1},
                                                   stream,
                                                   mr);
  REQUIRE(attempt);
  REQUIRE(attempt.num_groups == 2);

  std::vector<std::unique_ptr<cudf::column>> expected_columns;
  expected_columns.push_back(
    with_nulls(make_strings({"A", ""}, stream, mr), {true, false}, stream, mr));
  expected_columns.push_back(
    make_fixed<int64_t>(cudf::data_type{cudf::type_id::INT64}, {10, 0}, {true, false}, stream, mr));
  expected_columns.push_back(
    make_fixed<int32_t>(cudf::data_type{cudf::type_id::INT32}, {1, 0}, stream, mr));
  expected_columns.push_back(
    make_fixed<int64_t>(cudf::data_type{cudf::type_id::INT64}, {2, 2}, stream, mr));
  expected_columns.push_back(
    make_fixed<int32_t>(cudf::data_type{cudf::type_id::INT32}, {10, 0}, {true, false}, stream, mr));
  expected_columns.push_back(
    make_fixed<int32_t>(cudf::data_type{cudf::type_id::INT32}, {10, 0}, {true, false}, stream, mr));
  auto expected = make_table(std::move(expected_columns));
  require_tables_equal(attempt.table->view(), expected->view(), stream, mr);
}

TEST_CASE("tiny-domain aggregate preserves sliced INT8 and UINT8 offsets",
          "[physical_grouped_aggregate][tiny_domain]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(
    make_fixed<int8_t>(cudf::data_type{cudf::type_id::INT8}, {99, -1, -1, 1, 1, 99}, stream, mr));
  columns.push_back(
    make_fixed<uint8_t>(cudf::data_type{cudf::type_id::UINT8}, {99, 2, 2, 3, 3, 99}, stream, mr));
  columns.push_back(make_fixed<int32_t>(
    cudf::data_type{cudf::type_id::INT32}, {999, 4, 6, -2, 5, 999}, stream, mr));
  auto input       = make_table(std::move(columns));
  auto input_slice = cudf::slice(input->view(), {1, 5}, stream).front();
  for (cudf::size_type column = 0; column < input_slice.num_columns(); ++column) {
    REQUIRE(input_slice.column(column).offset() == 1);
  }

  auto attempt = try_tiny_domain_grouped_aggregate(
    input_slice,
    {0, 1},
    {cudf::aggregation::Kind::SUM, cudf::aggregation::Kind::MIN, cudf::aggregation::Kind::MAX},
    {2, 2, 2},
    stream,
    mr);
  REQUIRE(attempt);
  REQUIRE(attempt.num_groups == 2);

  std::vector<std::unique_ptr<cudf::column>> expected_columns;
  expected_columns.push_back(
    make_fixed<int8_t>(cudf::data_type{cudf::type_id::INT8}, {1, -1}, stream, mr));
  expected_columns.push_back(
    make_fixed<uint8_t>(cudf::data_type{cudf::type_id::UINT8}, {3, 2}, stream, mr));
  expected_columns.push_back(
    make_fixed<int64_t>(cudf::data_type{cudf::type_id::INT64}, {3, 10}, stream, mr));
  expected_columns.push_back(
    make_fixed<int32_t>(cudf::data_type{cudf::type_id::INT32}, {-2, 4}, stream, mr));
  expected_columns.push_back(
    make_fixed<int32_t>(cudf::data_type{cudf::type_id::INT32}, {5, 6}, stream, mr));
  auto expected = make_table(std::move(expected_columns));
  require_tables_equal(attempt.table->view(), expected->view(), stream, mr);
}

TEST_CASE("tiny-domain aggregate widens negative DECIMAL64 sums exactly",
          "[physical_grouped_aggregate][tiny_domain]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(
    make_fixed<int8_t>(cudf::data_type{cudf::type_id::INT8}, {1, 1, 2}, stream, mr));
  columns.push_back(make_fixed<int64_t>(
    cudf::data_type{cudf::type_id::DECIMAL64, -2}, {-100, 25, -50}, stream, mr));
  auto input = make_table(std::move(columns));

  auto attempt = try_tiny_domain_grouped_aggregate(
    input->view(),
    {0},
    {cudf::aggregation::Kind::SUM, cudf::aggregation::Kind::MIN, cudf::aggregation::Kind::MAX},
    {1, 1, 1},
    stream,
    mr);
  REQUIRE(attempt);
  REQUIRE(
    (attempt.table->view().column(1).type() == cudf::data_type{cudf::type_id::DECIMAL128, -2}));
  auto sums = copy_values<__int128_t>(attempt.table->view().column(1), stream);
  REQUIRE((sums == std::vector<__int128_t>{-75, -50}));
  auto minima = copy_values<int64_t>(attempt.table->view().column(2), stream);
  REQUIRE((minima == std::vector<int64_t>{-100, -50}));
  auto maxima = copy_values<int64_t>(attempt.table->view().column(3), stream);
  REQUIRE((maxima == std::vector<int64_t>{25, -50}));
}

TEST_CASE("tiny-domain aggregate is exact under one-group contention",
          "[physical_grouped_aggregate][tiny_domain][tiny_domain_contention]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  constexpr std::size_t rows = 1U << 18;
  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(make_fixed<int8_t>(
    cudf::data_type{cudf::type_id::INT8}, std::vector<int8_t>(rows, 7), stream, mr));
  columns.push_back(make_fixed<int32_t>(
    cudf::data_type{cudf::type_id::INT32}, std::vector<int32_t>(rows, 1), stream, mr));
  auto input = make_table(std::move(columns));

  auto attempt = try_tiny_domain_grouped_aggregate(input->view(),
                                                   {0},
                                                   {cudf::aggregation::Kind::SUM,
                                                    cudf::aggregation::Kind::COUNT_ALL,
                                                    cudf::aggregation::Kind::MIN,
                                                    cudf::aggregation::Kind::MAX},
                                                   {1, 1, 1, 1},
                                                   stream,
                                                   mr);
  REQUIRE(attempt);
  REQUIRE(attempt.num_groups == 1);
  auto view = attempt.table->view();
  REQUIRE(view.num_columns() == 5);
  REQUIRE((copy_values<int8_t>(view.column(0), stream) == std::vector<int8_t>{7}));
  REQUIRE((copy_values<int64_t>(view.column(1), stream) ==
           std::vector<int64_t>{static_cast<int64_t>(rows)}));
  REQUIRE((copy_values<int32_t>(view.column(2), stream) ==
           std::vector<int32_t>{static_cast<int32_t>(rows)}));
  REQUIRE((copy_values<int32_t>(view.column(3), stream) == std::vector<int32_t>{1}));
  REQUIRE((copy_values<int32_t>(view.column(4), stream) == std::vector<int32_t>{1}));
}

TEST_CASE("bounded-register aggregation deduplicates semantic states in arbitrary logical order",
          "[physical_grouped_aggregate][tiny_domain][tiny_domain_bounded_register]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(make_fixed<int8_t>(
    cudf::data_type{cudf::type_id::INT8}, {99, 3, 1, 2, 1, 3, 2, 99}, stream, mr));
  columns.push_back(make_fixed<int32_t>(
    cudf::data_type{cudf::type_id::INT32}, {999, 5, 1, 10, 2, -1, 4, 999}, stream, mr));
  auto parent = make_table(std::move(columns));
  auto input  = cudf::slice(parent->view(), {1, 7}, stream).front();
  REQUIRE(input.column(0).offset() == 1);
  REQUIRE(input.column(1).offset() == 1);

  auto attempt = try_tiny_domain_grouped_aggregate(input,
                                                   {0},
                                                   {cudf::aggregation::Kind::COUNT_ALL,
                                                    cudf::aggregation::Kind::SUM,
                                                    cudf::aggregation::Kind::SUM,
                                                    cudf::aggregation::Kind::COUNT_VALID},
                                                   {0, 1, 1, 1},
                                                   stream,
                                                   mr);
  REQUIRE(attempt);
  REQUIRE(attempt.num_groups == 3);
  REQUIRE(attempt.num_physical_register_states == 2);
  REQUIRE(attempt.bounded_register_attempted);
  REQUIRE(attempt.used_bounded_register);
  REQUIRE_FALSE(attempt.used_prefix_preflight);

  std::vector<std::unique_ptr<cudf::column>> expected_columns;
  expected_columns.push_back(
    make_fixed<int8_t>(cudf::data_type{cudf::type_id::INT8}, {1, 2, 3}, stream, mr));
  expected_columns.push_back(
    make_fixed<int64_t>(cudf::data_type{cudf::type_id::INT64}, {2, 2, 2}, stream, mr));
  expected_columns.push_back(
    make_fixed<int64_t>(cudf::data_type{cudf::type_id::INT64}, {3, 14, 4}, stream, mr));
  expected_columns.push_back(
    make_fixed<int64_t>(cudf::data_type{cudf::type_id::INT64}, {3, 14, 4}, stream, mr));
  expected_columns.push_back(
    make_fixed<int32_t>(cudf::data_type{cudf::type_id::INT32}, {2, 2, 2}, stream, mr));
  auto expected = make_table(std::move(expected_columns));
  require_tables_equal(attempt.table->view(), expected->view(), stream, mr);
}

TEST_CASE("bounded-register aggregation preserves nullable SUM and COUNT_VALID semantics",
          "[physical_grouped_aggregate][tiny_domain][tiny_domain_bounded_register]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(
    make_fixed<int8_t>(cudf::data_type{cudf::type_id::INT8}, {0, 0, 1, 1, 2, 2}, stream, mr));
  columns.push_back(make_fixed<int32_t>(cudf::data_type{cudf::type_id::INT32},
                                        {1, 99, 5, 7, 9, 10},
                                        {true, false, false, false, true, true},
                                        stream,
                                        mr));
  auto input = make_table(std::move(columns));

  auto attempt = try_tiny_domain_grouped_aggregate(input->view(),
                                                   {0},
                                                   {cudf::aggregation::Kind::SUM,
                                                    cudf::aggregation::Kind::COUNT_VALID,
                                                    cudf::aggregation::Kind::COUNT_VALID,
                                                    cudf::aggregation::Kind::COUNT_ALL},
                                                   {1, 1, 1, 0},
                                                   stream,
                                                   mr);
  REQUIRE(attempt);
  REQUIRE(attempt.num_physical_register_states == 3);
  REQUIRE(attempt.bounded_register_attempted);
  REQUIRE(attempt.used_bounded_register);

  std::vector<std::unique_ptr<cudf::column>> expected_columns;
  expected_columns.push_back(
    make_fixed<int8_t>(cudf::data_type{cudf::type_id::INT8}, {0, 1, 2}, stream, mr));
  expected_columns.push_back(make_fixed<int64_t>(
    cudf::data_type{cudf::type_id::INT64}, {1, 0, 19}, {true, false, true}, stream, mr));
  expected_columns.push_back(
    make_fixed<int32_t>(cudf::data_type{cudf::type_id::INT32}, {1, 0, 2}, stream, mr));
  expected_columns.push_back(
    make_fixed<int32_t>(cudf::data_type{cudf::type_id::INT32}, {1, 0, 2}, stream, mr));
  expected_columns.push_back(
    make_fixed<int64_t>(cudf::data_type{cudf::type_id::INT64}, {2, 2, 2}, stream, mr));
  auto expected = make_table(std::move(expected_columns));
  require_tables_equal(attempt.table->view(), expected->view(), stream, mr);
}

TEST_CASE("bounded-register resource cap uses source identity rather than equal values",
          "[physical_grouped_aggregate][tiny_domain][tiny_domain_bounded_register]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  std::vector<int8_t> keys{0, 1, 2, 3, 4, 0, 1, 2, 3, 4};
  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(make_fixed<int8_t>(cudf::data_type{cudf::type_id::INT8}, keys, stream, mr));
  for (int32_t multiplier = 1; multiplier <= 4; ++multiplier) {
    columns.push_back(make_fixed<int32_t>(cudf::data_type{cudf::type_id::INT32},
                                          std::vector<int32_t>(keys.size(), multiplier),
                                          stream,
                                          mr));
  }
  columns.push_back(make_fixed<int32_t>(
    cudf::data_type{cudf::type_id::INT32}, std::vector<int32_t>(keys.size(), 1), stream, mr));
  auto input = make_table(std::move(columns));
  std::vector<cudf::aggregation::Kind> const kinds(5, cudf::aggregation::Kind::SUM);

  auto aliased =
    try_tiny_domain_grouped_aggregate(input->view(), {0}, kinds, {1, 2, 3, 4, 1}, stream, mr);
  REQUIRE(aliased);
  REQUIRE(aliased.num_groups == 5);
  REQUIRE(aliased.used_bounded_register == aliased.bounded_register_attempted);
  REQUIRE(aliased.num_physical_register_states ==
          static_cast<std::size_t>(aliased.bounded_register_attempted ? 4 : 0));

  auto cloned =
    try_tiny_domain_grouped_aggregate(input->view(), {0}, kinds, {1, 2, 3, 4, 5}, stream, mr);
  REQUIRE(cloned);
  REQUIRE_FALSE(cloned.bounded_register_attempted);
  REQUIRE_FALSE(cloned.used_bounded_register);
  require_tables_equal(cloned.table->view(), aliased.table->view(), stream, mr);
}

TEST_CASE("bounded-register capacity dispatch covers generic group and state boundaries",
          "[physical_grouped_aggregate][tiny_domain][tiny_domain_bounded_register]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  struct capacity_case {
    int num_groups;
    int num_states;
    bool expected_register;
  };
  std::vector<capacity_case> const cases{{1, 7, true},
                                         {2, 7, true},
                                         {3, 7, true},
                                         {4, 8, true},
                                         {5, 4, true},
                                         {5, 5, false},
                                         {7, 3, true},
                                         {7, 5, false},
                                         {7, 7, false},
                                         {8, 4, true},
                                         {16, 2, true},
                                         {16, 3, false},
                                         {32, 1, true},
                                         {32, 2, false}};

  for (auto const& test : cases) {
    CAPTURE(test.num_groups, test.num_states, test.expected_register);
    std::vector<int8_t> keys(static_cast<std::size_t>(test.num_groups));
    for (int group = 0; group < test.num_groups; ++group) {
      keys[static_cast<std::size_t>(group)] = static_cast<int8_t>(group);
    }
    std::vector<std::unique_ptr<cudf::column>> columns;
    columns.push_back(make_fixed<int8_t>(cudf::data_type{cudf::type_id::INT8}, keys, stream, mr));
    std::vector<cudf::aggregation::Kind> kinds(static_cast<std::size_t>(test.num_states),
                                               cudf::aggregation::Kind::SUM);
    std::vector<int> indices;
    indices.reserve(static_cast<std::size_t>(test.num_states));
    for (int state = 0; state < test.num_states; ++state) {
      columns.push_back(make_fixed<int32_t>(
        cudf::data_type{cudf::type_id::INT32},
        std::vector<int32_t>(static_cast<std::size_t>(test.num_groups), state + 1),
        stream,
        mr));
      indices.push_back(state + 1);
    }
    auto input = make_table(std::move(columns));
    auto attempt =
      try_tiny_domain_grouped_aggregate(input->view(), {0}, kinds, indices, stream, mr);
    REQUIRE(attempt);
    REQUIRE(attempt.num_groups == static_cast<std::size_t>(test.num_groups));
    REQUIRE(attempt.used_bounded_register == attempt.bounded_register_attempted);
    if (test.expected_register) {
      REQUIRE(attempt.num_physical_register_states ==
              static_cast<std::size_t>(attempt.bounded_register_attempted ? test.num_states : 0));
    } else {
      REQUIRE_FALSE(attempt.bounded_register_attempted);
      REQUIRE(attempt.num_physical_register_states == 0);
    }
    for (int state = 0; state < test.num_states; ++state) {
      REQUIRE(copy_values<int64_t>(attempt.table->view().column(state + 1), stream) ==
              std::vector<int64_t>(static_cast<std::size_t>(test.num_groups), state + 1));
    }
  }
}

TEST_CASE(
  "bounded-register prefix preflight activates within generic capacity",
  "[physical_grouped_aggregate][tiny_domain][tiny_domain_bounded_register][tiny_domain_prefix]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  constexpr std::size_t rows = 1U << 17;
  std::vector<int8_t> keys(rows);
  for (std::size_t row = 0; row < rows; ++row) {
    keys[row] = static_cast<int8_t>(row % 3);
  }
  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(make_fixed<int8_t>(cudf::data_type{cudf::type_id::INT8}, keys, stream, mr));
  for (int64_t multiplier = 1; multiplier <= 5; ++multiplier) {
    columns.push_back(make_fixed<int64_t>(cudf::data_type{cudf::type_id::DECIMAL64, -2},
                                          std::vector<int64_t>(rows, multiplier),
                                          stream,
                                          mr));
  }
  auto input = make_table(std::move(columns));

  auto attempt = try_tiny_domain_grouped_aggregate(input->view(),
                                                   {0},
                                                   {cudf::aggregation::Kind::COUNT_ALL,
                                                    cudf::aggregation::Kind::SUM,
                                                    cudf::aggregation::Kind::SUM,
                                                    cudf::aggregation::Kind::SUM,
                                                    cudf::aggregation::Kind::SUM,
                                                    cudf::aggregation::Kind::SUM},
                                                   {0, 3, 1, 5, 2, 4},
                                                   stream,
                                                   mr);
  REQUIRE(attempt);
  REQUIRE(attempt.num_groups == 3);
  REQUIRE(attempt.num_physical_register_states == 6);
  REQUIRE(attempt.bounded_register_attempted);
  REQUIRE(attempt.used_bounded_register);
  REQUIRE(attempt.used_prefix_preflight);

  auto const base_count = static_cast<int64_t>(rows / 3);
  REQUIRE(copy_values<int64_t>(attempt.table->view().column(1), stream) ==
          std::vector<int64_t>{base_count + 1, base_count + 1, base_count});
  REQUIRE(copy_values<__int128_t>(attempt.table->view().column(2), stream) ==
          std::vector<__int128_t>{static_cast<__int128_t>(base_count + 1) * 3,
                                  static_cast<__int128_t>(base_count + 1) * 3,
                                  static_cast<__int128_t>(base_count) * 3});
}

TEST_CASE(
  "bounded-register prefix preflight fails closed on an unseen late key",
  "[physical_grouped_aggregate][tiny_domain][tiny_domain_bounded_register][tiny_domain_prefix]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  constexpr std::size_t prefix_rows = 1U << 16;
  auto const rows                   = prefix_rows + 1;
  std::vector<int8_t> keys(rows);
  for (std::size_t row = 0; row < prefix_rows; ++row) {
    keys[row] = static_cast<int8_t>(row % 2);
  }
  keys.back() = 2;
  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(make_fixed<int8_t>(cudf::data_type{cudf::type_id::INT8}, keys, stream, mr));
  columns.push_back(make_fixed<int32_t>(
    cudf::data_type{cudf::type_id::INT32}, std::vector<int32_t>(rows, 1), stream, mr));
  auto input = make_table(std::move(columns));

  auto attempt = try_tiny_domain_grouped_aggregate(
    input->view(), {0}, {cudf::aggregation::Kind::SUM}, {1}, stream, mr);
  REQUIRE_FALSE(attempt);
  REQUIRE(attempt.fallback_reason.find("missed a grouping key") != std::string::npos);
}

TEST_CASE("bounded-register overflow retries the exact-wide strategy before publication",
          "[physical_grouped_aggregate][tiny_domain][tiny_domain_bounded_register]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  constexpr std::size_t rows = 1U << 18;
  auto const maximum         = std::numeric_limits<int64_t>::max();
  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(make_fixed<int8_t>(
    cudf::data_type{cudf::type_id::INT8}, std::vector<int8_t>(rows, 9), stream, mr));
  columns.push_back(make_fixed<int64_t>(cudf::data_type{cudf::type_id::DECIMAL64, -3},
                                        std::vector<int64_t>(rows, maximum),
                                        stream,
                                        mr));
  auto input = make_table(std::move(columns));

  auto attempt = try_tiny_domain_grouped_aggregate(
    input->view(),
    {0},
    {cudf::aggregation::Kind::SUM, cudf::aggregation::Kind::COUNT_ALL},
    {1, 0},
    stream,
    mr);
  REQUIRE(attempt);
  REQUIRE(attempt.bounded_register_attempted);
  REQUIRE_FALSE(attempt.used_bounded_register);
  REQUIRE(attempt.used_prefix_preflight);
  REQUIRE(copy_values<__int128_t>(attempt.table->view().column(1), stream) ==
          std::vector<__int128_t>{static_cast<__int128_t>(maximum) * rows});
  REQUIRE(copy_values<int64_t>(attempt.table->view().column(2), stream) ==
          std::vector<int64_t>{static_cast<int64_t>(rows)});
}

TEST_CASE("bounded-register overflow retry preserves exact mixed-sign cancellation",
          "[physical_grouped_aggregate][tiny_domain][tiny_domain_bounded_register]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  constexpr std::size_t rows = 1U << 20;
  std::vector<int64_t> values(rows);
  for (std::size_t row = 0; row < rows; ++row) {
    if (row < rows / 4) {
      values[row] = std::numeric_limits<int64_t>::min();
    } else if (row < rows / 2) {
      values[row] = -1;
    } else if (row < 3 * rows / 4) {
      values[row] = std::numeric_limits<int64_t>::max();
    } else {
      values[row] = 2;
    }
  }
  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(make_fixed<int8_t>(
    cudf::data_type{cudf::type_id::INT8}, std::vector<int8_t>(rows, 4), stream, mr));
  columns.push_back(
    make_fixed<int64_t>(cudf::data_type{cudf::type_id::DECIMAL64, -4}, values, stream, mr));
  auto input = make_table(std::move(columns));

  auto attempt = try_tiny_domain_grouped_aggregate(
    input->view(), {0}, {cudf::aggregation::Kind::SUM}, {1}, stream, mr);
  REQUIRE(attempt);
  REQUIRE(attempt.bounded_register_attempted);
  REQUIRE_FALSE(attempt.used_bounded_register);
  REQUIRE(attempt.used_prefix_preflight);
  REQUIRE(copy_values<__int128_t>(attempt.table->view().column(1), stream) ==
          std::vector<__int128_t>{0});
}

TEST_CASE(
  "prefix preflight prioritizes a late invalid STRING key over register overflow",
  "[physical_grouped_aggregate][tiny_domain][tiny_domain_bounded_register][tiny_domain_prefix]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  constexpr std::size_t rows = 1U << 18;
  std::vector<std::string> keys(rows, "A");
  keys.back()        = "AA";
  auto const maximum = std::numeric_limits<int64_t>::max();
  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(make_strings(keys, stream, mr));
  columns.push_back(make_fixed<int64_t>(cudf::data_type{cudf::type_id::DECIMAL64, -1},
                                        std::vector<int64_t>(rows, maximum),
                                        stream,
                                        mr));
  auto input = make_table(std::move(columns));

  auto attempt = try_tiny_domain_grouped_aggregate(
    input->view(), {0}, {cudf::aggregation::Kind::SUM}, {1}, stream, mr);
  REQUIRE_FALSE(attempt);
  REQUIRE(attempt.table == nullptr);
  REQUIRE(attempt.fallback_reason.find("exactly one byte") != std::string::npos);
}

TEST_CASE("bounded-register aggregation supports sliced two-STRING-key inputs",
          "[physical_grouped_aggregate][tiny_domain][tiny_domain_bounded_register]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(make_strings({"X", "A", "A", "B", "B", "C", "Z"}, stream, mr));
  columns.push_back(make_strings({"X", "x", "y", "x", "y", "x", "Z"}, stream, mr));
  columns.push_back(make_fixed<int64_t>(
    cudf::data_type{cudf::type_id::DECIMAL64, -2}, {999, 1, 2, 3, 4, 5, 999}, stream, mr));
  auto parent = make_table(std::move(columns));
  auto input  = cudf::slice(parent->view(), {1, 6}, stream).front();
  REQUIRE(input.column(0).offset() == 1);
  REQUIRE(input.column(1).offset() == 1);
  REQUIRE(input.column(2).offset() == 1);

  auto attempt = try_tiny_domain_grouped_aggregate(
    input, {0, 1}, {cudf::aggregation::Kind::SUM}, {2}, stream, mr);
  REQUIRE(attempt);
  REQUIRE(attempt.num_groups == 5);
  REQUIRE(attempt.bounded_register_attempted);
  REQUIRE(attempt.used_bounded_register);
  std::vector<std::unique_ptr<cudf::column>> expected_columns;
  expected_columns.push_back(make_strings({"A", "A", "B", "B", "C"}, stream, mr));
  expected_columns.push_back(make_strings({"x", "y", "x", "y", "x"}, stream, mr));
  expected_columns.push_back(make_fixed<__int128_t>(
    cudf::data_type{cudf::type_id::DECIMAL128, -2}, {1, 2, 3, 4, 5}, stream, mr));
  auto expected = make_table(std::move(expected_columns));
  require_tables_equal(attempt.table->view(), expected->view(), stream, mr);
}

TEST_CASE("tiny-domain aggregate fails closed for overflow and out-of-domain inputs",
          "[physical_grouped_aggregate][tiny_domain]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  SECTION("SUM carrier excluded by the row-bound proof")
  {
    std::vector<std::unique_ptr<cudf::column>> columns;
    columns.push_back(make_fixed<int8_t>(cudf::data_type{cudf::type_id::INT8}, {1, 1}, stream, mr));
    columns.push_back(make_fixed<uint64_t>(cudf::data_type{cudf::type_id::UINT64},
                                           {std::numeric_limits<uint64_t>::max(), 1},
                                           stream,
                                           mr));
    auto input   = make_table(std::move(columns));
    auto attempt = try_tiny_domain_grouped_aggregate(
      input->view(), {0}, {cudf::aggregation::Kind::SUM}, {1}, stream, mr);
    REQUIRE_FALSE(attempt);
    REQUIRE(attempt.fallback_reason.find("not overflow-safe") != std::string::npos);
  }

  SECTION("multi-byte STRING")
  {
    std::vector<std::unique_ptr<cudf::column>> columns;
    columns.push_back(make_strings({"AA", "AA"}, stream, mr));
    columns.push_back(
      make_fixed<int32_t>(cudf::data_type{cudf::type_id::INT32}, {1, 2}, stream, mr));
    auto input   = make_table(std::move(columns));
    auto attempt = try_tiny_domain_grouped_aggregate(
      input->view(), {0}, {cudf::aggregation::Kind::SUM}, {1}, stream, mr);
    REQUIRE_FALSE(attempt);
    REQUIRE(attempt.fallback_reason.find("exactly one byte") != std::string::npos);
  }

  SECTION("more than 64 groups")
  {
    std::vector<int8_t> keys(65);
    std::vector<int32_t> values(65, 1);
    for (std::size_t i = 0; i < keys.size(); ++i) {
      keys[i] = static_cast<int8_t>(i);
    }
    std::vector<std::unique_ptr<cudf::column>> columns;
    columns.push_back(make_fixed<int8_t>(cudf::data_type{cudf::type_id::INT8}, keys, stream, mr));
    columns.push_back(
      make_fixed<int32_t>(cudf::data_type{cudf::type_id::INT32}, values, stream, mr));
    auto input   = make_table(std::move(columns));
    auto attempt = try_tiny_domain_grouped_aggregate(
      input->view(), {0}, {cudf::aggregation::Kind::SUM}, {1}, stream, mr);
    REQUIRE_FALSE(attempt);
    REQUIRE(attempt.fallback_reason.find("exceeds 64 groups") != std::string::npos);
  }
}

TEST_CASE("physical grouped aggregate records tiny-domain AVG-state activation",
          "[physical_grouped_aggregate][tiny_domain]")
{
  using namespace sirius::test::operator_utils;

  auto memory_manager = sirius::test::operator_utils::initialize_memory_manager();
  auto* space         = memory_manager->get_memory_space(cucascade::memory::Tier::GPU, 0);
  REQUIRE(space != nullptr);
  auto mr     = get_resource_ref(*space);
  auto stream = default_stream();

  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(
    make_fixed<int8_t>(cudf::data_type{cudf::type_id::INT8}, {1, 1, 2, 2}, stream, mr));
  columns.push_back(
    make_fixed<int8_t>(cudf::data_type{cudf::type_id::INT8}, {2, 4, -1, 5}, stream, mr));
  auto input_table                                   = make_table(std::move(columns));
  std::shared_ptr<cucascade::data_batch> input_batch = sirius::make_data_batch(
    std::move(input_table), *space, stream, sirius::telemetry::batch_telemetry_info{});

  auto expressions =
    sirius::test::create_aggregate_expressions<tiny_int8_traits>({0}, {"avg"}, {1});
  sirius::op::sirius_physical_grouped_aggregate aggregate(std::move(expressions.output_types),
                                                          std::move(expressions.aggregates),
                                                          std::move(expressions.groups),
                                                          2,
                                                          true);
  REQUIRE(aggregate.tiny_domain_strategy_enabled());
  REQUIRE(aggregate.tiny_domain_activation_count() == 0);
  REQUIRE(aggregate.tiny_domain_fallback_count() == 0);

  std::vector<int8_t> fallback_keys(65);
  std::vector<int8_t> fallback_values(65, 1);
  for (std::size_t row = 0; row < fallback_keys.size(); ++row) {
    fallback_keys[row] = static_cast<int8_t>(row);
  }
  std::vector<std::unique_ptr<cudf::column>> fallback_columns;
  fallback_columns.push_back(
    make_fixed<int8_t>(cudf::data_type{cudf::type_id::INT8}, fallback_keys, stream, mr));
  fallback_columns.push_back(
    make_fixed<int8_t>(cudf::data_type{cudf::type_id::INT8}, fallback_values, stream, mr));
  auto fallback_table                                   = make_table(std::move(fallback_columns));
  std::shared_ptr<cucascade::data_batch> fallback_batch = sirius::make_data_batch(
    std::move(fallback_table), *space, stream, sirius::telemetry::batch_telemetry_info{});

  auto output = aggregate.execute(
    sirius::op::pipelineable_operator_data(
      std::vector<std::shared_ptr<cucascade::data_batch>>{input_batch, fallback_batch}),
    stream);
  REQUIRE(aggregate.tiny_domain_activation_count() == 1);
  REQUIRE(aggregate.tiny_domain_fallback_count() == 1);
  auto const& batches =
    dynamic_cast<sirius::op::pipelineable_operator_data const&>(*output).get_data_batches();
  REQUIRE(batches.size() == 2);

  auto fast_view     = sirius::get_cudf_table_view(*batches[0]);
  auto fallback_view = sirius::get_cudf_table_view(*batches[1]);
  REQUIRE(fast_view.num_columns() == 3);  // key plus AVG's SUM and COUNT_VALID states
  REQUIRE(fast_view.num_rows() == 2);
  REQUIRE(fallback_view.num_rows() == 65);
  REQUIRE(fast_view.column(1).type().id() == cudf::type_id::INT64);
  REQUIRE(fallback_view.column(1).type().id() == cudf::type_id::INT64);
  REQUIRE(fast_view.column(2).type().id() == cudf::type_id::INT32);
  REQUIRE(fallback_view.column(2).type().id() == cudf::type_id::INT32);
  REQUIRE((copy_values<int64_t>(fast_view.column(1), stream) == std::vector<int64_t>{6, 4}));
  REQUIRE((copy_values<int32_t>(fast_view.column(2), stream) == std::vector<int32_t>{2, 2}));

  std::vector<cudf::table_view> views{fast_view, fallback_view};
  auto concatenated = cudf::concatenate(views, stream, mr);
  REQUIRE(concatenated->num_columns() == 3);
  REQUIRE(concatenated->num_rows() == 67);
}
