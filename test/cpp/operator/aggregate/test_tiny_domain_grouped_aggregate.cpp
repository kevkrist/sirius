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

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using sirius::op::try_tiny_domain_grouped_aggregate;
using sirius::op::try_tiny_domain_q1_projection_aggregate;
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

std::vector<cudf::aggregation::Kind> q1_aggregate_kinds()
{
  return {cudf::aggregation::Kind::SUM,
          cudf::aggregation::Kind::SUM,
          cudf::aggregation::Kind::SUM,
          cudf::aggregation::Kind::SUM,
          cudf::aggregation::Kind::SUM,
          cudf::aggregation::Kind::COUNT_VALID,
          cudf::aggregation::Kind::SUM,
          cudf::aggregation::Kind::COUNT_VALID,
          cudf::aggregation::Kind::SUM,
          cudf::aggregation::Kind::COUNT_VALID,
          cudf::aggregation::Kind::COUNT_ALL};
}

std::vector<int> q1_aggregate_indices(bool cloned_average_sources = false)
{
  if (cloned_average_sources) { return {2, 3, 4, 5, 7, 7, 8, 8, 6, 6, 0}; }
  return {2, 3, 4, 5, 2, 2, 3, 3, 6, 6, 0};
}

std::vector<int> q1_projection_aggregate_indices() { return {2, 3, 4, 5, 6, 6, 7, 7, 8, 8, 0}; }

sirius::op::tiny_domain_q1_projection_plan q1_projection_plan()
{
  using sirius::op::tiny_domain_q1_value_source;

  auto const decimal_15_2 = sirius::logical_type::make_decimal(15, 2);
  return {{0, 1},
          {2, 3, 4, 5},
          {decimal_15_2, decimal_15_2, decimal_15_2, decimal_15_2},
          {decimal_15_2,
           decimal_15_2,
           sirius::logical_type::make_decimal(18, 4),
           sirius::logical_type::make_decimal(18, 6),
           decimal_15_2},
          {tiny_domain_q1_value_source::independent_sum,
           tiny_domain_q1_value_source::price,
           tiny_domain_q1_value_source::discounted_price,
           tiny_domain_q1_value_source::charge,
           tiny_domain_q1_value_source::discount},
          {0, 1, 2, 3, 0, 5, 1, 5, 4, 5, 5}};
}

std::unique_ptr<cudf::table> make_q1_raw_input(
  std::vector<std::string> const& return_flags,
  std::vector<std::string> const& line_statuses,
  std::array<std::vector<int64_t>, 4> const& decimal_values,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr,
  int32_t price_scale = -2)
{
  REQUIRE(return_flags.size() == line_statuses.size());
  for (auto const& values : decimal_values) {
    REQUIRE(values.size() == return_flags.size());
  }

  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.reserve(6);
  columns.push_back(make_strings(return_flags, stream, mr));
  columns.push_back(make_strings(line_statuses, stream, mr));
  columns.push_back(make_fixed<int64_t>(
    cudf::data_type{cudf::type_id::DECIMAL64, -2}, decimal_values[0], stream, mr));
  columns.push_back(make_fixed<int64_t>(
    cudf::data_type{cudf::type_id::DECIMAL64, price_scale}, decimal_values[1], stream, mr));
  columns.push_back(make_fixed<int64_t>(
    cudf::data_type{cudf::type_id::DECIMAL64, -2}, decimal_values[2], stream, mr));
  columns.push_back(make_fixed<int64_t>(
    cudf::data_type{cudf::type_id::DECIMAL64, -2}, decimal_values[3], stream, mr));
  return make_table(std::move(columns));
}

std::unique_ptr<cudf::table> make_q1_projected_input(
  std::vector<std::string> const& return_flags,
  std::vector<std::string> const& line_statuses,
  std::array<std::vector<int64_t>, 4> const& decimal_values,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr)
{
  REQUIRE(return_flags.size() == line_statuses.size());
  for (auto const& values : decimal_values) {
    REQUIRE(values.size() == return_flags.size());
  }

  std::vector<int64_t> discounted_price(return_flags.size());
  std::vector<int64_t> charge(return_flags.size());
  for (std::size_t row = 0; row < return_flags.size(); ++row) {
    auto const discounted =
      static_cast<__int128_t>(decimal_values[1][row]) * (int64_t{100} - decimal_values[2][row]);
    auto const charged = discounted * (int64_t{100} + decimal_values[3][row]);
    REQUIRE(discounted >= std::numeric_limits<int64_t>::min());
    REQUIRE(discounted <= std::numeric_limits<int64_t>::max());
    REQUIRE(charged >= std::numeric_limits<int64_t>::min());
    REQUIRE(charged <= std::numeric_limits<int64_t>::max());
    discounted_price[row] = static_cast<int64_t>(discounted);
    charge[row]           = static_cast<int64_t>(charged);
  }

  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.reserve(9);
  columns.push_back(make_strings(return_flags, stream, mr));
  columns.push_back(make_strings(line_statuses, stream, mr));
  columns.push_back(make_fixed<int64_t>(
    cudf::data_type{cudf::type_id::DECIMAL64, -2}, decimal_values[0], stream, mr));
  columns.push_back(make_fixed<int64_t>(
    cudf::data_type{cudf::type_id::DECIMAL64, -2}, decimal_values[1], stream, mr));
  columns.push_back(make_fixed<int64_t>(
    cudf::data_type{cudf::type_id::DECIMAL64, -4}, discounted_price, stream, mr));
  columns.push_back(
    make_fixed<int64_t>(cudf::data_type{cudf::type_id::DECIMAL64, -6}, charge, stream, mr));
  columns.push_back(make_fixed<int64_t>(
    cudf::data_type{cudf::type_id::DECIMAL64, -2}, decimal_values[0], stream, mr));
  columns.push_back(make_fixed<int64_t>(
    cudf::data_type{cudf::type_id::DECIMAL64, -2}, decimal_values[1], stream, mr));
  columns.push_back(make_fixed<int64_t>(
    cudf::data_type{cudf::type_id::DECIMAL64, -2}, decimal_values[2], stream, mr));
  return make_table(std::move(columns));
}

constexpr std::size_t q1_preflight_sample_rows = 1U << 16;

std::unique_ptr<cudf::table> make_q1_like_input(
  std::vector<std::string> const& return_flags,
  std::vector<std::string> const& line_statuses,
  std::array<std::vector<int64_t>, 5> const& decimal_values,
  bool clone_average_sources,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr)
{
  REQUIRE(return_flags.size() == line_statuses.size());
  for (auto const& values : decimal_values) {
    REQUIRE(values.size() == return_flags.size());
  }

  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.reserve(clone_average_sources ? 9 : 7);
  columns.push_back(make_strings(return_flags, stream, mr));
  columns.push_back(make_strings(line_statuses, stream, mr));
  for (auto const& values : decimal_values) {
    columns.push_back(
      make_fixed<int64_t>(cudf::data_type{cudf::type_id::DECIMAL64, -2}, values, stream, mr));
  }
  if (clone_average_sources) {
    columns.push_back(make_fixed<int64_t>(
      cudf::data_type{cudf::type_id::DECIMAL64, -2}, decimal_values[0], stream, mr));
    columns.push_back(make_fixed<int64_t>(
      cudf::data_type{cudf::type_id::DECIMAL64, -2}, decimal_values[1], stream, mr));
  }
  return make_table(std::move(columns));
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

TEST_CASE("tiny-domain aggregate matches the Q1 state schema and structural fallback",
          "[physical_grouped_aggregate][tiny_domain_register]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  std::vector<std::string> const return_flags{"N", "A", "N", "A", "A", "N", "A", "N"};
  std::vector<std::string> const line_statuses{"O", "F", "F", "O", "F", "O", "O", "F"};
  std::array<std::vector<int64_t>, 5> const values{
    std::vector<int64_t>{1, 2, 3, 4, 5, 6, 7, 8},
    std::vector<int64_t>{10, 20, 30, 40, 50, 60, 70, 80},
    std::vector<int64_t>{100, 200, 300, 400, 500, 600, 700, 800},
    std::vector<int64_t>{1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000},
    std::vector<int64_t>{2, 3, 4, 5, 6, 7, 8, 9}};

  auto input = make_q1_like_input(return_flags, line_statuses, values, false, stream, mr);
  auto fast  = try_tiny_domain_grouped_aggregate(
    input->view(), {0, 1}, q1_aggregate_kinds(), q1_aggregate_indices(), stream, mr);
  REQUIRE(fast);
  REQUIRE(fast.num_groups == 4);
  REQUIRE(fast.register_private_attempted);
  REQUIRE(fast.used_register_private);
  REQUIRE_FALSE(fast.used_sampled_preflight);

  auto const view = fast.table->view();
  REQUIRE(view.num_columns() == 13);
  auto expected_return_flags  = make_strings({"A", "A", "N", "N"}, stream, mr);
  auto expected_line_statuses = make_strings({"F", "O", "F", "O"}, stream, mr);
  require_columns_equal(view.column(0), expected_return_flags->view(), stream, mr);
  require_columns_equal(view.column(1), expected_line_statuses->view(), stream, mr);

  auto require_sum = [&](cudf::size_type column, std::vector<__int128_t> const& expected) {
    REQUIRE((view.column(column).type() == cudf::data_type{cudf::type_id::DECIMAL128, -2}));
    REQUIRE(copy_values<__int128_t>(view.column(column), stream) == expected);
  };
  require_sum(2, {7, 11, 11, 7});
  require_sum(3, {70, 110, 110, 70});
  require_sum(4, {700, 1100, 1100, 700});
  require_sum(5, {7000, 11000, 11000, 7000});
  require_sum(6, {7, 11, 11, 7});
  require_sum(8, {70, 110, 110, 70});
  require_sum(10, {9, 13, 13, 9});
  REQUIRE(view.column(7).type().id() == cudf::type_id::INT32);
  REQUIRE(view.column(9).type().id() == cudf::type_id::INT32);
  REQUIRE(view.column(11).type().id() == cudf::type_id::INT32);
  REQUIRE(view.column(12).type().id() == cudf::type_id::INT64);
  REQUIRE((copy_values<int32_t>(view.column(7), stream) == std::vector<int32_t>{2, 2, 2, 2}));
  REQUIRE((copy_values<int32_t>(view.column(9), stream) == std::vector<int32_t>{2, 2, 2, 2}));
  REQUIRE((copy_values<int32_t>(view.column(11), stream) == std::vector<int32_t>{2, 2, 2, 2}));
  REQUIRE((copy_values<int64_t>(view.column(12), stream) == std::vector<int64_t>{2, 2, 2, 2}));

  // Equal-valued clones are deliberately different input sources. This must preserve the old,
  // generic tiny-domain implementation instead of treating equality of values as state identity.
  auto fallback_input = make_q1_like_input(return_flags, line_statuses, values, true, stream, mr);
  auto structural_fallback = try_tiny_domain_grouped_aggregate(
    fallback_input->view(), {0, 1}, q1_aggregate_kinds(), q1_aggregate_indices(true), stream, mr);
  REQUIRE(structural_fallback);
  REQUIRE_FALSE(structural_fallback.register_private_attempted);
  REQUIRE_FALSE(structural_fallback.used_register_private);
  REQUIRE_FALSE(structural_fallback.used_sampled_preflight);
  require_tables_equal(structural_fallback.table->view(), view, stream, mr);
}

TEST_CASE("Q1 register-private aggregate preserves sliced aggregate offsets",
          "[physical_grouped_aggregate][tiny_domain_register][tiny_domain_register_slice]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  std::vector<std::string> const parent_return_flags{
    "X", "N", "A", "N", "A", "A", "N", "A", "N", "Z"};
  std::vector<std::string> const parent_line_statuses{
    "X", "O", "F", "F", "O", "F", "O", "O", "F", "Z"};
  std::array<std::vector<int64_t>, 5> const parent_values{
    std::vector<int64_t>{999, 1, 2, 3, 4, 5, 6, 7, 8, 999},
    std::vector<int64_t>{999, 10, 20, 30, 40, 50, 60, 70, 80, 999},
    std::vector<int64_t>{999, 100, 200, 300, 400, 500, 600, 700, 800, 999},
    std::vector<int64_t>{999, 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 999},
    std::vector<int64_t>{999, 2, 3, 4, 5, 6, 7, 8, 9, 999}};
  std::vector<std::string> const expected_return_flags{"N", "A", "N", "A", "A", "N", "A", "N"};
  std::vector<std::string> const expected_line_statuses{"O", "F", "F", "O", "F", "O", "O", "F"};
  std::array<std::vector<int64_t>, 5> const expected_values{
    std::vector<int64_t>{1, 2, 3, 4, 5, 6, 7, 8},
    std::vector<int64_t>{10, 20, 30, 40, 50, 60, 70, 80},
    std::vector<int64_t>{100, 200, 300, 400, 500, 600, 700, 800},
    std::vector<int64_t>{1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000},
    std::vector<int64_t>{2, 3, 4, 5, 6, 7, 8, 9}};

  auto parent =
    make_q1_like_input(parent_return_flags, parent_line_statuses, parent_values, false, stream, mr);
  auto sliced = cudf::slice(parent->view(), {1, 9}, stream).front();
  REQUIRE(sliced.column(0).offset() == 1);
  REQUIRE(sliced.column(2).offset() == 1);
  auto expected = make_q1_like_input(
    expected_return_flags, expected_line_statuses, expected_values, false, stream, mr);

  auto sliced_attempt = try_tiny_domain_grouped_aggregate(
    sliced, {0, 1}, q1_aggregate_kinds(), q1_aggregate_indices(), stream, mr);
  auto expected_attempt = try_tiny_domain_grouped_aggregate(
    expected->view(), {0, 1}, q1_aggregate_kinds(), q1_aggregate_indices(), stream, mr);
  REQUIRE(sliced_attempt);
  REQUIRE(expected_attempt);
  REQUIRE(sliced_attempt.used_register_private);
  REQUIRE(expected_attempt.used_register_private);
  REQUIRE_FALSE(sliced_attempt.used_sampled_preflight);
  require_tables_equal(sliced_attempt.table->view(), expected_attempt.table->view(), stream, mr);
}

TEST_CASE("Q1 register-private shape rejects nullable aggregate input",
          "[physical_grouped_aggregate][tiny_domain_register][tiny_domain_register_nullable]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  std::vector<std::string> const return_flags{"N", "A", "N", "A", "A", "N", "A", "N"};
  std::vector<std::string> const line_statuses{"O", "F", "F", "O", "F", "O", "O", "F"};
  std::array<std::vector<int64_t>, 5> const values{
    std::vector<int64_t>{1, 2, 3, 4, 5, 6, 7, 8},
    std::vector<int64_t>{10, 20, 30, 40, 50, 60, 70, 80},
    std::vector<int64_t>{100, 200, 300, 400, 500, 600, 700, 800},
    std::vector<int64_t>{1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000},
    std::vector<int64_t>{2, 3, 4, 5, 6, 7, 8, 9}};

  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(make_strings(return_flags, stream, mr));
  columns.push_back(make_strings(line_statuses, stream, mr));
  columns.push_back(make_fixed<int64_t>(cudf::data_type{cudf::type_id::DECIMAL64, -2},
                                        values[0],
                                        {false, true, true, true, true, true, true, true},
                                        stream,
                                        mr));
  for (std::size_t source = 1; source < values.size(); ++source) {
    columns.push_back(make_fixed<int64_t>(
      cudf::data_type{cudf::type_id::DECIMAL64, -2}, values[source], stream, mr));
  }

  auto input   = make_table(std::move(columns));
  auto attempt = try_tiny_domain_grouped_aggregate(
    input->view(), {0, 1}, q1_aggregate_kinds(), q1_aggregate_indices(), stream, mr);
  REQUIRE(attempt);
  REQUIRE_FALSE(attempt.register_private_attempted);
  REQUIRE_FALSE(attempt.used_register_private);
  REQUIRE_FALSE(attempt.used_sampled_preflight);
  REQUIRE(copy_values<__int128_t>(attempt.table->view().column(2), stream) ==
          std::vector<__int128_t>{7, 11, 11, 6});
  REQUIRE(copy_values<int32_t>(attempt.table->view().column(7), stream) ==
          std::vector<int32_t>{2, 2, 2, 1});
}

TEST_CASE(
  "Q1 sampled preflight reruns the full preflight when the prefix has fewer than four groups",
  "[physical_grouped_aggregate][tiny_domain_register][tiny_domain_sampled_preflight]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  auto const rows = q1_preflight_sample_rows + 3;
  std::vector<std::string> return_flags(rows, "A");
  std::vector<std::string> line_statuses(rows, "F");
  return_flags[q1_preflight_sample_rows]      = "A";
  line_statuses[q1_preflight_sample_rows]     = "O";
  return_flags[q1_preflight_sample_rows + 1]  = "N";
  line_statuses[q1_preflight_sample_rows + 1] = "F";
  return_flags[q1_preflight_sample_rows + 2]  = "N";
  line_statuses[q1_preflight_sample_rows + 2] = "O";
  std::array<std::vector<int64_t>, 5> const values{std::vector<int64_t>(rows, 1),
                                                   std::vector<int64_t>(rows, 2),
                                                   std::vector<int64_t>(rows, 3),
                                                   std::vector<int64_t>(rows, 4),
                                                   std::vector<int64_t>(rows, 5)};

  auto input   = make_q1_like_input(return_flags, line_statuses, values, false, stream, mr);
  auto attempt = try_tiny_domain_grouped_aggregate(
    input->view(), {0, 1}, q1_aggregate_kinds(), q1_aggregate_indices(), stream, mr);
  REQUIRE(attempt);
  REQUIRE(attempt.num_groups == 4);
  REQUIRE(attempt.register_private_attempted);
  REQUIRE(attempt.used_register_private);
  REQUIRE_FALSE(attempt.used_sampled_preflight);

  auto const view             = attempt.table->view();
  auto expected_return_flags  = make_strings({"A", "A", "N", "N"}, stream, mr);
  auto expected_line_statuses = make_strings({"F", "O", "F", "O"}, stream, mr);
  require_columns_equal(view.column(0), expected_return_flags->view(), stream, mr);
  require_columns_equal(view.column(1), expected_line_statuses->view(), stream, mr);
  auto const expected_counts =
    std::vector<int64_t>{static_cast<int64_t>(q1_preflight_sample_rows), 1, 1, 1};
  REQUIRE(copy_values<int64_t>(view.column(12), stream) == expected_counts);
  std::vector<__int128_t> expected_sums(expected_counts.begin(), expected_counts.end());
  REQUIRE(copy_values<__int128_t>(view.column(2), stream) == expected_sums);
}

TEST_CASE("Q1 sampled preflight fails to the exact generic path for a fifth key after the prefix",
          "[physical_grouped_aggregate][tiny_domain_register][tiny_domain_sampled_preflight]")
{
  auto memory_manager = sirius::test::operator_utils::initialize_memory_manager();
  auto* space         = memory_manager->get_memory_space(cucascade::memory::Tier::GPU, 0);
  REQUIRE(space != nullptr);
  auto stream = cudf::get_default_stream();
  auto mr     = space->get_default_allocator();

  auto const rows = q1_preflight_sample_rows + 1;
  std::vector<std::string> return_flags(rows);
  std::vector<std::string> line_statuses(rows);
  for (std::size_t row = 0; row < q1_preflight_sample_rows; ++row) {
    auto const group   = row % 4;
    return_flags[row]  = group < 2 ? "A" : "N";
    line_statuses[row] = group % 2 == 0 ? "F" : "O";
  }
  return_flags.back()  = "R";
  line_statuses.back() = "F";
  std::array<std::vector<int64_t>, 5> const values{std::vector<int64_t>(rows, 1),
                                                   std::vector<int64_t>(rows, 2),
                                                   std::vector<int64_t>(rows, 3),
                                                   std::vector<int64_t>(rows, 4),
                                                   std::vector<int64_t>(rows, 5)};

  auto input   = make_q1_like_input(return_flags, line_statuses, values, false, stream, mr);
  auto attempt = try_tiny_domain_grouped_aggregate(
    input->view(), {0, 1}, q1_aggregate_kinds(), q1_aggregate_indices(), stream, mr);
  REQUIRE_FALSE(attempt);
  REQUIRE(attempt.table == nullptr);
  REQUIRE(attempt.fallback_reason.find("missed a grouping key") != std::string::npos);

  // Exercise the same exact cuDF fallback invoked by the physical caller, then compare its full
  // expanded Q1 schema and every output column against the known five-group result.
  auto input_batch = sirius::make_data_batch(
    std::move(input), *space, stream, sirius::telemetry::batch_telemetry_info{});
  auto read_only_input = input_batch->to_read_only();
  auto generic_batch   = sirius::op::gpu_aggregate_impl::local_grouped_aggregate(
    read_only_input, {0, 1}, q1_aggregate_kinds(), q1_aggregate_indices(), {}, stream, *space);
  auto sorted = cudf::sort(sirius::get_cudf_table_view(*generic_batch), {}, {}, stream, mr);
  auto view   = sorted->view();
  REQUIRE(view.num_rows() == 5);
  REQUIRE(view.num_columns() == 13);

  auto expected_return_flags  = make_strings({"A", "A", "N", "N", "R"}, stream, mr);
  auto expected_line_statuses = make_strings({"F", "O", "F", "O", "F"}, stream, mr);
  require_columns_equal(view.column(0), expected_return_flags->view(), stream, mr);
  require_columns_equal(view.column(1), expected_line_statuses->view(), stream, mr);

  auto const rows_per_prefix_group = static_cast<int64_t>(q1_preflight_sample_rows / 4);
  auto const expected_counts       = std::vector<int64_t>{
    rows_per_prefix_group, rows_per_prefix_group, rows_per_prefix_group, rows_per_prefix_group, 1};
  auto require_sum = [&](cudf::size_type column, int64_t multiplier) {
    REQUIRE((view.column(column).type() == cudf::data_type{cudf::type_id::DECIMAL128, -2}));
    std::vector<__int128_t> expected;
    expected.reserve(expected_counts.size());
    for (auto count : expected_counts) {
      expected.push_back(static_cast<__int128_t>(count) * multiplier);
    }
    REQUIRE(copy_values<__int128_t>(view.column(column), stream) == expected);
  };
  require_sum(2, 1);
  require_sum(3, 2);
  require_sum(4, 3);
  require_sum(5, 4);
  require_sum(6, 1);
  require_sum(8, 2);
  require_sum(10, 5);
  auto const expected_counts32 =
    std::vector<int32_t>(expected_counts.begin(), expected_counts.end());
  REQUIRE(copy_values<int32_t>(view.column(7), stream) == expected_counts32);
  REQUIRE(copy_values<int32_t>(view.column(9), stream) == expected_counts32);
  REQUIRE(copy_values<int32_t>(view.column(11), stream) == expected_counts32);
  REQUIRE(copy_values<int64_t>(view.column(12), stream) == expected_counts);
}

TEST_CASE(
  "Q1 sampled preflight prefers exact fallback for a late multi-byte key with register overflow",
  "[physical_grouped_aggregate][tiny_domain_register][tiny_domain_sampled_preflight]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  constexpr std::size_t rows = 1U << 20;
  std::vector<std::string> return_flags(rows);
  std::vector<std::string> line_statuses(rows);
  for (std::size_t row = 0; row < rows; ++row) {
    auto const group   = row % 4;
    return_flags[row]  = group < 2 ? "A" : "N";
    line_statuses[row] = group % 2 == 0 ? "F" : "O";
  }
  line_statuses.back() = "FF";
  auto const maximum   = std::numeric_limits<int64_t>::max();
  std::array<std::vector<int64_t>, 5> const values{std::vector<int64_t>(rows, maximum),
                                                   std::vector<int64_t>(rows, maximum),
                                                   std::vector<int64_t>(rows, maximum),
                                                   std::vector<int64_t>(rows, maximum),
                                                   std::vector<int64_t>(rows, maximum)};

  auto input   = make_q1_like_input(return_flags, line_statuses, values, false, stream, mr);
  auto attempt = try_tiny_domain_grouped_aggregate(
    input->view(), {0, 1}, q1_aggregate_kinds(), q1_aggregate_indices(), stream, mr);
  REQUIRE_FALSE(attempt);
  REQUIRE(attempt.table == nullptr);
  REQUIRE(attempt.fallback_reason.find("exactly one byte") != std::string::npos);
}

TEST_CASE("Q1 register-private aggregate is exact under four-group contention",
          "[physical_grouped_aggregate][tiny_domain_register][tiny_domain_register_contention]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  constexpr std::size_t rows = 1U << 18;
  std::vector<std::string> return_flags(rows);
  std::vector<std::string> line_statuses(rows);
  for (std::size_t row = 0; row < rows; ++row) {
    auto const group   = row % 4;
    return_flags[row]  = group < 2 ? "A" : "N";
    line_statuses[row] = group % 2 == 0 ? "F" : "O";
  }
  std::array<std::vector<int64_t>, 5> const values{std::vector<int64_t>(rows, 1),
                                                   std::vector<int64_t>(rows, 2),
                                                   std::vector<int64_t>(rows, 3),
                                                   std::vector<int64_t>(rows, 4),
                                                   std::vector<int64_t>(rows, 5)};

  auto input   = make_q1_like_input(return_flags, line_statuses, values, false, stream, mr);
  auto attempt = try_tiny_domain_grouped_aggregate(
    input->view(), {0, 1}, q1_aggregate_kinds(), q1_aggregate_indices(), stream, mr);
  REQUIRE(attempt);
  REQUIRE(attempt.register_private_attempted);
  REQUIRE(attempt.used_register_private);
  REQUIRE(attempt.used_sampled_preflight);

  auto const view           = attempt.table->view();
  auto const rows_per_group = static_cast<int64_t>(rows / 4);
  auto repeated_sum         = [&](int64_t multiplier) {
    return std::vector<__int128_t>(4, static_cast<__int128_t>(rows_per_group) * multiplier);
  };
  REQUIRE(copy_values<__int128_t>(view.column(2), stream) == repeated_sum(1));
  REQUIRE(copy_values<__int128_t>(view.column(3), stream) == repeated_sum(2));
  REQUIRE(copy_values<__int128_t>(view.column(4), stream) == repeated_sum(3));
  REQUIRE(copy_values<__int128_t>(view.column(5), stream) == repeated_sum(4));
  REQUIRE(copy_values<__int128_t>(view.column(6), stream) == repeated_sum(1));
  REQUIRE(copy_values<__int128_t>(view.column(8), stream) == repeated_sum(2));
  REQUIRE(copy_values<__int128_t>(view.column(10), stream) == repeated_sum(5));
  REQUIRE((copy_values<int32_t>(view.column(7), stream) ==
           std::vector<int32_t>(4, static_cast<int32_t>(rows_per_group))));
  REQUIRE((copy_values<int32_t>(view.column(9), stream) ==
           std::vector<int32_t>(4, static_cast<int32_t>(rows_per_group))));
  REQUIRE((copy_values<int32_t>(view.column(11), stream) ==
           std::vector<int32_t>(4, static_cast<int32_t>(rows_per_group))));
  REQUIRE(
    (copy_values<int64_t>(view.column(12), stream) == std::vector<int64_t>(4, rows_per_group)));
}

TEST_CASE("Q1 register-private aggregate falls back on per-thread INT64 overflow",
          "[physical_grouped_aggregate][tiny_domain_register_overflow]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  constexpr std::size_t rows = 1U << 20;
  std::vector<std::string> return_flags(rows);
  std::vector<std::string> line_statuses(rows);
  for (std::size_t row = 0; row < rows; ++row) {
    auto const group   = row % 4;
    return_flags[row]  = group < 2 ? "A" : "N";
    line_statuses[row] = group % 2 == 0 ? "F" : "O";
  }
  auto const maximum = std::numeric_limits<int64_t>::max();
  std::array<std::vector<int64_t>, 5> const values{std::vector<int64_t>(rows, maximum),
                                                   std::vector<int64_t>(rows, maximum),
                                                   std::vector<int64_t>(rows, maximum),
                                                   std::vector<int64_t>(rows, maximum),
                                                   std::vector<int64_t>(rows, maximum)};

  auto input   = make_q1_like_input(return_flags, line_statuses, values, false, stream, mr);
  auto attempt = try_tiny_domain_grouped_aggregate(
    input->view(), {0, 1}, q1_aggregate_kinds(), q1_aggregate_indices(), stream, mr);
  REQUIRE(attempt);
  REQUIRE(attempt.register_private_attempted);
  REQUIRE_FALSE(attempt.used_register_private);
  REQUIRE(attempt.used_sampled_preflight);

  auto const view           = attempt.table->view();
  auto const rows_per_group = static_cast<int64_t>(rows / 4);
  auto const exact_sum      = static_cast<__int128_t>(maximum) * rows_per_group;
  auto const expected       = std::vector<__int128_t>(4, exact_sum);
  for (auto const column : std::array<cudf::size_type, 7>{2, 3, 4, 5, 6, 8, 10}) {
    REQUIRE(copy_values<__int128_t>(view.column(column), stream) == expected);
  }
  REQUIRE((copy_values<int32_t>(view.column(7), stream) ==
           std::vector<int32_t>(4, static_cast<int32_t>(rows_per_group))));
  REQUIRE((copy_values<int32_t>(view.column(9), stream) ==
           std::vector<int32_t>(4, static_cast<int32_t>(rows_per_group))));
  REQUIRE((copy_values<int32_t>(view.column(11), stream) ==
           std::vector<int32_t>(4, static_cast<int32_t>(rows_per_group))));
  REQUIRE(
    (copy_values<int64_t>(view.column(12), stream) == std::vector<int64_t>(4, rows_per_group)));
}

TEST_CASE("Q1 register-private negative overflow falls back before exact mixed-sign cancellation",
          "[physical_grouped_aggregate][tiny_domain_register_overflow]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  constexpr std::size_t rows = 1U << 20;
  std::vector<std::string> return_flags(rows);
  std::vector<std::string> line_statuses(rows);
  for (std::size_t row = 0; row < rows; ++row) {
    auto const group   = row % 4;
    return_flags[row]  = group < 2 ? "A" : "N";
    line_statuses[row] = group % 2 == 0 ? "F" : "O";
  }

  std::vector<int64_t> cancelling_values(rows);
  for (std::size_t row = 0; row < rows; ++row) {
    if (row < rows / 4) {
      cancelling_values[row] = std::numeric_limits<int64_t>::min();
    } else if (row < rows / 2) {
      cancelling_values[row] = -1;
    } else if (row < 3 * rows / 4) {
      cancelling_values[row] = std::numeric_limits<int64_t>::max();
    } else {
      cancelling_values[row] = 2;
    }
  }
  std::array<std::vector<int64_t>, 5> const values{
    cancelling_values, cancelling_values, cancelling_values, cancelling_values, cancelling_values};

  auto input   = make_q1_like_input(return_flags, line_statuses, values, false, stream, mr);
  auto attempt = try_tiny_domain_grouped_aggregate(
    input->view(), {0, 1}, q1_aggregate_kinds(), q1_aggregate_indices(), stream, mr);
  REQUIRE(attempt);
  REQUIRE(attempt.register_private_attempted);
  REQUIRE_FALSE(attempt.used_register_private);
  REQUIRE(attempt.used_sampled_preflight);

  auto const view = attempt.table->view();
  for (auto const column : std::array<cudf::size_type, 7>{2, 3, 4, 5, 6, 8, 10}) {
    REQUIRE(copy_values<__int128_t>(view.column(column), stream) == std::vector<__int128_t>(4, 0));
  }
  REQUIRE(copy_values<int64_t>(view.column(12), stream) ==
          std::vector<int64_t>(4, static_cast<int64_t>(rows / 4)));
}

TEST_CASE("fused Q1 projection aggregate matches the sequential projected path",
          "[physical_grouped_aggregate][tiny_domain_q1_projection]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  auto require_matches_sequential = [&](cudf::table_view raw, cudf::table_view projected) {
    auto fused = try_tiny_domain_q1_projection_aggregate(
      raw, q1_projection_plan(), q1_aggregate_kinds(), stream, mr);
    auto sequential = try_tiny_domain_grouped_aggregate(
      projected, {0, 1}, q1_aggregate_kinds(), q1_projection_aggregate_indices(), stream, mr);
    REQUIRE(fused);
    REQUIRE(sequential);
    REQUIRE(fused.register_private_attempted);
    REQUIRE(fused.used_register_private);
    REQUIRE(fused.num_groups == sequential.num_groups);
    require_tables_equal(fused.table->view(), sequential.table->view(), stream, mr);
  };

  SECTION("whole input")
  {
    std::vector<std::string> const return_flags{"N", "A", "N", "A", "A", "N", "A", "N"};
    std::vector<std::string> const line_statuses{"O", "F", "F", "O", "F", "O", "O", "F"};
    std::array<std::vector<int64_t>, 4> const values{
      std::vector<int64_t>{100, 200, 300, 400, 500, 600, 700, 800},
      std::vector<int64_t>{1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000},
      std::vector<int64_t>{5, 10, 15, 20, 25, 30, 35, 40},
      std::vector<int64_t>{2, 4, 6, 8, 10, 12, 14, 16}};

    auto raw       = make_q1_raw_input(return_flags, line_statuses, values, stream, mr);
    auto projected = make_q1_projected_input(return_flags, line_statuses, values, stream, mr);
    require_matches_sequential(raw->view(), projected->view());
  }

  SECTION("non-zero-offset slice")
  {
    std::vector<std::string> const return_flags{"Z", "N", "A", "N", "A", "A", "N", "A", "N", "Z"};
    std::vector<std::string> const line_statuses{"Z", "O", "F", "F", "O", "F", "O", "O", "F", "Z"};
    std::array<std::vector<int64_t>, 4> const values{
      std::vector<int64_t>{999, 100, 200, 300, 400, 500, 600, 700, 800, 999},
      std::vector<int64_t>{999, 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 999},
      std::vector<int64_t>{99, 5, 10, 15, 20, 25, 30, 35, 40, 99},
      std::vector<int64_t>{99, 2, 4, 6, 8, 10, 12, 14, 16, 99}};

    auto raw       = make_q1_raw_input(return_flags, line_statuses, values, stream, mr);
    auto projected = make_q1_projected_input(return_flags, line_statuses, values, stream, mr);
    auto raw_slice = cudf::slice(raw->view(), {1, raw->num_rows() - 1}, stream).front();
    auto projected_slice =
      cudf::slice(projected->view(), {1, projected->num_rows() - 1}, stream).front();
    REQUIRE(raw_slice.column(0).offset() == 1);
    REQUIRE(raw_slice.column(2).offset() == 1);
    REQUIRE(projected_slice.column(4).offset() == 1);
    require_matches_sequential(raw_slice, projected_slice);
  }
}

TEST_CASE("fused Q1 projection aggregate rejects a malformed logical-state map",
          "[physical_grouped_aggregate][tiny_domain_q1_projection]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();

  std::vector<std::string> const return_flags{"A", "A", "N", "N"};
  std::vector<std::string> const line_statuses{"F", "O", "F", "O"};
  std::array<std::vector<int64_t>, 4> const values{std::vector<int64_t>(4, 100),
                                                   std::vector<int64_t>(4, 1000),
                                                   std::vector<int64_t>(4, 5),
                                                   std::vector<int64_t>(4, 2)};
  auto raw                    = make_q1_raw_input(return_flags, line_statuses, values, stream, mr);
  auto plan                   = q1_projection_plan();
  plan.logical_to_physical[0] = 5;

  auto attempt =
    try_tiny_domain_q1_projection_aggregate(raw->view(), plan, q1_aggregate_kinds(), stream, mr);
  REQUIRE_FALSE(attempt);
  REQUIRE(attempt.table == nullptr);
  REQUIRE(attempt.fallback_reason.find("logical states do not match") != std::string::npos);
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
