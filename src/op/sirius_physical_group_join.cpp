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

#include "op/sirius_physical_group_join.hpp"

#include "cudf/cudf_utils.hpp"
#include "data/data_batch_utils.hpp"
#include "log/logging.hpp"
#include "memory/size_arithmetic.hpp"
#include "op/aggregate/group_join_impl.hpp"
#include "op/dynamic_filter/dynamic_filter_publisher.hpp"
#include "op/sirius_physical_delim_join.hpp"
#include "pipeline/sirius_meta_pipeline.hpp"
#include "pipeline/sirius_pipeline.hpp"
#include "sirius/exception.hpp"

#include <cudf/aggregation.hpp>
#include <cudf/binaryop.hpp>
#include <cudf/column/column_factories.hpp>
#include <cudf/concatenate.hpp>
#include <cudf/copying.hpp>
#include <cudf/groupby.hpp>
#include <cudf/join/join.hpp>
#include <cudf/null_mask.hpp>
#include <cudf/replace.hpp>
#include <cudf/scalar/scalar.hpp>
#include <cudf/table/table_view.hpp>
#include <cudf/unary.hpp>
#include <cudf/utilities/type_dispatcher.hpp>

#include <rmm/aligned.hpp>
#include <rmm/cuda_device.hpp>
#include <rmm/error.hpp>

#include <cuda_runtime_api.h>
#include <nvtx3/nvtx3.hpp>

#include <cucascade/memory/memory_space.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace sirius::op {

namespace {
[[nodiscard]] cudf::size_type checked_cudf_size(std::size_t value, std::string_view what)
{
  auto const max = static_cast<std::size_t>(std::numeric_limits<cudf::size_type>::max());
  if (value > max) {
    throw sirius::invalid_input_exception(
      "group_join: {} {} exceeds cudf::size_type max {}", what, value, max);
  }
  return static_cast<cudf::size_type>(value);
}

[[nodiscard]] cudf::column_view checked_column(cudf::table_view batch,
                                               std::size_t index,
                                               std::size_t batch_index,
                                               std::string_view role)
{
  auto const num_columns = batch.num_columns();
  if (num_columns < 0 || index >= static_cast<std::size_t>(num_columns)) {
    throw sirius::internal_exception(
      "group_join: input batch {} has {} columns; {} column index {} is out of range",
      batch_index,
      num_columns,
      role,
      index);
  }
  return batch.column(static_cast<cudf::size_type>(index));
}

[[nodiscard]] int64_t checked_null_count(cudf::column_view const& column, std::size_t batch_index)
{
  auto const nulls = column.null_count();
  if (nulls < 0 || nulls > column.size()) {
    throw sirius::internal_exception(
      "group_join: preserved batch {} has invalid null count {} for {} rows",
      batch_index,
      nulls,
      column.size());
  }
  return static_cast<int64_t>(nulls);
}

[[nodiscard]] int64_t checked_add_rows(int64_t total, int64_t rows, std::string_view side)
{
  if (rows < 0 || total > std::numeric_limits<int64_t>::max() - rows) {
    throw sirius::invalid_input_exception(
      "group_join: {} row count exceeds BIGINT accounting capacity", side);
  }
  return total + rows;
}

[[nodiscard]] bool count_product_needs_validation(int64_t preserved_rows,
                                                  int64_t counted_rows,
                                                  bool count_star) noexcept
{
  auto const lhs = static_cast<uint64_t>(preserved_rows);
  auto const rhs =
    static_cast<uint64_t>(count_star ? std::max<int64_t>(counted_rows, 1) : counted_rows);
  auto const max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  return rhs != 0 && lhs > max / rhs;
}

// EXCLUDE implements COUNT(col); INCLUDE implements COUNT(*) and preserved-key presence.
std::unique_ptr<cudf::table> sparse_partial_count(cudf::column_view const& keys,
                                                  cudf::column_view const& values,
                                                  cudf::null_policy value_policy,
                                                  rmm::cuda_stream_view stream,
                                                  rmm::device_async_resource_ref mr)
{
  cudf::groupby::groupby gb(cudf::table_view({keys}), cudf::null_policy::EXCLUDE, cudf::sorted::NO);
  std::vector<cudf::groupby::aggregation_request> requests(1);
  requests[0].values = values;
  requests[0].aggregations.push_back(
    cudf::make_count_aggregation<cudf::groupby_aggregation>(value_policy));
  auto [group_keys, results] = gb.aggregate(requests, stream, mr);
  // cuDF groupby COUNT emits size_type (INT32); widen so the partial merge sums in INT64.
  auto count64 =
    cudf::cast(results[0].results[0]->view(), cudf::data_type{cudf::type_id::INT64}, stream, mr);
  auto key_cols = group_keys->release();
  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(std::move(key_cols[0]));
  columns.push_back(std::move(count64));
  return std::make_unique<cudf::table>(std::move(columns));
}

std::unique_ptr<cudf::table> sparse_merge_pair(std::unique_ptr<cudf::table> lhs,
                                               std::unique_ptr<cudf::table> rhs,
                                               rmm::cuda_stream_view stream,
                                               rmm::device_async_resource_ref mr)
{
  std::vector<cudf::table_view> views{lhs->view(), rhs->view()};
  auto combined = cudf::concatenate(views, stream, mr);
  lhs.reset();
  rhs.reset();

  auto merged = [&] {
    cudf::groupby::groupby gb(
      cudf::table_view({combined->view().column(0)}), cudf::null_policy::EXCLUDE, cudf::sorted::NO);
    std::vector<cudf::groupby::aggregation_request> requests(1);
    requests[0].values = combined->view().column(1);
    requests[0].aggregations.push_back(cudf::make_sum_aggregation<cudf::groupby_aggregation>());
    auto [group_keys, results] = gb.aggregate(requests, stream, mr);
    auto key_cols              = group_keys->release();
    std::vector<std::unique_ptr<cudf::column>> columns;
    columns.push_back(std::move(key_cols[0]));
    columns.push_back(std::move(results[0].results[0]));
    return std::make_unique<cudf::table>(std::move(columns));
  }();
  combined.reset();
  return merged;
}

// Merge in balanced pairs to avoid one all-partials concatenation.
std::unique_ptr<cudf::table> sparse_merge_partials(
  std::vector<std::unique_ptr<cudf::table>> partials,
  cudf::data_type key_type,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr)
{
  if (partials.empty()) {
    std::vector<std::unique_ptr<cudf::column>> columns;
    columns.push_back(
      cudf::make_fixed_width_column(key_type, 0, cudf::mask_state::UNALLOCATED, stream, mr));
    columns.push_back(cudf::make_fixed_width_column(
      cudf::data_type{cudf::type_id::INT64}, 0, cudf::mask_state::UNALLOCATED, stream, mr));
    return std::make_unique<cudf::table>(std::move(columns));
  }
  while (partials.size() > 1) {
    std::vector<std::unique_ptr<cudf::table>> next;
    next.reserve(partials.size() / 2 + partials.size() % 2);
    for (std::size_t i = 0; i < partials.size(); i += 2) {
      if (i + 1 == partials.size()) {
        next.push_back(std::move(partials[i]));
      } else {
        auto lhs = std::move(partials[i]);
        auto rhs = std::move(partials[i + 1]);
        next.push_back(sparse_merge_pair(std::move(lhs), std::move(rhs), stream, mr));
      }
    }
    partials = std::move(next);
  }
  return std::move(partials.front());
}

cudf::column_view gather_map_view(rmm::device_uvector<cudf::size_type> const& indices)
{
  return cudf::column_view(cudf::data_type{cudf::type_id::INT32},
                           checked_cudf_size(indices.size(), "sparse gather-map length"),
                           indices.data(),
                           nullptr,
                           0,
                           0,
                           {});
}

// Monomorphized dense pathway: build the count-bundle state, run both accumulate passes, emit.
template <typename KeyT, typename CountT>
std::unique_ptr<cudf::table> run_dense_count(
  int64_t min_key,
  int64_t range,
  std::vector<cudf::column_view> const& preserved_keys,
  std::vector<cudf::column_view> const& counted_keys,
  std::vector<std::optional<cudf::column_view>> const& counted_values,
  cudf::data_type key_type,
  bool count_star,
  int64_t null_group_rows,
  bool check_product_overflow,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr)
{
  group_join_state<KeyT, groupjoin::count_bundle<CountT>> state(min_key, range, stream, mr);
  for (auto const& col : preserved_keys) {
    state.accumulate_preserved(col, stream);
  }
  for (std::size_t i = 0; i < counted_keys.size(); ++i) {
    state.accumulate_counted(
      counted_keys[i], counted_values[i] ? &*counted_values[i] : nullptr, stream);
  }
  return state.emit(key_type, count_star, null_group_rows, stream, mr, check_product_overflow);
}

// ---------------------------------------------------------------------------------------------
// INNER/DIRECT value-form helpers. Aggregate arguments flow as their integer representation:
// DECIMAL32/64 accumulate and compare as unscaled reps (order-preserving under the fixed scale)
// and the declared-type conversion happens once, at finalize.
// ---------------------------------------------------------------------------------------------

[[nodiscard]] cudf::column_view arg_rep_view(cudf::column_view const& col)
{
  switch (col.type().id()) {
    case cudf::type_id::INT32:
    case cudf::type_id::INT64: return col;
    case cudf::type_id::DECIMAL32:
      return cudf::bit_cast(col, cudf::data_type{cudf::type_id::INT32});
    case cudf::type_id::DECIMAL64:
      return cudf::bit_cast(col, cudf::data_type{cudf::type_id::INT64});
    default:
      throw sirius::internal_exception(
        "group_join: unsupported aggregate argument type {} (expected "
        "INT32/INT64/DECIMAL32/DECIMAL64)",
        static_cast<int32_t>(col.type().id()));
  }
}

/// Coarse host bound for dense SUM/AVG admission: the int64 accumulation provably cannot overflow
/// when counted_rows x max(|vmin|, |vmax|) fits in int64. Inconclusive declines dense (parity
/// with the generic path's unchecked int64 accumulation is then preserved by the sparse strategy).
[[nodiscard]] bool sum_accumulation_bound_safe(int64_t counted_rows,
                                               int64_t value_min,
                                               int64_t value_max) noexcept
{
  auto const unsigned_abs = [](int64_t v) {
    return v < 0 ? ~static_cast<uint64_t>(v) + 1 : static_cast<uint64_t>(v);
  };
  auto const magnitude = std::max(unsigned_abs(value_min), unsigned_abs(value_max));
  if (magnitude == 0 || counted_rows == 0) { return true; }
  auto const bigint_max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  return static_cast<uint64_t>(counted_rows) <= bigint_max / magnitude;
}

/// Zero-copy retype of an INT64 column to an 8-byte-representation type, keeping the validity
/// mask.
std::unique_ptr<cudf::column> retype_int64_rep(std::unique_ptr<cudf::column> col,
                                               cudf::data_type target)
{
  auto const rows       = col->size();
  auto const null_count = col->null_count();
  auto contents         = col->release();
  return std::make_unique<cudf::column>(
    target,
    rows,
    std::move(*contents.data),
    contents.null_mask ? std::move(*contents.null_mask) : rmm::device_buffer{},
    null_count);
}

/// Converts the raw INT64 aggregate (plus AVG's divisor) to the declared output type. Mirrors the
/// generic aggregate's DECIMAL widening (`gpu_aggregate_impl.cpp`) and the grouped-merge AVG
/// finalize (`sirius_physical_grouped_aggregate_merge.cpp`): DECIMAL AVG divides in fixed point,
/// everything else divides in FLOAT64.
std::unique_ptr<cudf::column> finalize_value_column(std::unique_ptr<cudf::column> value,
                                                    std::unique_ptr<cudf::column> avg_divisor,
                                                    groupjoin::agg_op op,
                                                    cudf::data_type declared,
                                                    cudf::data_type arg_type,
                                                    rmm::cuda_stream_view stream,
                                                    rmm::device_async_resource_ref mr)
{
  using groupjoin::agg_op;
  bool const arg_is_decimal =
    arg_type.id() == cudf::type_id::DECIMAL32 || arg_type.id() == cudf::type_id::DECIMAL64;
  // Reinterpreting the accumulation with the argument's scale before any cast makes cudf's
  // fixed-point conversions rescale correctly.
  auto as_arg_scaled = [&](std::unique_ptr<cudf::column> col) {
    return retype_int64_rep(std::move(col),
                            cudf::data_type{cudf::type_id::DECIMAL64, arg_type.scale()});
  };
  switch (op) {
    case agg_op::COUNT_STAR:
    case agg_op::COUNT_VALID: return value;
    case agg_op::SUM:
    case agg_op::MIN:
    case agg_op::MAX: {
      if (!arg_is_decimal) {
        if (declared.id() == cudf::type_id::INT64) { return value; }
        return cudf::cast(value->view(), declared, stream, mr);
      }
      auto scaled = as_arg_scaled(std::move(value));
      if (scaled->type() == declared) { return scaled; }
      return cudf::cast(scaled->view(), declared, stream, mr);
    }
    case agg_op::AVG: {
      if (avg_divisor == nullptr) {
        throw sirius::internal_exception("group_join: AVG finalize requires the divisor column");
      }
      bool const declared_decimal = declared.id() == cudf::type_id::DECIMAL32 ||
                                    declared.id() == cudf::type_id::DECIMAL64 ||
                                    declared.id() == cudf::type_id::DECIMAL128;
      if (declared_decimal) {
        auto numerator = arg_is_decimal ? as_arg_scaled(std::move(value)) : std::move(value);
        return cudf::binary_operation(
          numerator->view(), avg_divisor->view(), cudf::binary_operator::DIV, declared, stream, mr);
      }
      auto const f64 = cudf::data_type{cudf::type_id::FLOAT64};
      std::unique_ptr<cudf::column> numerator;
      if (arg_is_decimal) {
        auto scaled = as_arg_scaled(std::move(value));
        numerator   = cudf::cast(scaled->view(), f64, stream, mr);
      } else {
        numerator = cudf::cast(value->view(), f64, stream, mr);
      }
      auto denominator = cudf::cast(avg_divisor->view(), f64, stream, mr);
      return cudf::binary_operation(
        numerator->view(), denominator->view(), cudf::binary_operator::DIV, f64, stream, mr);
    }
  }
  throw sirius::internal_exception("group_join: unknown aggregate op in finalize");
}

/// Widens a groupby result column to INT64 so the pairwise merge accumulates uniformly.
std::unique_ptr<cudf::column> widen_to_int64(std::unique_ptr<cudf::column> col,
                                             rmm::cuda_stream_view stream,
                                             rmm::device_async_resource_ref mr)
{
  if (col->type().id() == cudf::type_id::INT64) { return col; }
  return cudf::cast(col->view(), cudf::data_type{cudf::type_id::INT64}, stream, mr);
}

/// Per-batch partial for the INNER/DIRECT sparse strategy: `[key, value:int64]`, plus AVG's
/// valid-argument count computed in the same groupby pass. Aggregate outputs keep their validity
/// masks (all-NULL-argument groups stay NULL through the merge). DIRECT passes
/// `null_policy::INCLUDE` so NULL keys form a real group.
std::unique_ptr<cudf::table> sparse_partial_value(cudf::column_view const& keys,
                                                  cudf::column_view const* rep_args,
                                                  groupjoin::agg_op op,
                                                  cudf::null_policy key_policy,
                                                  rmm::cuda_stream_view stream,
                                                  rmm::device_async_resource_ref mr)
{
  using groupjoin::agg_op;
  cudf::groupby::groupby gb(cudf::table_view({keys}), key_policy, cudf::sorted::NO);
  std::vector<cudf::groupby::aggregation_request> requests(1);
  switch (op) {
    case agg_op::COUNT_STAR:
      requests[0].values = keys;
      requests[0].aggregations.push_back(
        cudf::make_count_aggregation<cudf::groupby_aggregation>(cudf::null_policy::INCLUDE));
      break;
    case agg_op::COUNT_VALID:
      requests[0].values = *rep_args;
      requests[0].aggregations.push_back(
        cudf::make_count_aggregation<cudf::groupby_aggregation>(cudf::null_policy::EXCLUDE));
      break;
    case agg_op::SUM:
      requests[0].values = *rep_args;
      requests[0].aggregations.push_back(cudf::make_sum_aggregation<cudf::groupby_aggregation>());
      break;
    case agg_op::MIN:
      requests[0].values = *rep_args;
      requests[0].aggregations.push_back(cudf::make_min_aggregation<cudf::groupby_aggregation>());
      break;
    case agg_op::MAX:
      requests[0].values = *rep_args;
      requests[0].aggregations.push_back(cudf::make_max_aggregation<cudf::groupby_aggregation>());
      break;
    case agg_op::AVG:
      requests[0].values = *rep_args;
      requests[0].aggregations.push_back(cudf::make_sum_aggregation<cudf::groupby_aggregation>());
      requests[0].aggregations.push_back(
        cudf::make_count_aggregation<cudf::groupby_aggregation>(cudf::null_policy::EXCLUDE));
      break;
  }
  auto [group_keys, results] = gb.aggregate(requests, stream, mr);
  auto key_cols              = group_keys->release();
  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(std::move(key_cols[0]));
  for (auto& result : results[0].results) {
    columns.push_back(widen_to_int64(std::move(result), stream, mr));
  }
  return std::make_unique<cudf::table>(std::move(columns));
}

/// Lossless partial-merge aggregations: min/min, max/max, and sum for everything else (counts
/// merge by summation).
void push_merge_aggregation(std::vector<cudf::groupby::aggregation_request>& requests,
                            groupjoin::agg_op op)
{
  switch (op) {
    case groupjoin::agg_op::MIN:
      requests.back().aggregations.push_back(
        cudf::make_min_aggregation<cudf::groupby_aggregation>());
      break;
    case groupjoin::agg_op::MAX:
      requests.back().aggregations.push_back(
        cudf::make_max_aggregation<cudf::groupby_aggregation>());
      break;
    default:
      requests.back().aggregations.push_back(
        cudf::make_sum_aggregation<cudf::groupby_aggregation>());
      break;
  }
}

std::unique_ptr<cudf::table> sparse_merge_value_pair(std::unique_ptr<cudf::table> lhs,
                                                     std::unique_ptr<cudf::table> rhs,
                                                     groupjoin::agg_op op,
                                                     cudf::null_policy key_policy,
                                                     rmm::cuda_stream_view stream,
                                                     rmm::device_async_resource_ref mr)
{
  std::vector<cudf::table_view> views{lhs->view(), rhs->view()};
  auto combined = cudf::concatenate(views, stream, mr);
  lhs.reset();
  rhs.reset();

  auto const num_value_cols = combined->num_columns() - 1;
  cudf::groupby::groupby gb(
    cudf::table_view({combined->view().column(0)}), key_policy, cudf::sorted::NO);
  std::vector<cudf::groupby::aggregation_request> requests;
  requests.reserve(static_cast<std::size_t>(num_value_cols));
  for (cudf::size_type i = 0; i < num_value_cols; ++i) {
    requests.emplace_back();
    requests.back().values = combined->view().column(i + 1);
    // The first value column carries the op's partial; AVG's second column is a count.
    push_merge_aggregation(requests, i == 0 ? op : groupjoin::agg_op::COUNT_VALID);
  }
  auto [group_keys, results] = gb.aggregate(requests, stream, mr);
  combined.reset();
  auto key_cols = group_keys->release();
  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(std::move(key_cols[0]));
  for (auto& result : results) {
    columns.push_back(std::move(result.results[0]));
  }
  return std::make_unique<cudf::table>(std::move(columns));
}

// Merge in balanced pairs to avoid one all-partials concatenation (the count path's discipline).
std::unique_ptr<cudf::table> sparse_merge_value_partials(
  std::vector<std::unique_ptr<cudf::table>> partials,
  cudf::data_type key_type,
  groupjoin::agg_op op,
  cudf::null_policy key_policy,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr)
{
  if (partials.empty()) {
    std::vector<std::unique_ptr<cudf::column>> columns;
    columns.push_back(
      cudf::make_fixed_width_column(key_type, 0, cudf::mask_state::UNALLOCATED, stream, mr));
    columns.push_back(cudf::make_fixed_width_column(
      cudf::data_type{cudf::type_id::INT64}, 0, cudf::mask_state::UNALLOCATED, stream, mr));
    if (op == groupjoin::agg_op::AVG) {
      columns.push_back(cudf::make_fixed_width_column(
        cudf::data_type{cudf::type_id::INT64}, 0, cudf::mask_state::UNALLOCATED, stream, mr));
    }
    return std::make_unique<cudf::table>(std::move(columns));
  }
  while (partials.size() > 1) {
    std::vector<std::unique_ptr<cudf::table>> next;
    next.reserve(partials.size() / 2 + partials.size() % 2);
    for (std::size_t i = 0; i < partials.size(); i += 2) {
      if (i + 1 == partials.size()) {
        next.push_back(std::move(partials[i]));
      } else {
        next.push_back(sparse_merge_value_pair(
          std::move(partials[i]), std::move(partials[i + 1]), op, key_policy, stream, mr));
      }
    }
    partials = std::move(next);
  }
  return std::move(partials.front());
}

[[nodiscard]] std::string_view form_name(groupjoin::join_form form)
{
  switch (form) {
    case groupjoin::join_form::OUTER_PRESERVING: return "OUTER_PRESERVING";
    case groupjoin::join_form::INNER: return "INNER";
    case groupjoin::join_form::DIRECT: return "DIRECT";
  }
  return "UNKNOWN";
}

[[nodiscard]] std::string_view agg_op_name(groupjoin::agg_op op)
{
  switch (op) {
    case groupjoin::agg_op::COUNT_STAR: return "COUNT_STAR";
    case groupjoin::agg_op::COUNT_VALID: return "COUNT_VALID";
    case groupjoin::agg_op::SUM: return "SUM";
    case groupjoin::agg_op::MIN: return "MIN";
    case groupjoin::agg_op::MAX: return "MAX";
    case groupjoin::agg_op::AVG: return "AVG";
  }
  return "UNKNOWN";
}

[[nodiscard]] bool is_count_op(groupjoin::agg_op op) noexcept
{
  return op == groupjoin::agg_op::COUNT_STAR || op == groupjoin::agg_op::COUNT_VALID;
}

// COUNT_STAR and COUNT_VALID collapse on the dense path: the argument-validity gate admits only
// NULL-free argument columns, under which the two counts are provably equal.
[[nodiscard]] groupjoin::dense_value_op dense_op_for(groupjoin::agg_op op)
{
  switch (op) {
    case groupjoin::agg_op::COUNT_STAR:
    case groupjoin::agg_op::COUNT_VALID: return groupjoin::dense_value_op::COUNT;
    case groupjoin::agg_op::SUM: return groupjoin::dense_value_op::SUM;
    case groupjoin::agg_op::MIN: return groupjoin::dense_value_op::MIN;
    case groupjoin::agg_op::MAX: return groupjoin::dense_value_op::MAX;
    case groupjoin::agg_op::AVG: return groupjoin::dense_value_op::AVG;
  }
  throw sirius::internal_exception("group_join: unknown aggregate op");
}

// The whitelisted (form, bundle) combinations; everything else fails closed at construction.
void validate_group_join_spec(groupjoin::group_join_spec const& spec,
                              duckdb::vector<sirius::logical_type> const& types)
{
  if (spec.slots.size() != 1) {
    throw sirius::internal_exception("group_join: expected exactly 1 aggregate slot, got {}",
                                     spec.slots.size());
  }
  auto const& slot = spec.slots[0];
  if (spec.form == groupjoin::join_form::OUTER_PRESERVING && !is_count_op(slot.op)) {
    // The value-over-outer seam: unmatched preserved groups need NULL aggregate outputs, a mask
    // the dense emit does not produce. Such shapes stay on the generic path.
    throw sirius::internal_exception(
      "group_join: value bundles over OUTER_PRESERVING are not implemented");
  }
  if (slot.arg_idx.has_value() == (slot.op == groupjoin::agg_op::COUNT_STAR)) {
    throw sirius::internal_exception(
      "group_join: COUNT(*) forbids an argument column and {} requires one", agg_op_name(slot.op));
  }
  if (types.size() != 2) {
    throw sirius::internal_exception(
      "group_join: expected [key, aggregate] output schema, got {} columns", types.size());
  }
  if (!(types[1] == slot.output_type)) {
    throw sirius::internal_exception(
      "group_join: slot output type {} does not match the declared output column type {}",
      slot.output_type.to_string(),
      types[1].to_string());
  }
  bool output_type_ok = false;
  switch (slot.op) {
    case groupjoin::agg_op::COUNT_STAR:
    case groupjoin::agg_op::COUNT_VALID:
      output_type_ok = slot.output_type.id() == sirius::type_id::BIGINT;
      break;
    case groupjoin::agg_op::SUM:
      output_type_ok = slot.output_type.id() == sirius::type_id::BIGINT ||
                       slot.output_type.id() == sirius::type_id::DECIMAL;
      break;
    case groupjoin::agg_op::MIN:
    case groupjoin::agg_op::MAX:
      output_type_ok = slot.output_type.id() == sirius::type_id::INTEGER ||
                       slot.output_type.id() == sirius::type_id::BIGINT ||
                       slot.output_type.id() == sirius::type_id::DECIMAL;
      break;
    case groupjoin::agg_op::AVG:
      output_type_ok = slot.output_type.id() == sirius::type_id::DOUBLE ||
                       slot.output_type.id() == sirius::type_id::DECIMAL;
      break;
  }
  if (!output_type_ok) {
    throw sirius::internal_exception("group_join: {} cannot produce declared output type {}",
                                     agg_op_name(slot.op),
                                     slot.output_type.to_string());
  }
}

}  // namespace

group_join_input::tagged_batches group_join_input::tag_batches(
  std::vector<std::shared_ptr<::cucascade::data_batch>> preserved_batches,
  std::vector<std::shared_ptr<::cucascade::data_batch>> counted_batches)
{
  if (preserved_batches.size() > std::numeric_limits<std::size_t>::max() - counted_batches.size()) {
    throw sirius::internal_exception("group_join: input batch count overflow");
  }

  tagged_batches result;
  auto const total_batches = preserved_batches.size() + counted_batches.size();
  result.batches.reserve(total_batches);
  result.sides.reserve(total_batches);

  auto append = [&](std::vector<std::shared_ptr<::cucascade::data_batch>> batches,
                    input_side side,
                    std::string_view side_name) {
    for (std::size_t i = 0; i < batches.size(); ++i) {
      if (!batches[i]) {
        throw sirius::internal_exception("group_join: null {} batch at index {}", side_name, i);
      }
      result.batches.push_back(std::move(batches[i]));
      result.sides.push_back(side);
    }
  };
  append(std::move(preserved_batches), input_side::PRESERVED, "preserved");
  append(std::move(counted_batches), input_side::COUNTED, "counted");
  return result;
}

group_join_input::group_join_input(
  std::vector<std::shared_ptr<::cucascade::data_batch>> preserved_batches,
  std::vector<std::shared_ptr<::cucascade::data_batch>> counted_batches)
  : group_join_input(tag_batches(std::move(preserved_batches), std::move(counted_batches)))
{
}

group_join_input::group_join_input(tagged_batches input)
  : pipelineable_operator_data(std::move(input.batches)), _input_sides(std::move(input.sides))
{
  if (_input_sides.size() != get_data_batches().size()) {
    throw sirius::internal_exception("group_join: input side metadata is not batch-aligned");
  }
}

sirius_physical_group_join::sirius_physical_group_join(
  duckdb::vector<sirius::logical_type> types,
  std::size_t estimated_cardinality,
  groupjoin::group_join_spec spec,
  dynamic_filter_publish_plan dynamic_filter_plan,
  dynamic_filter_stats* dynamic_filter_stats_sink)
  : sirius_physical_operator(
      SiriusPhysicalOperatorType::GROUP_JOIN, std::move(types), estimated_cardinality),
    _spec(std::move(spec)),
    _dynamic_filter_plan(std::move(dynamic_filter_plan)),
    _dynamic_filter_stats(dynamic_filter_stats_sink)
{
  validate_group_join_spec(_spec, this->types);
}

std::string sirius_physical_group_join::params_to_string() const
{
  // The count-spec string is load-bearing for plan-parity comparisons; only the new forms extend
  // it.
  if (_spec.form == groupjoin::join_form::OUTER_PRESERVING) {
    return " (preserved_key=" + std::to_string(_spec.preserved_key_idx) +
           ", counted_key=" + std::to_string(_spec.counted_key_idx) +
           (counted_value_idx() ? ", count_col=" + std::to_string(*counted_value_idx())
                                : std::string(", count_star")) +
           ", max_state_bytes=" + std::to_string(_spec.max_state_bytes) + ")";
  }
  return " (form=" + std::string(form_name(_spec.form)) +
         ", op=" + std::string(agg_op_name(_spec.slots[0].op)) +
         ", preserved_key=" + std::to_string(_spec.preserved_key_idx) +
         ", counted_key=" + std::to_string(_spec.counted_key_idx) +
         (counted_value_idx() ? ", arg_col=" + std::to_string(*counted_value_idx())
                              : std::string(", count_star")) +
         ", max_state_bytes=" + std::to_string(_spec.max_state_bytes) + ")";
}

std::string_view sirius_physical_group_join::input_port_for(
  sirius_physical_operator const& producer) const
{
  if (children.size() != 2) {
    throw sirius::internal_exception("GROUP_JOIN repository wiring requires exactly two children");
  }
  if (children[0].get() == &producer) { return PRESERVED_PORT; }
  if (children[1].get() == &producer) { return COUNTED_PORT; }
  // A routing-only DELIM_SCAN preserved child produces no data of its own: the preserved batches
  // arrive from the owning delim join's distinct-chain root, which the converter retargets to
  // this operator (mirroring the hash join's non-child CONCAT producer mapping). Accept exactly
  // that producer; every other non-child producer stays an error.
  if (auto* owning_delim = producer.owning_delim_join()) {
    for (auto const& delim_scan : owning_delim->delim_scans) {
      if (&delim_scan.get() == children[0].get()) { return PRESERVED_PORT; }
    }
  }
  throw sirius::internal_exception("GROUP_JOIN repository wiring source is not a direct child");
}

MemoryBarrierType sirius_physical_group_join::input_barrier_for(
  sirius_physical_operator const& /*producer*/) const
{
  return MemoryBarrierType::FULL;
}

void sirius_physical_group_join::build_pipelines(pipeline::sirius_pipeline& current,
                                                 pipeline::sirius_meta_pipeline& meta_pipeline)
{
  // FULL-barrier sink with one input port per child.
  if (children.size() != 2) {
    throw sirius::internal_exception("group_join: expected 2 children, got {}", children.size());
  }
  auto& sink_meta    = meta_pipeline.create_child_meta_pipeline(current, *this);
  auto& host_current = *sink_meta.get_base_pipeline();

  auto build_child_side = [&](sirius_physical_operator& child) {
    // Wiring by provenance: a routing-only DELIM_SCAN child appends no operator and produces no
    // data -- its build_pipelines registers the mandatory scheduling dependency on the owning
    // delim join's distinct chain. Invoke it in place against this operator's sink pipeline;
    // wrapping it as the sink of a fresh producer pipeline would skip that protocol and leave a
    // sourceless dead pipeline.
    if (child.type == SiriusPhysicalOperatorType::DELIM_SCAN) {
      child.build_pipelines(host_current, sink_meta);
      return;
    }
    auto& child_meta = sink_meta.create_child_meta_pipeline(host_current, child);
    if (child.children.empty()) { return; }
    if (child.children.size() != 1) {
      throw sirius::internal_exception("group_join: child subtree root must be unary or a leaf");
    }
    child_meta.build(*child.children[0]);
  };
  build_child_side(*children[1]);
  build_child_side(*children[0]);
}

std::optional<task_creation_hint> sirius_physical_group_join::get_next_task_hint()
{
  // The first poll after the preserved producer finishes is the publication trigger; the call is
  // a cheap no-op for operators without a publication plan.
  maybe_publish_preserved_membership();

  if (ports.empty()) { return std::nullopt; }

  // Either input may be empty, but both producers must finish before the task runs.
  for (auto const& p : _ports_list) {
    if (p->src_pipeline && !p->src_pipeline->is_pipeline_finished()) {
      auto* producer = &(p->src_pipeline->get_operators()[0].get());
      return task_creation_hint{TaskCreationHint::WAITING_FOR_INPUT_DATA, producer};
    }
  }
  if (!all_ports_empty()) { return task_creation_hint{TaskCreationHint::READY, this}; }
  return std::nullopt;
}

std::unique_ptr<operator_data> sirius_physical_group_join::get_next_task_input_data()
{
  std::vector<std::shared_ptr<::cucascade::data_batch>> preserved_batches;
  std::vector<std::shared_ptr<::cucascade::data_batch>> counted_batches;

  auto drain = [](port* input_port,
                  std::vector<std::shared_ptr<::cucascade::data_batch>>& destination) {
    if (input_port->repo == nullptr) { return; }
    while (auto batch = input_port->repo->pop_next_data_batch()) {
      destination.push_back(std::move(batch));
    }
  };
  drain(get_port(PRESERVED_PORT), preserved_batches);
  drain(get_port(COUNTED_PORT), counted_batches);

  if (preserved_batches.empty() && counted_batches.empty()) { return nullptr; }
  return std::make_unique<group_join_input>(std::move(preserved_batches),
                                            std::move(counted_batches));
}

void sirius_physical_group_join::maybe_publish_preserved_membership()
{
  if (!_dynamic_filter_plan.enabled()) { return; }
  if (_dynamic_filter_publication_state.load(std::memory_order_acquire) !=
      dynamic_filter_publication_state::OPEN) {
    return;
  }
  auto const port_it = ports.find(std::string(PRESERVED_PORT));
  if (port_it == ports.end()) { return; }
  auto* preserved_port = port_it->second;
  if (preserved_port->repo == nullptr || preserved_port->src_pipeline == nullptr ||
      !preserved_port->src_pipeline->is_pipeline_finished()) {
    return;
  }

  // Publish only from the complete preserved side; a partial filter could drop valid join rows.
  // The CAS makes the attempt one-shot under concurrent hint polls.
  auto expected = dynamic_filter_publication_state::OPEN;
  if (!_dynamic_filter_publication_state.compare_exchange_strong(
        expected,
        dynamic_filter_publication_state::PUBLISHING,
        std::memory_order_acq_rel,
        std::memory_order_acquire)) {
    return;
  }
  if (_dynamic_filter_stats != nullptr) {
    _dynamic_filter_stats->publication_attempts.fetch_add(1, std::memory_order_relaxed);
  }

  // Publication is optional: any failure below downgrades to "no filters" rather than failing the
  // query, because this runs on the task-creation path where an escaped exception would not be
  // attributed to the query.
  try {
    auto const batch_ids = preserved_port->repo->get_batch_ids(0);
    std::shared_ptr<::cucascade::data_batch> preserved_batch;
    if (batch_ids.size() == 1 && preserved_port->repo->total_size() == 1) {
      preserved_batch = preserved_port->repo->get_data_batch_by_id(batch_ids[0], 0);
    }
    if (preserved_batch == nullptr) {
      SIRIUS_LOG_DEBUG(
        "[sirius_physical_group_join] dynamic-filter publication (id={}) skipped: the preserved "
        "side finished as {} batch(es), not one whole delivery.",
        get_operator_id(),
        preserved_port->repo->total_size());
      if (_dynamic_filter_stats != nullptr) {
        _dynamic_filter_stats->publications_skipped_build_not_whole.fetch_add(
          1, std::memory_order_relaxed);
      }
      _dynamic_filter_publication_state.store(dynamic_filter_publication_state::CLOSED,
                                              std::memory_order_release);
      return;
    }

    auto preserved_ro = preserved_batch->to_read_only();
    auto* ms          = preserved_ro.get_data() ? preserved_ro.get_memory_space() : nullptr;
    bool const gpu_resident =
      ms != nullptr && preserved_ro.get_current_tier() == ::cucascade::memory::Tier::GPU;
    bool const source_usable =
      gpu_resident && _dynamic_filter_plan.has_replica_on_device(ms->get_device_id());
    if (!source_usable) {
      SIRIUS_LOG_DEBUG(
        "[sirius_physical_group_join] dynamic-filter publication (id={}) skipped: the preserved "
        "batch is {}.",
        get_operator_id(),
        gpu_resident ? "resident on a GPU this plan holds no replica space for"
                     : "not GPU-resident");
      if (_dynamic_filter_stats != nullptr) {
        _dynamic_filter_stats->publications_skipped_source_not_resident.fetch_add(
          1, std::memory_order_relaxed);
      }
      _dynamic_filter_publication_state.store(dynamic_filter_publication_state::CLOSED,
                                              std::memory_order_release);
      return;
    }

    nvtx3::scoped_range nvtx_range{"group_join::dynfilter_publish"};
    // Wait for the preserved writer before reading on the publication stream.
    rmm::cuda_set_device_raii device_guard{rmm::cuda_device_id{ms->get_device_id()}};
    auto publish_stream = ms->acquire_stream();
    if (auto const writer_event = preserved_ro.get_writer_event(); writer_event != nullptr) {
      auto const status = cudaStreamWaitEvent(publish_stream.value(), writer_event, 0);
      if (status != cudaSuccess) {
        throw sirius::internal_exception("group_join: dynamic-filter writer-event wait failed: {}",
                                         cudaGetErrorString(status));
      }
    } else {
      auto const status = cudaDeviceSynchronize();
      if (status != cudaSuccess) {
        throw sirius::internal_exception(
          "group_join: dynamic-filter source synchronization failed: {}",
          cudaGetErrorString(status));
      }
    }
    publish_preserved_membership(sirius::get_cudf_table_view(preserved_ro), publish_stream);
  } catch (std::exception const& error) {
    auto claimed = dynamic_filter_publication_state::PUBLISHING;
    if (_dynamic_filter_publication_state.compare_exchange_strong(
          claimed,
          dynamic_filter_publication_state::FAILED,
          std::memory_order_acq_rel,
          std::memory_order_acquire) &&
        _dynamic_filter_stats != nullptr) {
      _dynamic_filter_stats->publications_failed.fetch_add(1, std::memory_order_relaxed);
    }
    SIRIUS_LOG_WARN(
      "[sirius_physical_group_join] dynamic-filter publication (id={}) failed; continuing "
      "without filters: {}",
      get_operator_id(),
      error.what());
  }
}

void sirius_physical_group_join::publish_preserved_membership(
  cudf::table_view const& preserved_view, rmm::cuda_stream_view stream)
{
  // The trigger owns the PUBLISHING claim until this function sets a terminal state.
  D_ASSERT(_dynamic_filter_publication_state.load(std::memory_order_acquire) ==
           dynamic_filter_publication_state::PUBLISHING);

  try {
    auto const outcome =
      sirius::op::publish_dynamic_filters(_dynamic_filter_plan, preserved_view, stream);
    SIRIUS_LOG_DEBUG(
      "[sirius_physical_group_join] dynamic-filter publication: {} key(s) considered, {} skipped "
      "(domain gate), {} skipped (type mismatch), {} membership + {} zone-map built, {} filter(s) "
      "pushed across {} active target(s).",
      outcome.keys_considered,
      outcome.keys_skipped_domain_gate,
      outcome.keys_skipped_type_mismatch,
      outcome.membership_filters_built,
      outcome.zone_map_filters_built,
      outcome.filters_pushed,
      outcome.active_targets);
    if (_dynamic_filter_stats != nullptr) {
      auto& stats        = *_dynamic_filter_stats;
      auto const relaxed = std::memory_order_relaxed;
      stats.keys_considered.fetch_add(outcome.keys_considered, relaxed);
      stats.keys_with_known_domain.fetch_add(outcome.keys_with_known_domain, relaxed);
      stats.keys_skipped_domain_gate.fetch_add(outcome.keys_skipped_domain_gate, relaxed);
      stats.keys_skipped_type_mismatch.fetch_add(outcome.keys_skipped_type_mismatch, relaxed);
      stats.keys_build_exceeded_domain.fetch_add(outcome.keys_build_exceeded_domain, relaxed);
      stats.membership_filters_built.fetch_add(outcome.membership_filters_built, relaxed);
      stats.zone_map_filters_built.fetch_add(outcome.zone_map_filters_built, relaxed);
      stats.publications_skipped_targets_drained.fetch_add(outcome.skipped_targets_drained,
                                                           relaxed);
      stats.filters_pushed.fetch_add(outcome.filters_pushed, relaxed);
    }
    _dynamic_filter_publication_state.store(dynamic_filter_publication_state::FINISHED,
                                            std::memory_order_release);
    if (_dynamic_filter_stats != nullptr) {
      _dynamic_filter_stats->publications_finished.fetch_add(1, std::memory_order_relaxed);
    }
  } catch (rmm::out_of_memory const& oom) {
    // Dynamic filters are optional; device OOM fails publication without failing the query.
    _dynamic_filter_publication_state.store(dynamic_filter_publication_state::FAILED,
                                            std::memory_order_release);
    if (_dynamic_filter_stats != nullptr) {
      _dynamic_filter_stats->publications_failed.fetch_add(1, std::memory_order_relaxed);
    }
    SIRIUS_LOG_WARN(
      "[sirius_physical_group_join] dynamic-filter publication (id={}) hit device memory "
      "exhaustion; continuing without filters: {}",
      get_operator_id(),
      oom.what());
  }
}

void sirius_physical_group_join::on_finalize_operator()
{
  // Finalization closes only an unclaimed publication window.
  auto expected = dynamic_filter_publication_state::OPEN;
  _dynamic_filter_publication_state.compare_exchange_strong(
    expected,
    dynamic_filter_publication_state::CLOSED,
    std::memory_order_acq_rel,
    std::memory_order_acquire);
}

std::size_t sirius_physical_group_join::no_history_peak_memory_estimate(
  const input_stats& stats) const
{
  using sirius::memory::saturating_add;
  using sirius::memory::saturating_mul;

  constexpr std::size_t allocation_floor = 1024 * 1024;
  constexpr auto allocation_alignment    = rmm::CUDA_ALLOCATION_ALIGNMENT;
  auto const aligned_charge              = [](std::size_t bytes) {
    if (bytes == 0) { return std::size_t{0}; }
    auto const padded = saturating_add(bytes, static_cast<std::size_t>(allocation_alignment - 1));
    if (padded == std::numeric_limits<std::size_t>::max()) { return padded; }
    return (padded / allocation_alignment) * allocation_alignment;
  };

  // Dense state bytes are capped at min(max_state_bytes, 4 * input bytes) -- both the budget gate
  // and the state-vs-input gate hold for every form, whatever the per-array slot widths resolve
  // to at runtime.
  auto const histogram_cap   = static_cast<std::size_t>(_spec.max_state_bytes);
  auto const histogram_bytes = std::min(histogram_cap, saturating_mul(4, stats.bytes));

  auto const key_width      = sirius::get_cudf_type(types[0]).id() == cudf::type_id::INT32
                                ? sizeof(int32_t)
                                : sizeof(int64_t);
  auto const cudf_row_limit = static_cast<std::size_t>(std::numeric_limits<cudf::size_type>::max());
  auto const output_rows    = std::min(stats.bytes / key_width, cudf_row_limit);
  auto const selected_bytes = saturating_mul(sizeof(int64_t), output_rows);

  bool const count_spec = _spec.form == groupjoin::join_form::OUTER_PRESERVING;
  auto const& slot      = _spec.slots[0];
  bool const is_avg     = slot.op == groupjoin::agg_op::AVG;
  bool const has_sum    = slot.op == groupjoin::agg_op::SUM || is_avg;

  // Emitted columns: [key, INT64 value] for the count spec; the new forms add AVG's INT64
  // divisor, the declared-type finalize column, and (for AVG's FLOAT64 branch) two cast
  // temporaries.
  std::size_t per_row_output_bytes = saturating_add(key_width, sizeof(int64_t));
  if (!count_spec) {
    auto const out_width = static_cast<std::size_t>(cudf::size_of(sirius::get_cudf_type(types[1])));
    per_row_output_bytes = saturating_add(per_row_output_bytes, out_width);
    if (is_avg) {
      per_row_output_bytes = saturating_add(per_row_output_bytes, 3 * sizeof(int64_t));
    }
  }
  auto const output_bytes = saturating_mul(per_row_output_bytes, output_rows);
  auto mask_bytes         = static_cast<std::size_t>(
    cudf::bitmask_allocation_size_bytes(checked_cudf_size(output_rows, "output row bound")));
  // The new forms' sparse strategy can mask the aggregate column as well as the key.
  if (!count_spec) { mask_bytes = saturating_mul(2, mask_bytes); }

  auto dense_peak = saturating_add(allocation_floor, histogram_bytes);
  dense_peak      = saturating_add(dense_peak, selected_bytes);
  dense_peak      = saturating_add(dense_peak, output_bytes);
  dense_peak      = saturating_add(dense_peak, mask_bytes);
  dense_peak      = saturating_add(dense_peak, histogram_bytes);  // selection/CUB workspace

  // Retain each batch's value/validity scalars through the final sync and charge them separately.
  // SUM/AVG bundles widen the device extrema array to 4 slots and add the argument's per-batch
  // minmax scalars.
  auto const global_extrema = aligned_charge((count_spec || !has_sum ? 2 : 4) * sizeof(int64_t));
  auto scalar_bytes = saturating_add(aligned_charge(key_width), aligned_charge(sizeof(bool)));
  if (!count_spec && has_sum) {
    scalar_bytes = saturating_add(
      scalar_bytes, saturating_add(aligned_charge(sizeof(int64_t)), aligned_charge(sizeof(bool))));
  }
  auto const extrema_per_batch = saturating_mul(2, scalar_bytes);
  auto minmax_peak             = saturating_add(allocation_floor, global_extrema);
  minmax_peak = saturating_add(minmax_peak, saturating_mul(stats.num_batches, extrema_per_batch));

  auto const sparse_peak = saturating_add(allocation_floor, saturating_mul(16, stats.bytes));
  return std::max({dense_peak, sparse_peak, minmax_peak});
}

std::unique_ptr<operator_data> sirius_physical_group_join::execute(const operator_data& input_data,
                                                                   rmm::cuda_stream_view stream)
{
  nvtx3::scoped_range nvtx_range{"sirius_physical_group_join::execute"};
  auto const* input = dynamic_cast<const group_join_input*>(&input_data);
  if (input == nullptr) {
    throw sirius::internal_exception("group_join: unexpected input data type");
  }
  auto const ro_batches   = input->get_read_only_batches();
  auto const& input_sides = input->input_sides();
  if (input_sides.size() != ro_batches.size()) {
    throw sirius::internal_exception(
      "group_join: {} side tags are not aligned with {} materialized batches",
      input_sides.size(),
      ro_batches.size());
  }

  // Task preparation colocates every input in the reservation space, so any batch supplies the
  // allocator.
  cucascade::memory::memory_space* space = nullptr;
  for (auto const& batch : ro_batches) {
    if (batch.get_memory_space() != nullptr) {
      space = batch.get_memory_space();
      break;
    }
  }
  if (space == nullptr) {
    throw sirius::internal_exception("group_join: no memory space on input batches");
  }
  auto mr = space->get_default_allocator();

  auto output = _spec.form == groupjoin::join_form::OUTER_PRESERVING
                  ? execute_count_outer(ro_batches, input_sides, stream, mr)
                  : execute_inner_direct(ro_batches, input_sides, stream, mr);

  SIRIUS_LOG_INFO("[group_join] emitted {} group rows ({} strategy)",
                  output->num_rows(),
                  _last_strategy == strategy::DENSE ? "dense" : "sparse");

  std::vector<std::shared_ptr<::cucascade::data_batch>> results;
  results.push_back(sirius::make_data_batch(std::move(output), *space, stream, batch_telemetry()));
  return std::make_unique<pipelineable_operator_data>(std::move(results));
}

std::unique_ptr<cudf::table> sirius_physical_group_join::execute_count_outer(
  std::vector<::cucascade::read_only_data_batch> const& ro_batches,
  std::vector<group_join_input::input_side> const& input_sides,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr)
{
  auto const key_type   = sirius::get_cudf_type(types[0]);
  auto require_key_type = [&](cudf::column_view const& col, char const* side) {
    if (col.type().id() != key_type.id()) {
      throw sirius::internal_exception(
        "group_join: {} key column carrier {} does not match declared key type {}",
        side,
        static_cast<int32_t>(col.type().id()),
        static_cast<int32_t>(key_type.id()));
    }
  };

  auto const counted_value_index = counted_value_idx();
  std::vector<cudf::column_view> preserved_keys;
  std::vector<cudf::column_view> counted_keys;
  std::vector<std::optional<cudf::column_view>> counted_values;
  int64_t preserved_rows          = 0;
  int64_t preserved_null_keys     = 0;
  int64_t counted_rows            = 0;
  std::size_t input_logical_bytes = 0;
  for (std::size_t i = 0; i < ro_batches.size(); ++i) {
    auto const* representation = ro_batches[i].get_data();
    if (representation == nullptr) {
      throw sirius::internal_exception("group_join: input batch {} has no representation", i);
    }
    input_logical_bytes = sirius::memory::saturating_add(
      input_logical_bytes, representation->get_uncompressed_data_size_in_bytes());
    auto const batch_view = sirius::get_cudf_table_view(ro_batches[i]);
    if (input_sides[i] == group_join_input::input_side::PRESERVED) {
      auto const col = checked_column(batch_view, _spec.preserved_key_idx, i, "preserved key");
      require_key_type(col, "preserved");
      preserved_rows =
        checked_add_rows(preserved_rows, static_cast<int64_t>(col.size()), "preserved");
      preserved_null_keys =
        checked_add_rows(preserved_null_keys, checked_null_count(col, i), "preserved NULL-key");
      preserved_keys.push_back(col);
    } else {
      auto const col = checked_column(batch_view, _spec.counted_key_idx, i, "counted key");
      require_key_type(col, "counted");
      counted_rows = checked_add_rows(counted_rows, static_cast<int64_t>(col.size()), "counted");
      counted_keys.push_back(col);
      if (counted_value_index) {
        counted_values.emplace_back(
          checked_column(batch_view, *counted_value_index, i, "COUNT argument"));
      } else {
        counted_values.emplace_back(std::nullopt);
      }
    }
  }

  bool const count_star = !counted_value_index.has_value();
  bool const check_product_overflow =
    count_product_needs_validation(preserved_rows, counted_rows, count_star);
  int64_t const non_null_keys = preserved_rows - preserved_null_keys;

  std::unique_ptr<cudf::table> output;
  if (non_null_keys == 0) {
    _last_strategy = strategy::DENSE;
    output         = group_join_empty_output(key_type, count_star, preserved_null_keys, stream, mr);
  } else {
    auto const min_max = group_join_global_minmax(preserved_keys, stream, mr);
    if (!min_max) {
      throw sirius::internal_exception(
        "group_join: minmax reported no valid keys but null accounting found {}", non_null_keys);
    }

    // A zero unsigned range denotes the full 64-bit domain and forces the sparse path.
    uint64_t const range_u =
      static_cast<uint64_t>(min_max->second) - static_cast<uint64_t>(min_max->first) + 1;
    // The count bundle keeps the joint promotion rule: either side at 2^32 rows widens both
    // count arrays.
    bool const wide = preserved_rows >= std::numeric_limits<uint32_t>::max() ||
                      counted_rows >= std::numeric_limits<uint32_t>::max();
    uint64_t const slot_bytes          = wide ? sizeof(uint64_t) : sizeof(uint32_t);
    uint64_t const combined_slot_bytes = 2 * slot_bytes;
    auto const size_max                = std::numeric_limits<std::size_t>::max();
    bool const layout_valid = range_u != 0 && range_u <= size_max / combined_slot_bytes &&
                              range_u <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    auto const histogram_bytes =
      layout_valid ? static_cast<std::size_t>(range_u * combined_slot_bytes) : size_max;
    auto const non_null_rows = static_cast<std::size_t>(non_null_keys);
    auto const total_rows = sirius::memory::saturating_add(static_cast<std::size_t>(preserved_rows),
                                                           static_cast<std::size_t>(counted_rows));
    bool const dense_ok = layout_valid && range_u <= _spec.max_state_bytes / combined_slot_bytes &&
                          range_u <= sirius::memory::saturating_mul(8, non_null_rows) &&
                          range_u <= sirius::memory::saturating_mul(2, total_rows) &&
                          histogram_bytes <= sirius::memory::saturating_mul(4, input_logical_bytes);

    if (dense_ok) {
      _last_strategy   = strategy::DENSE;
      auto const range = static_cast<int64_t>(range_u);
      SIRIUS_LOG_INFO(
        "[group_join] dense path: keys in [{}, {}] (range {}, {}-bit slots), preserved "
        "rows {} (null keys {}), counted rows {}",
        min_max->first,
        min_max->second,
        range,
        wide ? 64 : 32,
        preserved_rows,
        preserved_null_keys,
        counted_rows);
      auto const dispatch_key = [&](auto key_tag) {
        using KeyT = decltype(key_tag);
        return wide ? run_dense_count<KeyT, uint64_t>(min_max->first,
                                                      range,
                                                      preserved_keys,
                                                      counted_keys,
                                                      counted_values,
                                                      key_type,
                                                      count_star,
                                                      preserved_null_keys,
                                                      check_product_overflow,
                                                      stream,
                                                      mr)
                    : run_dense_count<KeyT, uint32_t>(min_max->first,
                                                      range,
                                                      preserved_keys,
                                                      counted_keys,
                                                      counted_values,
                                                      key_type,
                                                      count_star,
                                                      preserved_null_keys,
                                                      check_product_overflow,
                                                      stream,
                                                      mr);
      };
      switch (key_type.id()) {
        case cudf::type_id::INT32: output = dispatch_key(int32_t{}); break;
        case cudf::type_id::INT64: output = dispatch_key(int64_t{}); break;
        default:
          throw sirius::internal_exception(
            "group_join: unsupported key column type {} (expected INT32/INT64)",
            static_cast<int32_t>(key_type.id()));
      }
    } else {
      _last_strategy = strategy::SPARSE;
      SIRIUS_LOG_INFO(
        "[group_join] sparse path: keys in [{}, {}], range {}, histogram bytes {}, "
        "input bytes {}, budget {}",
        min_max->first,
        min_max->second,
        range_u,
        histogram_bytes,
        input_logical_bytes,
        _spec.max_state_bytes);

      std::vector<std::unique_ptr<cudf::table>> counted_partials;
      for (std::size_t i = 0; i < counted_keys.size(); ++i) {
        if (counted_keys[i].size() == 0) { continue; }
        auto const& values = counted_values[i] ? *counted_values[i] : counted_keys[i];
        auto const policy =
          counted_values[i] ? cudf::null_policy::EXCLUDE : cudf::null_policy::INCLUDE;
        counted_partials.push_back(
          sparse_partial_count(counted_keys[i], values, policy, stream, mr));
      }
      auto counted_agg = sparse_merge_partials(std::move(counted_partials), key_type, stream, mr);

      // Preserved side: distinct keys with their multiplicity (duplicate preserved keys
      // multiply the per-key match count, matching join-then-group-by semantics).
      std::vector<std::unique_ptr<cudf::table>> preserved_partials;
      for (auto const& col : preserved_keys) {
        if (col.size() == 0) { continue; }
        preserved_partials.push_back(
          sparse_partial_count(col, col, cudf::null_policy::INCLUDE, stream, mr));
      }
      auto preserved_agg =
        sparse_merge_partials(std::move(preserved_partials), key_type, stream, mr);

      auto const preserved_key_view      = cudf::table_view({preserved_agg->view().column(0)});
      auto const counted_key_view        = cudf::table_view({counted_agg->view().column(0)});
      auto [left_indices, right_indices] = cudf::left_join(
        preserved_key_view, counted_key_view, cudf::null_equality::UNEQUAL, stream, mr);

      auto keys_out = cudf::gather(preserved_key_view,
                                   gather_map_view(*left_indices),
                                   cudf::out_of_bounds_policy::DONT_CHECK,
                                   stream,
                                   mr);
      auto presence = cudf::gather(cudf::table_view({preserved_agg->view().column(1)}),
                                   gather_map_view(*left_indices),
                                   cudf::out_of_bounds_policy::DONT_CHECK,
                                   stream,
                                   mr);
      auto matched  = cudf::gather(cudf::table_view({counted_agg->view().column(1)}),
                                  gather_map_view(*right_indices),
                                  cudf::out_of_bounds_policy::NULLIFY,
                                  stream,
                                  mr);
      left_indices.reset();
      right_indices.reset();
      preserved_agg.reset();
      counted_agg.reset();

      cudf::numeric_scalar<int64_t> zero(0, true, stream, mr);
      auto matched_filled = cudf::replace_nulls(matched->view().column(0), zero, stream, mr);
      matched.reset();
      if (count_star) {
        // COUNT(*): unmatched preserved rows survive the outer join as one row each.
        cudf::numeric_scalar<int64_t> one(1, true, stream, mr);
        matched_filled = cudf::binary_operation(matched_filled->view(),
                                                one,
                                                cudf::binary_operator::NULL_MAX,
                                                cudf::data_type{cudf::type_id::INT64},
                                                stream,
                                                mr);
      }
      if (check_product_overflow) {
        throw_if_count_product_overflows(
          presence->view().column(0), matched_filled->view(), stream, mr);
      }
      auto values = cudf::binary_operation(presence->view().column(0),
                                           matched_filled->view(),
                                           cudf::binary_operator::MUL,
                                           cudf::data_type{cudf::type_id::INT64},
                                           stream,
                                           mr);
      presence.reset();
      matched_filled.reset();

      std::vector<std::unique_ptr<cudf::column>> columns;
      columns.push_back(std::move(keys_out->release()[0]));
      columns.push_back(std::move(values));
      output = std::make_unique<cudf::table>(std::move(columns));

      if (preserved_null_keys > 0) {
        if (output->num_rows() == std::numeric_limits<cudf::size_type>::max()) {
          throw sirius::invalid_input_exception(
            "group_join: adding the NULL group would exceed cudf::size_type max {}",
            std::numeric_limits<cudf::size_type>::max());
        }
        auto null_group =
          group_join_empty_output(key_type, count_star, preserved_null_keys, stream, mr);
        std::vector<cudf::table_view> parts{output->view(), null_group->view()};
        output = cudf::concatenate(parts, stream, mr);
      }
    }
  }

  return output;
}

std::unique_ptr<cudf::table> sirius_physical_group_join::execute_inner_direct(
  std::vector<::cucascade::read_only_data_batch> const& ro_batches,
  std::vector<group_join_input::input_side> const& input_sides,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr)
{
  using groupjoin::agg_op;
  using groupjoin::join_form;

  bool const is_direct  = _spec.form == join_form::DIRECT;
  auto const& slot      = _spec.slots[0];
  auto const key_type   = sirius::get_cudf_type(types[0]);
  auto const out_type   = sirius::get_cudf_type(types[1]);
  auto require_key_type = [&](cudf::column_view const& col, char const* side) {
    if (col.type().id() != key_type.id()) {
      throw sirius::internal_exception(
        "group_join: {} key column carrier {} does not match declared key type {}",
        side,
        static_cast<int32_t>(col.type().id()),
        static_cast<int32_t>(key_type.id()));
    }
  };

  // Harvest per-batch key columns (and, for argument-taking ops, the argument's integer
  // representation) with checked row accounting. DIRECT takes a single "counted" input.
  bool const needs_argument = slot.op != agg_op::COUNT_STAR;
  std::vector<cudf::column_view> preserved_keys;
  std::vector<cudf::column_view> counted_keys;
  std::vector<cudf::column_view> counted_rep_args;
  cudf::data_type arg_type{cudf::type_id::EMPTY};
  int64_t preserved_rows          = 0;
  int64_t preserved_null_keys     = 0;
  int64_t counted_rows            = 0;
  int64_t counted_null_keys       = 0;
  int64_t argument_null_values    = 0;
  std::size_t input_logical_bytes = 0;
  for (std::size_t i = 0; i < ro_batches.size(); ++i) {
    auto const* representation = ro_batches[i].get_data();
    if (representation == nullptr) {
      throw sirius::internal_exception("group_join: input batch {} has no representation", i);
    }
    input_logical_bytes = sirius::memory::saturating_add(
      input_logical_bytes, representation->get_uncompressed_data_size_in_bytes());
    auto const batch_view = sirius::get_cudf_table_view(ro_batches[i]);
    if (input_sides[i] == group_join_input::input_side::PRESERVED) {
      if (is_direct) {
        throw sirius::internal_exception(
          "group_join: the DIRECT form takes a single counted input, got a preserved batch");
      }
      auto const col = checked_column(batch_view, _spec.preserved_key_idx, i, "preserved key");
      require_key_type(col, "preserved");
      preserved_rows =
        checked_add_rows(preserved_rows, static_cast<int64_t>(col.size()), "preserved");
      preserved_null_keys =
        checked_add_rows(preserved_null_keys, checked_null_count(col, i), "preserved NULL-key");
      preserved_keys.push_back(col);
    } else {
      auto const col = checked_column(batch_view, _spec.counted_key_idx, i, "counted key");
      require_key_type(col, "counted");
      counted_rows = checked_add_rows(counted_rows, static_cast<int64_t>(col.size()), "counted");
      counted_null_keys =
        checked_add_rows(counted_null_keys, checked_null_count(col, i), "counted NULL-key");
      counted_keys.push_back(col);
      if (needs_argument) {
        auto const arg = checked_column(batch_view, *slot.arg_idx, i, "aggregate argument");
        if (arg_type.id() == cudf::type_id::EMPTY) {
          arg_type = arg.type();
        } else if (arg.type() != arg_type) {
          throw sirius::internal_exception(
            "group_join: aggregate argument carrier changed across batches ({} versus {})",
            static_cast<int32_t>(arg.type().id()),
            static_cast<int32_t>(arg_type.id()));
        }
        // Metadata-only argument-validity gate accounting (checked before state allocation).
        argument_null_values =
          checked_add_rows(argument_null_values, checked_null_count(arg, i), "argument NULL");
        counted_rep_args.push_back(arg_rep_view(arg));
      }
    }
  }

  auto const empty_output = [&] {
    std::vector<std::unique_ptr<cudf::column>> columns;
    columns.push_back(
      cudf::make_fixed_width_column(key_type, 0, cudf::mask_state::UNALLOCATED, stream, mr));
    columns.push_back(
      cudf::make_fixed_width_column(out_type, 0, cudf::mask_state::UNALLOCATED, stream, mr));
    return std::make_unique<cudf::table>(std::move(columns));
  };

  // INNER emits nothing without both sides; DIRECT emits nothing without rows. These trivial
  // short-circuits count as the dense strategy, mirroring the count path's empty handling.
  if (counted_rows == 0 || (!is_direct && preserved_rows - preserved_null_keys == 0)) {
    _last_strategy = strategy::DENSE;
    return empty_output();
  }

  bool const bundle_has_sum = slot.op == agg_op::SUM || slot.op == agg_op::AVG;

  // The argument-validity gate: any NULL argument (organic or outer-join padding inside a DIRECT
  // child) routes the task to the mask-preserving sparse strategy, which is exact for nullable
  // arguments.
  bool dense_eligible = argument_null_values == 0;

  std::optional<group_join_extrema> extrema;
  if (dense_eligible) {
    // Key extrema size the state domain: the preserved side for INNER (counted keys are
    // bounds-checked), the sole input for DIRECT. SUM/AVG fold the argument extrema into the same
    // pass and the same single sync.
    auto const& domain_keys = is_direct ? counted_keys : preserved_keys;
    if (bundle_has_sum) {
      extrema = group_join_global_minmax_with_values(domain_keys, counted_rep_args, stream, mr);
    } else if (auto const key_minmax = group_join_global_minmax(domain_keys, stream, mr)) {
      extrema = group_join_extrema{key_minmax->first, key_minmax->second, false, 0, 0};
    }
    if (!extrema) {
      if (!is_direct) {
        throw sirius::internal_exception(
          "group_join: minmax reported no valid keys but {} non-NULL preserved rows exist",
          preserved_rows - preserved_null_keys);
      }
      // DIRECT with only NULL keys still owns the NULL group; the sparse path groups it.
      dense_eligible = false;
    }
  }

  uint64_t range_u        = 0;
  std::size_t state_bytes = std::numeric_limits<std::size_t>::max();
  bool presence_wide      = false;
  bool matched_wide       = false;
  if (dense_eligible) {
    // A zero unsigned range denotes the full 64-bit domain and forces the sparse path.
    range_u = static_cast<uint64_t>(extrema->key_max) - static_cast<uint64_t>(extrema->key_min) + 1;
    // Per-array widths (new forms only): presence bounds by preserved rows, matched by counted
    // rows; payloads are always 64-bit.
    presence_wide = preserved_rows >= std::numeric_limits<uint32_t>::max();
    matched_wide  = counted_rows >= std::numeric_limits<uint32_t>::max();
    uint64_t const combined_slot_bytes =
      (is_direct ? 0 : (presence_wide ? sizeof(uint64_t) : sizeof(uint32_t))) +
      (matched_wide ? sizeof(uint64_t) : sizeof(uint32_t)) +
      (is_count_op(slot.op) ? 0 : sizeof(int64_t));
    // DIRECT allocates one extra slot for the NULL group.
    uint64_t const alloc_slots = range_u + (is_direct ? 1 : 0);
    auto const size_max        = std::numeric_limits<std::size_t>::max();
    bool const layout_valid =
      range_u != 0 && alloc_slots >= range_u && alloc_slots <= size_max / combined_slot_bytes &&
      range_u <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - 1;
    state_bytes =
      layout_valid ? static_cast<std::size_t>(alloc_slots * combined_slot_bytes) : size_max;
    auto const total_rows = sirius::memory::saturating_add(static_cast<std::size_t>(preserved_rows),
                                                           static_cast<std::size_t>(counted_rows));
    // Gate table for the INNER/DIRECT forms: (a) layout, (b) budget, (d) range vs rows, and (e)
    // state vs input bytes. Gate (c) (range vs live preserved keys) is deliberately dropped: on
    // the asymmetric shapes these forms target, declining dense would eager-aggregate an enormous
    // counted side for a tiny group set.
    dense_eligible = layout_valid && alloc_slots <= _spec.max_state_bytes / combined_slot_bytes &&
                     range_u <= sirius::memory::saturating_mul(2, total_rows) &&
                     state_bytes <= sirius::memory::saturating_mul(4, input_logical_bytes);
    // SUM/AVG overflow policy: decline dense unless the coarse host bound proves the int64
    // accumulation safe (COUNT keeps its exact product machinery; MIN/MAX need none).
    if (dense_eligible && bundle_has_sum && extrema->has_value_extrema &&
        !sum_accumulation_bound_safe(counted_rows, extrema->value_min, extrema->value_max)) {
      dense_eligible = false;
    }
  }

  auto const dense_op = dense_op_for(slot.op);
  std::unique_ptr<cudf::table> raw;
  if (dense_eligible) {
    _last_strategy   = strategy::DENSE;
    auto const range = static_cast<int64_t>(range_u);
    SIRIUS_LOG_INFO(
      "[group_join] {} {} dense path: keys in [{}, {}] (range {}, presence {}-bit, matched "
      "{}-bit), preserved rows {}, counted rows {} (null keys {})",
      form_name(_spec.form),
      agg_op_name(slot.op),
      extrema->key_min,
      extrema->key_max,
      range,
      presence_wide ? 64 : 32,
      matched_wide ? 64 : 32,
      preserved_rows,
      counted_rows,
      counted_null_keys);
    bool const check_count_overflow =
      is_count_op(slot.op) && !is_direct &&
      count_product_needs_validation(preserved_rows, counted_rows, /*count_star=*/false);

    auto const dispatch_arg = [&](auto key_tag, auto matched_tag, auto arg_tag) {
      using KeyT     = decltype(key_tag);
      using MatchedT = decltype(matched_tag);
      using ArgT     = decltype(arg_tag);
      if (is_direct) {
        // NULL-group presence is host-known from the batch metadata scan; passing it down keeps
        // the dense DIRECT driver free of any readback sync of its own.
        return group_join_dense_direct<KeyT, MatchedT, ArgT>(dense_op,
                                                             extrema->key_min,
                                                             range,
                                                             counted_keys,
                                                             counted_rep_args,
                                                             key_type,
                                                             counted_null_keys > 0,
                                                             stream,
                                                             mr);
      }
      return presence_wide
               ? group_join_dense_inner<KeyT, uint64_t, MatchedT, ArgT>(dense_op,
                                                                        extrema->key_min,
                                                                        range,
                                                                        preserved_keys,
                                                                        counted_keys,
                                                                        counted_rep_args,
                                                                        key_type,
                                                                        check_count_overflow,
                                                                        stream,
                                                                        mr)
               : group_join_dense_inner<KeyT, uint32_t, MatchedT, ArgT>(dense_op,
                                                                        extrema->key_min,
                                                                        range,
                                                                        preserved_keys,
                                                                        counted_keys,
                                                                        counted_rep_args,
                                                                        key_type,
                                                                        check_count_overflow,
                                                                        stream,
                                                                        mr);
    };
    auto const dispatch_matched = [&](auto key_tag, auto arg_tag) {
      return matched_wide ? dispatch_arg(key_tag, uint64_t{}, arg_tag)
                          : dispatch_arg(key_tag, uint32_t{}, arg_tag);
    };
    auto const dispatch_key = [&](auto key_tag) {
      // COUNT takes no argument column; the int32 tag instantiation is arbitrary there.
      bool const arg_is_64 =
        !counted_rep_args.empty() && counted_rep_args[0].type().id() == cudf::type_id::INT64;
      return arg_is_64 ? dispatch_matched(key_tag, int64_t{})
                       : dispatch_matched(key_tag, int32_t{});
    };
    switch (key_type.id()) {
      case cudf::type_id::INT32: raw = dispatch_key(int32_t{}); break;
      case cudf::type_id::INT64: raw = dispatch_key(int64_t{}); break;
      default:
        throw sirius::internal_exception(
          "group_join: unsupported key column type {} (expected INT32/INT64)",
          static_cast<int32_t>(key_type.id()));
    }
  } else {
    _last_strategy = strategy::SPARSE;
    SIRIUS_LOG_INFO(
      "[group_join] {} {} sparse path: state bytes {}, input bytes {}, budget {}, argument "
      "nulls {}",
      form_name(_spec.form),
      agg_op_name(slot.op),
      state_bytes,
      input_logical_bytes,
      _spec.max_state_bytes,
      argument_null_values);

    // DIRECT includes NULL keys as a real group; the join forms keep null-unequal semantics.
    auto const key_policy = is_direct ? cudf::null_policy::INCLUDE : cudf::null_policy::EXCLUDE;
    std::vector<std::unique_ptr<cudf::table>> counted_partials;
    for (std::size_t i = 0; i < counted_keys.size(); ++i) {
      if (counted_keys[i].size() == 0) { continue; }
      counted_partials.push_back(
        sparse_partial_value(counted_keys[i],
                             needs_argument ? &counted_rep_args[i] : nullptr,
                             slot.op,
                             key_policy,
                             stream,
                             mr));
    }
    auto counted_agg = sparse_merge_value_partials(
      std::move(counted_partials), key_type, slot.op, key_policy, stream, mr);

    if (is_direct) {
      raw = std::move(counted_agg);
    } else {
      // Preserved side: distinct keys with their multiplicity, exactly as the count path.
      std::vector<std::unique_ptr<cudf::table>> preserved_partials;
      for (auto const& col : preserved_keys) {
        if (col.size() == 0) { continue; }
        preserved_partials.push_back(
          sparse_partial_count(col, col, cudf::null_policy::INCLUDE, stream, mr));
      }
      auto preserved_agg =
        sparse_merge_partials(std::move(preserved_partials), key_type, stream, mr);

      // INNER form: a key joins iff it appears on both sides, which realizes the matched > 0
      // emit filter structurally (every counted-partial key has at least one row).
      auto const preserved_key_view      = cudf::table_view({preserved_agg->view().column(0)});
      auto const counted_key_view        = cudf::table_view({counted_agg->view().column(0)});
      auto [left_indices, right_indices] = cudf::inner_join(
        preserved_key_view, counted_key_view, cudf::null_equality::UNEQUAL, stream, mr);

      auto keys_out = cudf::gather(preserved_key_view,
                                   gather_map_view(*left_indices),
                                   cudf::out_of_bounds_policy::DONT_CHECK,
                                   stream,
                                   mr);
      auto presence = cudf::gather(cudf::table_view({preserved_agg->view().column(1)}),
                                   gather_map_view(*left_indices),
                                   cudf::out_of_bounds_policy::DONT_CHECK,
                                   stream,
                                   mr);
      std::vector<cudf::column_view> counted_value_views;
      for (cudf::size_type c = 1; c < counted_agg->view().num_columns(); ++c) {
        counted_value_views.push_back(counted_agg->view().column(c));
      }
      auto matched = cudf::gather(cudf::table_view(counted_value_views),
                                  gather_map_view(*right_indices),
                                  cudf::out_of_bounds_policy::DONT_CHECK,
                                  stream,
                                  mr);
      left_indices.reset();
      right_indices.reset();
      preserved_agg.reset();
      counted_agg.reset();

      auto matched_cols = matched->release();
      std::vector<std::unique_ptr<cudf::column>> columns;
      columns.push_back(std::move(keys_out->release()[0]));
      switch (slot.op) {
        case agg_op::COUNT_STAR:
        case agg_op::COUNT_VALID: {
          if (count_product_needs_validation(preserved_rows, counted_rows, false)) {
            throw_if_count_product_overflows(
              presence->view().column(0), matched_cols[0]->view(), stream, mr);
          }
          columns.push_back(cudf::binary_operation(presence->view().column(0),
                                                   matched_cols[0]->view(),
                                                   cudf::binary_operator::MUL,
                                                   cudf::data_type{cudf::type_id::INT64},
                                                   stream,
                                                   mr));
          break;
        }
        case agg_op::SUM:
          // Yan-Larson scaling; a NULL sum (all-NULL-argument group) stays NULL through the
          // multiply.
          columns.push_back(cudf::binary_operation(presence->view().column(0),
                                                   matched_cols[0]->view(),
                                                   cudf::binary_operator::MUL,
                                                   cudf::data_type{cudf::type_id::INT64},
                                                   stream,
                                                   mr));
          break;
        case agg_op::MIN:
        case agg_op::MAX:
          // Duplicate-agnostic: no scaling.
          columns.push_back(std::move(matched_cols[0]));
          break;
        case agg_op::AVG:
          // Presence cancels in numerator and divisor.
          columns.push_back(std::move(matched_cols[0]));
          columns.push_back(std::move(matched_cols[1]));
          break;
      }
      presence.reset();
      raw = std::make_unique<cudf::table>(std::move(columns));
    }
  }

  // Shared finalize: cast the raw INT64 aggregate (and divide for AVG) into the declared type.
  auto raw_cols = raw->release();
  std::unique_ptr<cudf::column> divisor;
  if (slot.op == agg_op::AVG) { divisor = std::move(raw_cols[2]); }
  auto final_value = finalize_value_column(
    std::move(raw_cols[1]), std::move(divisor), slot.op, out_type, arg_type, stream, mr);
  std::vector<std::unique_ptr<cudf::column>> output_columns;
  output_columns.push_back(std::move(raw_cols[0]));
  output_columns.push_back(std::move(final_value));
  return std::make_unique<cudf::table>(std::move(output_columns));
}

}  // namespace sirius::op
