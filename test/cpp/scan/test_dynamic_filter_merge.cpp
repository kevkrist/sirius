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

/**
 * @file test_dynamic_filter_merge.cpp
 * @brief Tests for sirius::op::scan::merge_dynamic_filters_into_ast — the helper that AND-merges
 *        AST-capable dynamic filters into a parquet reader's filter tree, resolving consumer
 *        column indices through scan_plan and skipping hive-partition columns.
 */

#include <cudf/aggregation.hpp>
#include <cudf/ast/expressions.hpp>
#include <cudf/column/column_factories.hpp>
#include <cudf/filling.hpp>
#include <cudf/null_mask.hpp>
#include <cudf/reduction.hpp>
#include <cudf/scalar/scalar.hpp>
#include <cudf/table/table.hpp>
#include <cudf/utilities/default_stream.hpp>
#include <cudf/utilities/memory_resource.hpp>

#include <rmm/device_buffer.hpp>
#include <rmm/device_uvector.hpp>

#include <cuda_runtime.h>

#include <catch.hpp>
#include <op/dynamic_filter/dynamic_filter_mask_ops.hpp>
#include <op/dynamic_filter/sirius_dynamic_filter.hpp>
#include <op/scan/dynamic_filter_merge.hpp>
#include <op/scan/scan_plan.hpp>

#include <algorithm>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

using sirius::op::sirius_dynamic_filter;
using sirius::op::sirius_dynamic_filter_kind;
using sirius::op::sirius_dynamic_filter_set;
using sirius::op::sirius_dynamic_zone_map_filter;
using sirius::op::zone_map_entry;
using sirius::op::scan::dynamic_filter_apply_mode;
using sirius::op::scan::merge_dynamic_filters_into_ast;
using sirius::op::scan::scan_plan;

namespace {

std::unique_ptr<cudf::scalar> make_int32_scalar(int32_t v)
{
  return std::make_unique<cudf::numeric_scalar<int32_t>>(
    v, true, cudf::get_default_stream(), cudf::get_current_device_resource_ref());
}

std::shared_ptr<sirius_dynamic_zone_map_filter> make_zone_map(int32_t lo, int32_t hi)
{
  std::vector<zone_map_entry> zones;
  zones.push_back({make_int32_scalar(lo), make_int32_scalar(hi)});
  return std::make_shared<sirius_dynamic_zone_map_filter>(std::move(zones));
}

/// Zone-map with explicit boundary inclusivity, so the exclusive GREATER / LESS lowering branches
/// (sirius_dynamic_filter.cpp) get exercised — the default helper above only builds inclusive
/// bounds.
std::shared_ptr<sirius_dynamic_zone_map_filter> make_zone_map(int32_t lo,
                                                              int32_t hi,
                                                              bool inclusive_min,
                                                              bool inclusive_max)
{
  std::vector<zone_map_entry> zones;
  zones.push_back({make_int32_scalar(lo), make_int32_scalar(hi)});
  return std::make_shared<sirius_dynamic_zone_map_filter>(
    std::move(zones), inclusive_min, inclusive_max);
}

/// Build a minimal scan_plan with one DATA column at consumer index @p col_idx named @p name.
scan_plan make_data_only_plan(std::size_t col_idx, std::string name)
{
  scan_plan plan;
  plan.data_columns.push_back({/*primary_idx=*/col_idx, std::move(name)});
  // output_layout must have enough entries to cover col_idx.
  plan.output_layout.resize(col_idx + 1, {scan_plan::output_entry::DATA, 0});
  plan.output_layout[col_idx] = {scan_plan::output_entry::DATA, 0};
  return plan;
}

/// Build a plan where the column at @p col_idx is a hive partition (not in the parquet file).
/// The merge function skips partition columns at the @c output_entry::source check, so we don't
/// need to populate @c partition_columns with a real type.
scan_plan make_partition_plan(std::size_t col_idx)
{
  scan_plan plan;
  plan.output_layout.resize(col_idx + 1, {scan_plan::output_entry::DATA, 0});
  plan.output_layout[col_idx] = {scan_plan::output_entry::PARTITION, 0};
  return plan;
}

/// Filter that inherits the base but NOT the AST mixin — exercises the "lacks capability" skip.
class stub_runtime_only_filter final : public sirius_dynamic_filter {
 public:
  [[nodiscard]] sirius_dynamic_filter_kind kind() const override
  {
    return sirius_dynamic_filter_kind::ZONE_MAP;
  }
};

}  // namespace

TEST_CASE("merge_dynamic_filters_into_ast returns existing_root unchanged for an empty set",
          "[dynamic_filter][scan_merge]")
{
  sirius_dynamic_filter_set filters;  // empty
  auto plan = make_data_only_plan(0, "id");

  cudf::ast::tree tree;
  auto const& base = tree.emplace<cudf::ast::column_name_reference>("static_root_placeholder");

  auto const* root = merge_dynamic_filters_into_ast(tree, &base, filters, plan);

  REQUIRE(root == &base);
  REQUIRE(tree.size() == 1);
}

TEST_CASE("merge_dynamic_filters_into_ast returns nullptr when existing_root is null and set empty",
          "[dynamic_filter][scan_merge]")
{
  sirius_dynamic_filter_set filters;
  auto plan = make_data_only_plan(0, "id");

  cudf::ast::tree tree;
  auto const* root = merge_dynamic_filters_into_ast(tree, nullptr, filters, plan);

  REQUIRE(root == nullptr);
  REQUIRE(tree.size() == 0);
}

TEST_CASE("merge_dynamic_filters_into_ast builds a dynamic-only tree from one filter",
          "[dynamic_filter][scan_merge]")
{
  sirius_dynamic_filter_set filters;
  filters.push_filter(0, make_zone_map(100, 200));
  auto plan = make_data_only_plan(0, "id");

  cudf::ast::tree tree;
  auto const* root = merge_dynamic_filters_into_ast(tree, nullptr, filters, plan);

  REQUIRE(root != nullptr);
  // 1 column_name_reference + 2 literals + 2 comparisons + 1 AND for the single-zone filter.
  REQUIRE(tree.size() == 6);
  REQUIRE(root == &tree.back());
}

TEST_CASE("merge_dynamic_filters_into_ast AND-conjoins dynamic fragment with existing_root",
          "[dynamic_filter][scan_merge]")
{
  sirius_dynamic_filter_set filters;
  filters.push_filter(0, make_zone_map(100, 200));
  auto plan = make_data_only_plan(0, "id");

  cudf::ast::tree tree;
  auto const& base = tree.emplace<cudf::ast::column_name_reference>("static_root_placeholder");
  auto const* root = merge_dynamic_filters_into_ast(tree, &base, filters, plan);

  REQUIRE(root != nullptr);
  REQUIRE(root != &base);
  // 1 base + 1 col_ref + 2 lit + 2 op + 1 AND (filter) + 1 AND (merge with base) = 8.
  REQUIRE(tree.size() == 8);
  REQUIRE(root == &tree.back());
}

TEST_CASE("merge_dynamic_filters_into_ast skips hive-partition columns",
          "[dynamic_filter][scan_merge]")
{
  sirius_dynamic_filter_set filters;
  filters.push_filter(0, make_zone_map(100, 200));
  auto plan = make_partition_plan(0);  // col 0 is a hive partition

  cudf::ast::tree tree;
  auto const* root = merge_dynamic_filters_into_ast(tree, nullptr, filters, plan);

  REQUIRE(root == nullptr);  // nothing contributed
  REQUIRE(tree.size() == 0);
}

TEST_CASE("merge_dynamic_filters_into_ast skips filters lacking the AST capability",
          "[dynamic_filter][scan_merge]")
{
  sirius_dynamic_filter_set filters;
  filters.push_filter(0, std::make_shared<stub_runtime_only_filter>());
  auto plan = make_data_only_plan(0, "id");

  cudf::ast::tree tree;
  auto const* root = merge_dynamic_filters_into_ast(tree, nullptr, filters, plan);

  REQUIRE(root == nullptr);
  REQUIRE(tree.size() == 0);
}

TEST_CASE("merge_dynamic_filters_into_ast AND-conjoins multiple filters across columns",
          "[dynamic_filter][scan_merge]")
{
  sirius_dynamic_filter_set filters;
  filters.push_filter(0, make_zone_map(100, 200));
  filters.push_filter(1, make_zone_map(-5, 5));

  scan_plan plan;
  plan.data_columns.push_back({0, "id"});
  plan.data_columns.push_back({1, "value"});
  plan.output_layout = {{scan_plan::output_entry::DATA, 0}, {scan_plan::output_entry::DATA, 1}};

  cudf::ast::tree tree;
  auto const* root = merge_dynamic_filters_into_ast(tree, nullptr, filters, plan);

  REQUIRE(root != nullptr);
  REQUIRE(root == &tree.back());
  // 2 cols × (1 col_ref + 2 lit + 2 op + 1 AND) + 1 cross-col AND = 13.
  REQUIRE(tree.size() == 13);
}

TEST_CASE("merge_dynamic_filters_into_ast AND-conjoins multiple filters on the same column",
          "[dynamic_filter][scan_merge]")
{
  sirius_dynamic_filter_set filters;
  filters.push_filter(0, make_zone_map(100, 200));
  filters.push_filter(0, make_zone_map(150, 175));
  auto plan = make_data_only_plan(0, "id");

  cudf::ast::tree tree;
  auto const* root = merge_dynamic_filters_into_ast(tree, nullptr, filters, plan);

  REQUIRE(root != nullptr);
  REQUIRE(root == &tree.back());
  // 2 filters × (1 col_ref + 2 lit + 2 op + 1 AND) + 1 AND-merging-the-two = 13.
  REQUIRE(tree.size() == 13);
}

TEST_CASE("merge_dynamic_filters_into_ast ignores out-of-range col_idx defensively",
          "[dynamic_filter][scan_merge]")
{
  sirius_dynamic_filter_set filters;
  filters.push_filter(99, make_zone_map(0, 10));  // col 99 doesn't exist in plan
  auto plan = make_data_only_plan(0, "id");

  cudf::ast::tree tree;
  auto const* root = merge_dynamic_filters_into_ast(tree, nullptr, filters, plan);

  REQUIRE(root == nullptr);
  REQUIRE(tree.size() == 0);
}

//===----------------------------------------------------------------------===//
// apply_dynamic_filters_to_view — runtime apply (post-decode / cached)
//===----------------------------------------------------------------------===//

namespace {
/// One INT32 column [0, 1, ..., size-1] wrapped in a single-column table.
std::unique_ptr<cudf::table> make_sequence_table(int32_t size, rmm::cuda_stream_view stream)
{
  auto col = cudf::sequence(size,
                            cudf::numeric_scalar<int32_t>(0, true, stream),
                            cudf::numeric_scalar<int32_t>(1, true, stream),
                            stream);
  std::vector<std::unique_ptr<cudf::column>> cols;
  cols.push_back(std::move(col));
  return std::make_unique<cudf::table>(std::move(cols));
}

/// Copy an INT32 column's values to host. apply_boolean_mask gathers survivors in order with no
/// nulls, so the result is directly comparable to an expected sequence.
std::vector<int32_t> to_host_int32(cudf::column_view const& col, rmm::cuda_stream_view stream)
{
  std::vector<int32_t> host(static_cast<std::size_t>(col.size()));
  cudaMemcpyAsync(host.data(),
                  col.data<int32_t>(),
                  host.size() * sizeof(int32_t),
                  cudaMemcpyDeviceToHost,
                  stream.value());
  stream.synchronize();
  return host;
}

/// Build a zone-map-*only* filter (no membership) the way the hash-join producer does: reduce the
/// build column's min/max into device scalars on `stream`. A large build whose membership structure
/// doesn't fit L2 emits exactly this — the path whose missing build-stream sync produced
/// cross-stream false negatives (Q8/Q9/Q17 with enable_dynamic_zone_map_filter). The producer's
/// drain-before-publish is the structural guard; this exercises the zone-map-only correctness
/// (bounds, column, superset).
std::shared_ptr<sirius_dynamic_zone_map_filter> make_zone_map_from_reduce(
  cudf::column_view const& build_col, rmm::cuda_stream_view stream)
{
  auto mr    = cudf::get_current_device_resource_ref();
  auto min_s = cudf::reduce(build_col,
                            *cudf::make_min_aggregation<cudf::reduce_aggregation>(),
                            build_col.type(),
                            stream,
                            mr);
  auto max_s = cudf::reduce(build_col,
                            *cudf::make_max_aggregation<cudf::reduce_aggregation>(),
                            build_col.type(),
                            stream,
                            mr);
  std::vector<zone_map_entry> zones;
  zones.push_back({std::move(min_s), std::move(max_s)});
  return std::make_shared<sirius_dynamic_zone_map_filter>(std::move(zones));
}
}  // namespace

TEST_CASE("apply_dynamic_filters_to_view drops rows outside the zone",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  auto table  = make_sequence_table(10, stream);  // [0..9]

  sirius_dynamic_filter_set filters;
  filters.push_filter(0, make_zone_map(3, 6));  // inclusive [3,6] keeps 3,4,5,6

  auto out = sirius::op::scan::apply_dynamic_filters_to_view(table->view(), filters, stream);
  stream.synchronize();

  REQUIRE(out != nullptr);
  REQUIRE(out->num_columns() == 1);
  REQUIRE(out->num_rows() == 4);
  REQUIRE(to_host_int32(out->view().column(0), stream) == std::vector<int32_t>{3, 4, 5, 6});
  REQUIRE(table->num_rows() == 10);  // input untouched
}

TEST_CASE("apply_dynamic_filters_to_view honors an exclusive upper bound [lo, hi)",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  auto table  = make_sequence_table(10, stream);  // [0..9]

  sirius_dynamic_filter_set filters;
  // [3,6): inclusive_min, exclusive_max -> GREATER_EQUAL(3) AND LESS(6) -> {3,4,5}
  filters.push_filter(0, make_zone_map(3, 6, /*inclusive_min=*/true, /*inclusive_max=*/false));

  auto out = sirius::op::scan::apply_dynamic_filters_to_view(table->view(), filters, stream);
  stream.synchronize();

  REQUIRE(out != nullptr);
  REQUIRE(to_host_int32(out->view().column(0), stream) == std::vector<int32_t>{3, 4, 5});
}

TEST_CASE("apply_dynamic_filters_to_view honors an exclusive lower bound (lo, hi]",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  auto table  = make_sequence_table(10, stream);  // [0..9]

  sirius_dynamic_filter_set filters;
  // (3,6]: exclusive_min, inclusive_max -> GREATER(3) AND LESS_EQUAL(6) -> {4,5,6}
  filters.push_filter(0, make_zone_map(3, 6, /*inclusive_min=*/false, /*inclusive_max=*/true));

  auto out = sirius::op::scan::apply_dynamic_filters_to_view(table->view(), filters, stream);
  stream.synchronize();

  REQUIRE(out != nullptr);
  REQUIRE(to_host_int32(out->view().column(0), stream) == std::vector<int32_t>{4, 5, 6});
}

TEST_CASE("zone-map-only filter from a device reduce keeps a correct superset (no false negative)",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();

  // Build keys spanning [100, 200] — the producer reduces these to min=100, max=200 and, with no
  // membership filter, publishes a zone-map alone. Probe [0..299]: only [100..200] can possibly
  // join, and crucially every value a build key could equal must survive (no false negative).
  auto build = cudf::sequence(101,
                              cudf::numeric_scalar<int32_t>(100, true, stream),
                              cudf::numeric_scalar<int32_t>(1, true, stream),
                              stream);
  sirius_dynamic_filter_set filters;
  filters.push_filter(0, make_zone_map_from_reduce(build->view(), stream));

  auto probe = make_sequence_table(300, stream);  // [0..299]
  auto out   = sirius::op::scan::apply_dynamic_filters_to_view(probe->view(), filters, stream);
  stream.synchronize();

  REQUIRE(out != nullptr);
  std::vector<int32_t> expected(101);
  std::iota(expected.begin(), expected.end(), 100);  // {100, 101, ..., 200}, inclusive bounds
  REQUIRE(to_host_int32(out->view().column(0), stream) == expected);
}

TEST_CASE("apply_dynamic_filters_to_view returns nullptr for an empty channel",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  auto table  = make_sequence_table(10, stream);

  sirius_dynamic_filter_set filters;  // empty

  auto out = sirius::op::scan::apply_dynamic_filters_to_view(table->view(), filters, stream);
  stream.synchronize();

  REQUIRE(out == nullptr);
  REQUIRE(table->num_rows() == 10);  // input untouched
}

TEST_CASE("sirius_dynamic_filter_set ignore_columns drops filters for ignored columns",
          "[dynamic_filter][scan_merge]")
{
  // Wiring-time partition skip: a consumer marks its hive-partition output columns so the producer
  // never publishes a filter the post-decode apply would have to skip.
  sirius_dynamic_filter_set filters;
  filters.ignore_columns({0});                  // output col 0 is a hive partition
  filters.push_filter(0, make_zone_map(3, 6));  // dropped — column 0 is ignored
  filters.push_filter(1, make_zone_map(3, 6));  // kept — column 1 is a data column

  REQUIRE(filters.filters_for_column(0).empty());
  REQUIRE(filters.filters_for_column(1).size() == 1);
  REQUIRE(filters.filter_count() == 1);
}

TEST_CASE("apply_dynamic_filters_to_view AND-conjoins multiple zone filters on a column",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  auto table  = make_sequence_table(10, stream);  // [0..9]

  sirius_dynamic_filter_set filters;
  filters.push_filter(0, make_zone_map(2, 8));  // keeps 2..8
  filters.push_filter(0, make_zone_map(5, 9));  // AND keeps 5..9 → intersection 5..8

  auto out = sirius::op::scan::apply_dynamic_filters_to_view(table->view(), filters, stream);
  stream.synchronize();

  REQUIRE(out != nullptr);
  REQUIRE(out->num_rows() == 4);  // 5,6,7,8
  REQUIRE(table->num_rows() == 10);
}

//===----------------------------------------------------------------------===//
// Membership filters — IN-list (exact) and Bloom (no false negatives)
//===----------------------------------------------------------------------===//

namespace {
/// One INT64 sequence column [0, 1, ..., size-1] in a single-column table.
std::unique_ptr<cudf::table> make_int64_sequence_table(int64_t size, rmm::cuda_stream_view stream)
{
  std::vector<std::unique_ptr<cudf::column>> cols;
  cols.push_back(cudf::sequence(static_cast<cudf::size_type>(size),
                                cudf::numeric_scalar<int64_t>(0, true, stream),
                                cudf::numeric_scalar<int64_t>(1, true, stream),
                                stream));
  return std::make_unique<cudf::table>(std::move(cols));
}

/// Single-column table from explicit host values — for keys/probes that aren't arithmetic
/// sequences (e.g. ones containing the type-min sentinel).
template <class T>
std::unique_ptr<cudf::table> make_values_table(std::vector<T> const& values,
                                               cudf::data_type dtype,
                                               rmm::cuda_stream_view stream)
{
  auto col = cudf::make_numeric_column(
    dtype, static_cast<cudf::size_type>(values.size()), cudf::mask_state::UNALLOCATED, stream);
  cudaMemcpyAsync(col->mutable_view().data<T>(),
                  values.data(),
                  values.size() * sizeof(T),
                  cudaMemcpyHostToDevice,
                  stream.value());
  std::vector<std::unique_ptr<cudf::column>> cols;
  cols.push_back(std::move(col));
  return std::make_unique<cudf::table>(std::move(cols));
}

/// Copy an INT64 column's values to host (companion to to_host_int32).
std::vector<int64_t> to_host_int64(cudf::column_view const& col, rmm::cuda_stream_view stream)
{
  std::vector<int64_t> host(static_cast<std::size_t>(col.size()));
  cudaMemcpyAsync(host.data(),
                  col.data<int64_t>(),
                  host.size() * sizeof(int64_t),
                  cudaMemcpyDeviceToHost,
                  stream.value());
  stream.synchronize();
  return host;
}
}  // namespace

TEST_CASE("sirius_dynamic_in_list_filter keeps exactly the rows whose key is a build key",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  // Build key set {0,1,2,3,4}; probe table [0..9]. Exact membership keeps the first five.
  auto keys = cudf::sequence(5,
                             cudf::numeric_scalar<int64_t>(0, true, stream),
                             cudf::numeric_scalar<int64_t>(1, true, stream),
                             stream);
  sirius_dynamic_filter_set filters;
  filters.push_filter(0,
                      std::make_shared<sirius::op::sirius_dynamic_in_list_filter>(
                        keys->view(), stream, cudf::get_current_device_resource_ref()));

  auto table = make_int64_sequence_table(10, stream);
  auto out   = sirius::op::scan::apply_dynamic_filters_to_view(table->view(), filters, stream);
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(out->num_rows() == 5);
  REQUIRE(table->num_rows() == 10);
}

TEST_CASE("sirius_dynamic_in_list_filter INT64 path uses a persistent set and probes repeatedly",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  auto keys   = cudf::sequence(5,
                             cudf::numeric_scalar<int64_t>(0, true, stream),
                             cudf::numeric_scalar<int64_t>(1, true, stream),
                             stream);
  auto filter = std::make_shared<sirius::op::sirius_dynamic_in_list_filter>(
    keys->view(), stream, cudf::get_current_device_resource_ref());
  REQUIRE(filter->has_persistent_set());  // INT64, non-null keys → fast path

  sirius_dynamic_filter_set filters;
  filters.push_filter(0, filter);

  // Two applies against the same persistent structure (the per-split pattern).
  for (int i = 0; i < 2; ++i) {
    auto table = make_int64_sequence_table(10, stream);
    auto out   = sirius::op::scan::apply_dynamic_filters_to_view(table->view(), filters, stream);
    stream.synchronize();
    REQUIRE(out != nullptr);
    REQUIRE(out->num_rows() == 5);  // exact membership: 0..4
    REQUIRE(table->num_rows() == 10);
  }
}

TEST_CASE("sirius_dynamic_bloom_filter never drops a true match (no false negatives)",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  auto keys   = cudf::sequence(5,
                             cudf::numeric_scalar<int64_t>(0, true, stream),
                             cudf::numeric_scalar<int64_t>(1, true, stream),
                             stream);
  sirius_dynamic_filter_set filters;
  filters.push_filter(0,
                      std::make_shared<sirius::op::sirius_dynamic_bloom_filter>(
                        keys->view(), stream, cudf::get_current_device_resource_ref()));

  auto table = make_int64_sequence_table(10, stream);
  auto out   = sirius::op::scan::apply_dynamic_filters_to_view(table->view(), filters, stream);
  stream.synchronize();
  // All five build keys are in the probe, so every one must survive (Bloom has no false negatives).
  // False positives may keep a few extras, so the surviving count is in [5, 10].
  REQUIRE(out != nullptr);
  REQUIRE(out->num_rows() >= 5);
  REQUIRE(out->num_rows() <= 10);
  REQUIRE(table->num_rows() == 10);
}

TEST_CASE("sirius_dynamic_in_list_filter supports INT32 keys exactly",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  // INT32 build key set {0,1,2,3,4}; INT32 probe [0..9]. Exact membership keeps exactly
  // {0,1,2,3,4}.
  auto keys   = cudf::sequence(5,
                             cudf::numeric_scalar<int32_t>(0, true, stream),
                             cudf::numeric_scalar<int32_t>(1, true, stream),
                             stream);
  auto filter = std::make_shared<sirius::op::sirius_dynamic_in_list_filter>(
    keys->view(), stream, cudf::get_current_device_resource_ref());
  REQUIRE(filter->has_persistent_set());  // INT32, non-null keys → persistent-set fast path

  sirius_dynamic_filter_set filters;
  filters.push_filter(0, filter);

  auto table = make_sequence_table(10, stream);  // INT32 [0..9]
  auto out   = sirius::op::scan::apply_dynamic_filters_to_view(table->view(), filters, stream);
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(to_host_int32(out->view().column(0), stream) == std::vector<int32_t>{0, 1, 2, 3, 4});
}

TEST_CASE("sirius_dynamic_bloom_filter supports INT32 keys with no false negatives",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  auto keys   = cudf::sequence(5,
                             cudf::numeric_scalar<int32_t>(0, true, stream),
                             cudf::numeric_scalar<int32_t>(1, true, stream),
                             stream);
  sirius_dynamic_filter_set filters;
  filters.push_filter(0,
                      std::make_shared<sirius::op::sirius_dynamic_bloom_filter>(
                        keys->view(), stream, cudf::get_current_device_resource_ref()));

  auto table = make_sequence_table(10, stream);  // INT32 [0..9]
  auto out   = sirius::op::scan::apply_dynamic_filters_to_view(table->view(), filters, stream);
  stream.synchronize();
  REQUIRE(out != nullptr);
  // Build keys {0..4} all precede any false positive (which can only come from {5..9}), so the
  // first five survivors must be exactly the keys — proving no false negative.
  auto const survivors = to_host_int32(out->view().column(0), stream);
  REQUIRE(survivors.size() >= 5);
  REQUIRE(survivors.size() <= 10);
  REQUIRE(std::vector<int32_t>(survivors.begin(), survivors.begin() + 5) ==
          std::vector<int32_t>{0, 1, 2, 3, 4});
}

TEST_CASE("sirius_dynamic_bloom_filter excludes null build slots from the key set",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();

  // Build keys [0,1,2,3,4,999] with the 999 slot nulled: only {0..4} may enter the set.
  std::vector<int64_t> const key_values{0, 1, 2, 3, 4, 999};
  auto keys = cudf::make_numeric_column(
    cudf::data_type{cudf::type_id::INT64}, 6, cudf::mask_state::ALL_VALID, stream);
  cudaMemcpyAsync(keys->mutable_view().data<int64_t>(),
                  key_values.data(),
                  key_values.size() * sizeof(int64_t),
                  cudaMemcpyHostToDevice,
                  stream.value());
  cudf::set_null_mask(keys->mutable_view().null_mask(), 5, 6, false, stream);
  keys->set_null_count(1);

  // Reference filter over the same valid keys, built without nulls. Compaction is exact and the
  // hash policy deterministic, so the nullable build must produce a bit-identical filter.
  auto clean_keys = cudf::sequence(5,
                                   cudf::numeric_scalar<int64_t>(0, true, stream),
                                   cudf::numeric_scalar<int64_t>(1, true, stream),
                                   stream);

  sirius_dynamic_filter_set nullable_channel;
  nullable_channel.push_filter(0,
                               std::make_shared<sirius::op::sirius_dynamic_bloom_filter>(
                                 keys->view(), stream, cudf::get_current_device_resource_ref()));
  sirius_dynamic_filter_set reference_channel;
  reference_channel.push_filter(
    0,
    std::make_shared<sirius::op::sirius_dynamic_bloom_filter>(
      clean_keys->view(), stream, cudf::get_current_device_resource_ref()));

  // Probe [0..9] plus 999 — the value present only at the null build slot.
  auto probe = make_values_table<int64_t>(
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 999}, cudf::data_type{cudf::type_id::INT64}, stream);
  auto out_nullable =
    sirius::op::scan::apply_dynamic_filters_to_view(probe->view(), nullable_channel, stream);
  auto out_reference =
    sirius::op::scan::apply_dynamic_filters_to_view(probe->view(), reference_channel, stream);
  stream.synchronize();
  REQUIRE(out_nullable != nullptr);
  REQUIRE(out_reference != nullptr);

  auto const survivors = to_host_int64(out_nullable->view().column(0), stream);
  // No false negatives: the five valid keys lead the probe and must all survive, in order.
  REQUIRE(survivors.size() >= 5);
  REQUIRE(std::vector<int64_t>(survivors.begin(), survivors.begin() + 5) ==
          std::vector<int64_t>{0, 1, 2, 3, 4});
  // Identical behavior to the clean build — this is the deterministic assertion: before the fix
  // the raw ingest added the null slot's payload, so 999 always survived the nullable filter.
  REQUIRE(survivors == to_host_int64(out_reference->view().column(0), stream));
}

TEST_CASE("sirius_dynamic_in_list_filter keeps a build key equal to the INT64 sentinel",
          "[dynamic_filter][scan_merge]")
{
  auto stream      = cudf::get_default_stream();
  auto const dtype = cudf::data_type{cudf::type_id::INT64};
  // Build keys include INT64_MIN — the cuco empty-slot sentinel that static_set never inserts.
  auto keys =
    make_values_table<int64_t>({std::numeric_limits<int64_t>::min(), 0, 1, 2}, dtype, stream);
  auto filter = std::make_shared<sirius::op::sirius_dynamic_in_list_filter>(
    keys->view().column(0), stream, cudf::get_current_device_resource_ref());
  REQUIRE(filter->has_persistent_set());  // stays on the exact IN-list path (no Bloom downgrade)

  sirius_dynamic_filter_set filters;
  filters.push_filter(0, filter);

  // Probe {INT64_MIN, 2, 7}: INT64_MIN and 2 are build keys and must survive; 7 must be dropped.
  auto probe =
    make_values_table<int64_t>({std::numeric_limits<int64_t>::min(), 2, 7}, dtype, stream);
  auto out = sirius::op::scan::apply_dynamic_filters_to_view(probe->view(), filters, stream);
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(to_host_int64(out->view().column(0), stream) ==
          std::vector<int64_t>{std::numeric_limits<int64_t>::min(), 2});
}

TEST_CASE("sirius_dynamic_in_list_filter keeps a build key equal to the INT32 sentinel",
          "[dynamic_filter][scan_merge]")
{
  auto stream      = cudf::get_default_stream();
  auto const dtype = cudf::data_type{cudf::type_id::INT32};
  auto keys =
    make_values_table<int32_t>({std::numeric_limits<int32_t>::min(), 0, 1, 2}, dtype, stream);
  auto filter = std::make_shared<sirius::op::sirius_dynamic_in_list_filter>(
    keys->view().column(0), stream, cudf::get_current_device_resource_ref());
  REQUIRE(filter->has_persistent_set());

  sirius_dynamic_filter_set filters;
  filters.push_filter(0, filter);

  auto probe =
    make_values_table<int32_t>({std::numeric_limits<int32_t>::min(), 2, 7}, dtype, stream);
  auto out = sirius::op::scan::apply_dynamic_filters_to_view(probe->view(), filters, stream);
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(to_host_int32(out->view().column(0), stream) ==
          std::vector<int32_t>{std::numeric_limits<int32_t>::min(), 2});
}

TEST_CASE("sirius_dynamic_small_in_list_filter keeps exactly the rows whose key is a build key",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  // Small build key set {0,1,2,3,4}; probe [0..9]. The brute-force scan keeps the first five.
  auto keys = cudf::sequence(5,
                             cudf::numeric_scalar<int64_t>(0, true, stream),
                             cudf::numeric_scalar<int64_t>(1, true, stream),
                             stream);
  sirius_dynamic_filter_set filters;
  filters.push_filter(0,
                      std::make_shared<sirius::op::sirius_dynamic_small_in_list_filter>(
                        keys->view(), stream, cudf::get_current_device_resource_ref()));

  auto table = make_int64_sequence_table(10, stream);
  auto out   = sirius::op::scan::apply_dynamic_filters_to_view(table->view(), filters, stream);
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(to_host_int64(out->view().column(0), stream) == std::vector<int64_t>{0, 1, 2, 3, 4});
  REQUIRE(table->num_rows() == 10);
}

TEST_CASE("sirius_dynamic_small_in_list_filter supports INT32 keys exactly",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  auto keys   = cudf::sequence(5,
                             cudf::numeric_scalar<int32_t>(0, true, stream),
                             cudf::numeric_scalar<int32_t>(1, true, stream),
                             stream);
  sirius_dynamic_filter_set filters;
  filters.push_filter(0,
                      std::make_shared<sirius::op::sirius_dynamic_small_in_list_filter>(
                        keys->view(), stream, cudf::get_current_device_resource_ref()));

  auto table = make_sequence_table(10, stream);  // INT32 [0..9]
  auto out   = sirius::op::scan::apply_dynamic_filters_to_view(table->view(), filters, stream);
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(to_host_int32(out->view().column(0), stream) == std::vector<int32_t>{0, 1, 2, 3, 4});
}

TEST_CASE("sirius_dynamic_small_in_list_filter matches a key equal to INT32_MIN (cuco's sentinel)",
          "[dynamic_filter][scan_merge]")
{
  auto stream      = cudf::get_default_stream();
  auto const dtype = cudf::data_type{cudf::type_id::INT32};
  // Single build key {INT32_MIN} — the value cuco::static_set reserves as its empty slot and never
  // stores. The brute-force scan has no reserved value, so INT32_MIN is a valid needle: this filter
  // prunes non-matches exactly, where sirius_dynamic_in_list_filter would (harmlessly) keep them.
  auto keys = make_values_table<int32_t>({std::numeric_limits<int32_t>::min()}, dtype, stream);
  sirius_dynamic_filter_set filters;
  filters.push_filter(0,
                      std::make_shared<sirius::op::sirius_dynamic_small_in_list_filter>(
                        keys->view().column(0), stream, cudf::get_current_device_resource_ref()));

  // Probe {INT32_MIN, INT32_MIN+1, INT32_MIN+2}; only the first is a build key.
  auto probe = make_values_table<int32_t>({std::numeric_limits<int32_t>::min(),
                                           std::numeric_limits<int32_t>::min() + 1,
                                           std::numeric_limits<int32_t>::min() + 2},
                                          dtype,
                                          stream);
  auto out   = sirius::op::scan::apply_dynamic_filters_to_view(probe->view(), filters, stream);
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(to_host_int32(out->view().column(0), stream) ==
          std::vector<int32_t>{std::numeric_limits<int32_t>::min()});
}

TEST_CASE("sirius_dynamic_small_in_list_filter: kind, size, capabilities, and supports gate",
          "[dynamic_filter][scan_merge]")
{
  auto stream   = cudf::get_default_stream();
  auto const mr = cudf::get_current_device_resource_ref();
  using F       = sirius::op::sirius_dynamic_small_in_list_filter;

  auto one_i32       = cudf::sequence(1,
                                cudf::numeric_scalar<int32_t>(0, true, stream),
                                cudf::numeric_scalar<int32_t>(1, true, stream),
                                stream);
  auto max_i32       = cudf::sequence(static_cast<cudf::size_type>(F::k_max_keys),
                                cudf::numeric_scalar<int32_t>(0, true, stream),
                                cudf::numeric_scalar<int32_t>(1, true, stream),
                                stream);
  auto oversized_i32 = cudf::sequence(static_cast<cudf::size_type>(F::k_max_keys + 1),
                                      cudf::numeric_scalar<int32_t>(0, true, stream),
                                      cudf::numeric_scalar<int32_t>(1, true, stream),
                                      stream);
  auto empty_i32     = cudf::make_empty_column(cudf::data_type{cudf::type_id::INT32});
  auto null_i32      = cudf::make_numeric_column(
    cudf::data_type{cudf::type_id::INT32}, 1, cudf::mask_state::ALL_NULL, stream);
  auto f64 =
    make_values_table<double>({0.0, 1.0, 2.0}, cudf::data_type{cudf::type_id::FLOAT64}, stream);

  // supports() gate: 1..k_max_keys keys, INT32/INT64, no nulls.
  REQUIRE(F::supports(one_i32->view()));
  REQUIRE(F::supports(max_i32->view()));
  REQUIRE_FALSE(F::supports(empty_i32->view()));
  REQUIRE_FALSE(F::supports(oversized_i32->view()));
  REQUIRE_FALSE(F::supports(f64->view().column(0)));
  REQUIRE_FALSE(F::supports(null_i32->view()));

  F f(max_i32->view(), stream, mr);
  stream.synchronize();
  REQUIRE(f.kind() == sirius_dynamic_filter_kind::IN_LIST);
  REQUIRE(f.size() == F::k_max_keys);
  REQUIRE(f.replica_count() == 1);  // source-device snapshot built in the constructor
  // Cast through the base pointer, exactly as the consumer-side merge does: the filter advertises
  // the runtime-mask capability but not AST lowering, keeping it out of the parquet row-group path.
  sirius_dynamic_filter const* base = &f;
  REQUIRE(dynamic_cast<sirius::op::sirius_mask_applicable const*>(base) != nullptr);
  REQUIRE(dynamic_cast<sirius::op::sirius_ast_lowerable const*>(base) == nullptr);
}

//===----------------------------------------------------------------------===//
// apply_dynamic_filters_to_view — view-based core (pinned cached path)
//===----------------------------------------------------------------------===//

namespace {
/// IN-list filter keeping INT64 keys [0, count).
std::shared_ptr<sirius::op::sirius_dynamic_in_list_filter> make_in_list_prefix(
  int64_t count, rmm::cuda_stream_view stream)
{
  auto keys = cudf::sequence(static_cast<cudf::size_type>(count),
                             cudf::numeric_scalar<int64_t>(0, true, stream),
                             cudf::numeric_scalar<int64_t>(1, true, stream),
                             stream);
  return std::make_shared<sirius::op::sirius_dynamic_in_list_filter>(
    keys->view(), stream, cudf::get_current_device_resource_ref());
}

/// Membership filter that counts compute_mask calls and delegates to a wrapped IN-list filter.
/// Makes "the gate did not re-run this filter" directly observable.
class counting_in_list_filter final : public sirius_dynamic_filter,
                                      public sirius::op::sirius_mask_applicable {
 public:
  explicit counting_in_list_filter(std::shared_ptr<sirius::op::sirius_dynamic_in_list_filter> inner)
    : _inner(std::move(inner))
  {
  }

  [[nodiscard]] sirius_dynamic_filter_kind kind() const override { return _inner->kind(); }

  [[nodiscard]] bool is_available_on_device(int device_id) const noexcept override
  {
    return _inner->is_available_on_device(device_id);
  }

  [[nodiscard]] std::unique_ptr<cudf::column> compute_mask(
    cudf::column_view const& probe,
    int device_id,
    rmm::cuda_stream_view stream,
    rmm::device_async_resource_ref mr) const override
  {
    ++_mask_calls;
    return _inner->compute_mask(probe, device_id, stream, mr);
  }

  [[nodiscard]] int mask_calls() const noexcept { return _mask_calls; }

 private:
  std::shared_ptr<sirius::op::sirius_dynamic_in_list_filter> _inner;
  mutable int _mask_calls = 0;
};
}  // namespace

TEST_CASE("apply_dynamic_filters_to_view returns nullptr when no filter contributes",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  auto table  = make_int64_sequence_table(10, stream);

  sirius_dynamic_filter_set filters;  // empty

  auto out = sirius::op::scan::apply_dynamic_filters_to_view(table->view(), filters, stream);
  REQUIRE(out == nullptr);
  REQUIRE(table->num_rows() == 10);  // input untouched, still usable
}

TEST_CASE("apply_dynamic_filters_to_view gathers survivors without consuming the input",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  auto table  = make_int64_sequence_table(10, stream);  // [0..9]

  sirius_dynamic_filter_set filters;
  filters.push_filter(0, make_in_list_prefix(2, stream));  // keeps 0,1

  auto out = sirius::op::scan::apply_dynamic_filters_to_view(table->view(), filters, stream);
  stream.synchronize();

  REQUIRE(out != nullptr);
  REQUIRE(out->num_rows() == 2);
  REQUIRE(table->num_rows() == 10);  // the view's backing table is intact
}

//===----------------------------------------------------------------------===//
// dynamic_filter_gate — selectivity gating and re-arm on new publishes
//===----------------------------------------------------------------------===//

TEST_CASE("dynamic_filter_gate is not applicable before any filter publishes",
          "[dynamic_filter][scan_merge]")
{
  sirius::op::scan::dynamic_filter_gate gate;
  sirius_dynamic_filter_set filters;  // empty
  REQUIRE_FALSE(gate.applicable(filters));
}

TEST_CASE("dynamic_filter_gate disables after an unselective first split",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  sirius::op::scan::dynamic_filter_gate gate;

  sirius_dynamic_filter_set filters;
  filters.push_filter(0, make_in_list_prefix(10, stream));  // covers [0..9] — keeps 100%
  REQUIRE(gate.applicable(filters));

  auto table = make_int64_sequence_table(10, stream);
  auto out   = sirius::op::scan::apply_dynamic_filters_gated_view(
    table->view(), filters, gate, stream, dynamic_filter_apply_mode::include_ast_row_masks);
  stream.synchronize();

  REQUIRE(out != nullptr);                  // a mask was computed (keeps everything)
  REQUIRE(out->num_rows() == 10);           // ... so kept ratio is 1.0
  REQUIRE_FALSE(gate.applicable(filters));  // gate disabled for subsequent splits

  auto second = sirius::op::scan::apply_dynamic_filters_gated_view(
    table->view(), filters, gate, stream, dynamic_filter_apply_mode::include_ast_row_masks);
  REQUIRE(second == nullptr);  // gated out — no work
}

TEST_CASE("dynamic_filter_gate ignores a device with no local replica",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  sirius::op::scan::dynamic_filter_gate gate;

  sirius_dynamic_filter_set filters;
  filters.push_filter(0, make_in_list_prefix(2, stream));
  auto table = make_int64_sequence_table(10, stream);

  // The filter has only its current-device source replica. A consumer device with no local copy
  // must skip without recording a synthetic 100% keep ratio in the scan-global gate.
  auto unavailable = sirius::op::scan::apply_dynamic_filters_gated_view(
    table->view(),
    filters,
    gate,
    stream,
    dynamic_filter_apply_mode::include_ast_row_masks,
    /*device_id=*/12345);
  REQUIRE(unavailable == nullptr);
  REQUIRE(gate.applicable(filters));

  auto local = sirius::op::scan::apply_dynamic_filters_gated_view(
    table->view(), filters, gate, stream, dynamic_filter_apply_mode::include_ast_row_masks);
  stream.synchronize();
  REQUIRE(local != nullptr);
  REQUIRE(local->num_rows() == 2);
  REQUIRE(gate.applicable(filters));
}

TEST_CASE("dynamic_filter_gate re-arms when a filter publishes after the disable decision",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  sirius::op::scan::dynamic_filter_gate gate;

  // Unselective filter publishes first and disables the gate (the Q8 supplier hazard).
  sirius_dynamic_filter_set filters;
  filters.push_filter(0, make_in_list_prefix(10, stream));
  auto table = make_int64_sequence_table(10, stream);
  (void)sirius::op::scan::apply_dynamic_filters_gated_view(
    table->view(), filters, gate, stream, dynamic_filter_apply_mode::include_ast_row_masks);
  stream.synchronize();
  REQUIRE_FALSE(gate.applicable(filters));

  // A selective filter lands later: the channel grew, so the gate must re-arm...
  filters.push_filter(0, make_in_list_prefix(2, stream));
  REQUIRE(gate.applicable(filters));

  // ...and the re-measurement sees the combined mask (AND → keeps 0,1), going ACTIVE.
  auto out = sirius::op::scan::apply_dynamic_filters_gated_view(
    table->view(), filters, gate, stream, dynamic_filter_apply_mode::include_ast_row_masks);
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(out->num_rows() == 2);
  REQUIRE(gate.applicable(filters));  // active — stays applicable
}

TEST_CASE("dynamic_filter_gate stays active once a selective split proves the filter useful",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  sirius::op::scan::dynamic_filter_gate gate;

  sirius_dynamic_filter_set filters;
  filters.push_filter(0, make_in_list_prefix(2, stream));  // selective: keeps 20%
  auto table = make_int64_sequence_table(10, stream);
  (void)sirius::op::scan::apply_dynamic_filters_gated_view(
    table->view(), filters, gate, stream, dynamic_filter_apply_mode::include_ast_row_masks);
  stream.synchronize();
  REQUIRE(gate.applicable(filters));

  // A later unselective publish must not demote an active gate.
  filters.push_filter(0, make_in_list_prefix(10, stream));
  auto out = sirius::op::scan::apply_dynamic_filters_gated_view(
    table->view(), filters, gate, stream, dynamic_filter_apply_mode::include_ast_row_masks);
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(out->num_rows() == 2);      // cascade of both filters == their conjunction
  REQUIRE(gate.applicable(filters));  // still active
}

TEST_CASE("dynamic_filter_gate serializes concurrent stale and re-armed decisions",
          "[dynamic_filter][scan_merge][concurrent]")
{
  // Model tasks that started on the old one-filter generation and finish alongside selective tasks
  // that observed the newly-published second filter. Once any current-generation task commits
  // ACTIVE, stale completions must not overwrite it with DISABLED.
  sirius_dynamic_filter_set filters;
  filters.push_filter(0, std::make_shared<stub_runtime_only_filter>());
  filters.push_filter(0, std::make_shared<stub_runtime_only_filter>());

  constexpr std::size_t rounds          = 256;
  constexpr std::size_t stale_workers   = 6;
  constexpr std::size_t current_workers = 2;
  constexpr std::size_t total_workers   = stale_workers + current_workers;
  std::vector<std::unique_ptr<sirius::op::scan::dynamic_filter_gate>> gates;
  gates.reserve(rounds);
  for (std::size_t i = 0; i < rounds; ++i) {
    gates.push_back(std::make_unique<sirius::op::scan::dynamic_filter_gate>());
  }

  std::barrier phase{static_cast<std::ptrdiff_t>(total_workers + 1)};
  std::vector<std::thread> workers;
  workers.reserve(total_workers);
  auto record_all = [&](bool current_generation) {
    for (auto const& gate : gates) {
      phase.arrive_and_wait();
      gate->record_keep_ratio(/*rows_before=*/100,
                              /*rows_after=*/current_generation ? 10 : 100,
                              /*observed_filter_count=*/current_generation ? 2 : 1);
      phase.arrive_and_wait();
    }
  };
  for (std::size_t i = 0; i < stale_workers; ++i) {
    workers.emplace_back(record_all, false);
  }
  for (std::size_t i = 0; i < current_workers; ++i) {
    workers.emplace_back(record_all, true);
  }

  bool all_stayed_active = true;
  for (auto const& gate : gates) {
    phase.arrive_and_wait();
    phase.arrive_and_wait();

    // If a stale completion overwrote ACTIVE with the old generation's DISABLED decision, this
    // current-generation unselective result seals DISABLED and makes applicable() false. With the
    // serialized transition, ACTIVE is terminal and this call is a no-op.
    gate->record_keep_ratio(/*rows_before=*/100,
                            /*rows_after=*/100,
                            /*observed_filter_count=*/2);
    all_stayed_active = all_stayed_active && gate->applicable(filters);
  }
  for (auto& worker : workers) {
    worker.join();
  }

  REQUIRE(all_stayed_active);
}

TEST_CASE("cascaded membership filters produce the conjunction of all filters",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();

  sirius_dynamic_filter_set filters;
  filters.push_filter(0, make_in_list_prefix(7, stream));  // keeps 0..6
  filters.push_filter(0, make_in_list_prefix(4, stream));  // keeps 0..3

  auto table = make_int64_sequence_table(10, stream);
  auto out   = sirius::op::scan::apply_dynamic_filters_to_view(table->view(), filters, stream);
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(out->num_rows() == 4);  // 0..3 — intersection regardless of cascade order
}

TEST_CASE("per-filter gate measures marginal keep and skips a useless filter on later splits",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  sirius::op::scan::dynamic_filter_gate gate;

  auto useless   = make_in_list_prefix(10, stream);  // covers the whole domain — keep 1.0
  auto selective = make_in_list_prefix(2, stream);   // keeps 20%
  sirius_dynamic_filter_set filters;
  filters.push_filter(0, useless);
  filters.push_filter(0, selective);

  auto table = make_int64_sequence_table(10, stream);
  auto out   = sirius::op::scan::apply_dynamic_filters_gated_view(
    table->view(), filters, gate, stream, dynamic_filter_apply_mode::include_ast_row_masks);
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(out->num_rows() == 2);

  // First split measured both marginals: the domain-covering filter is now skippable, the
  // selective one is not.
  auto useless_kept = gate.filter_keep_ratio(useless.get(), filters.filter_count());
  REQUIRE(useless_kept.has_value());
  REQUIRE(sirius::op::scan::dynamic_filter_gate::filter_skippable(*useless_kept));
  auto selective_kept = gate.filter_keep_ratio(selective.get(), filters.filter_count());
  REQUIRE(selective_kept.has_value());
  REQUIRE_FALSE(sirius::op::scan::dynamic_filter_gate::filter_skippable(*selective_kept));

  // Later splits still produce the right rows with the useless filter dropped from the cascade.
  auto out2 = sirius::op::scan::apply_dynamic_filters_gated_view(
    table->view(), filters, gate, stream, dynamic_filter_apply_mode::include_ast_row_masks);
  stream.synchronize();
  REQUIRE(out2 != nullptr);
  REQUIRE(out2->num_rows() == 2);
}

TEST_CASE("per-filter gate keeps a dead verdict when the channel grows",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  sirius::op::scan::dynamic_filter_gate gate;

  auto useless = make_in_list_prefix(10, stream);  // covers the whole domain -- keep 1.0
  sirius_dynamic_filter_set filters;
  filters.push_filter(0, useless);

  auto table = make_int64_sequence_table(10, stream);
  auto out   = sirius::op::scan::apply_dynamic_filters_gated_view(
    table->view(), filters, gate, stream, dynamic_filter_apply_mode::include_ast_row_masks);
  stream.synchronize();
  REQUIRE(out != nullptr);

  auto const measured = gate.filter_keep_ratio(useless.get(), filters.filter_count());
  REQUIRE(measured.has_value());
  REQUIRE(sirius::op::scan::dynamic_filter_gate::filter_skippable(*measured));

  // Growth re-opens the scan-level gate, not this verdict: the dead filter stays dead.
  filters.push_filter(0, make_in_list_prefix(2, stream));
  auto const after_growth = gate.filter_keep_ratio(useless.get(), filters.filter_count());
  REQUIRE(after_growth.has_value());
  REQUIRE(sirius::op::scan::dynamic_filter_gate::filter_skippable(*after_growth));
  REQUIRE(after_growth == measured);  // the stored verdict, not a remeasure trigger
}

TEST_CASE("per-filter gate excludes a dead filter from the re-armed apply without re-running it",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  sirius::op::scan::dynamic_filter_gate gate;

  // The only filter covers the whole domain: the first split disables the scan-level gate and
  // records the filter's dead marginal verdict.
  auto useless = std::make_shared<counting_in_list_filter>(make_in_list_prefix(10, stream));
  sirius_dynamic_filter_set filters;
  filters.push_filter(0, useless);

  auto table = make_int64_sequence_table(10, stream);
  auto out   = sirius::op::scan::apply_dynamic_filters_gated_view(
    table->view(), filters, gate, stream, dynamic_filter_apply_mode::include_ast_row_masks);
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(out->num_rows() == 10);
  REQUIRE(useless->mask_calls() == 1);
  REQUIRE_FALSE(gate.applicable(filters));

  // Growth re-arms the scan-level gate; the dead verdict is permanent, so the re-armed apply
  // runs only the newcomer.
  auto selective = make_in_list_prefix(2, stream);
  filters.push_filter(0, selective);
  REQUIRE(gate.applicable(filters));

  auto out2 = sirius::op::scan::apply_dynamic_filters_gated_view(
    table->view(), filters, gate, stream, dynamic_filter_apply_mode::include_ast_row_masks);
  stream.synchronize();
  REQUIRE(out2 != nullptr);
  REQUIRE(out2->num_rows() == 2);
  REQUIRE(useless->mask_calls() == 1);

  auto const useless_kept = gate.filter_keep_ratio(useless.get(), filters.filter_count());
  REQUIRE(useless_kept.has_value());
  REQUIRE(sirius::op::scan::dynamic_filter_gate::filter_skippable(*useless_kept));
  auto const selective_kept = gate.filter_keep_ratio(selective.get(), filters.filter_count());
  REQUIRE(selective_kept.has_value());
  REQUIRE_FALSE(sirius::op::scan::dynamic_filter_gate::filter_skippable(*selective_kept));
  REQUIRE(gate.applicable(filters));  // the re-arm measured 0.2 -> ACTIVE
}

TEST_CASE("per-filter gate stales a selective verdict when the channel grows",
          "[dynamic_filter][scan_merge]")
{
  auto stream = cudf::get_default_stream();
  sirius::op::scan::dynamic_filter_gate gate;

  auto selective = make_in_list_prefix(2, stream);  // keeps 20%
  sirius_dynamic_filter_set filters;
  filters.push_filter(0, selective);

  auto table = make_int64_sequence_table(10, stream);
  auto out   = sirius::op::scan::apply_dynamic_filters_gated_view(
    table->view(), filters, gate, stream, dynamic_filter_apply_mode::include_ast_row_masks);
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(out->num_rows() == 2);

  auto const measured = gate.filter_keep_ratio(selective.get(), filters.filter_count());
  REQUIRE(measured.has_value());
  REQUIRE_FALSE(sirius::op::scan::dynamic_filter_gate::filter_skippable(*measured));

  // Growth stales a selective reading: new arrivals change the rows reaching the filter.
  filters.push_filter(0, make_in_list_prefix(4, stream));
  REQUIRE_FALSE(gate.filter_keep_ratio(selective.get(), filters.filter_count()).has_value());

  // The next apply remeasures it against the larger cascade.
  auto out2 = sirius::op::scan::apply_dynamic_filters_gated_view(
    table->view(), filters, gate, stream, dynamic_filter_apply_mode::include_ast_row_masks);
  stream.synchronize();
  REQUIRE(out2 != nullptr);
  REQUIRE(out2->num_rows() == 2);
  REQUIRE(gate.filter_keep_ratio(selective.get(), filters.filter_count()).has_value());
}

//===----------------------------------------------------------------------===//
// Narrow-carrier probes, stencils and the scan-side fused apply
//===----------------------------------------------------------------------===//

namespace {

/// Fixed-width column from host values, optionally with nulls at the given rows.
template <class T>
std::unique_ptr<cudf::column> make_column(std::vector<T> const& values,
                                          cudf::data_type dtype,
                                          rmm::cuda_stream_view stream,
                                          std::vector<cudf::size_type> const& null_rows = {})
{
  auto const n = static_cast<cudf::size_type>(values.size());
  auto col     = cudf::make_numeric_column(dtype, n, cudf::mask_state::UNALLOCATED, stream);
  cudaMemcpyAsync(col->mutable_view().head<T>(),
                  values.data(),
                  values.size() * sizeof(T),
                  cudaMemcpyHostToDevice,
                  stream.value());
  if (!null_rows.empty()) {
    auto mask = cudf::create_null_mask(n, cudf::mask_state::ALL_VALID, stream);
    for (auto const row : null_rows) {
      cudf::set_null_mask(
        static_cast<cudf::bitmask_type*>(mask.data()), row, row + 1, false, stream);
    }
    col->set_null_mask(std::move(mask), static_cast<cudf::size_type>(null_rows.size()));
  }
  stream.synchronize();
  return col;
}

/// BOOL8 values to host as bytes (std::vector<bool> is not contiguous).
std::vector<std::uint8_t> to_host_bool(cudf::column_view const& col, rmm::cuda_stream_view stream)
{
  std::vector<std::uint8_t> host(static_cast<std::size_t>(col.size()));
  cudaMemcpyAsync(
    host.data(), col.data<bool>(), host.size(), cudaMemcpyDeviceToHost, stream.value());
  stream.synchronize();
  return host;
}

std::unique_ptr<cudf::column> make_int64_keys(int64_t count, rmm::cuda_stream_view stream)
{
  return cudf::sequence(static_cast<cudf::size_type>(count),
                        cudf::numeric_scalar<int64_t>(0, true, stream),
                        cudf::numeric_scalar<int64_t>(1, true, stream),
                        stream);
}

template <class T>
std::vector<T> iota_values(std::size_t n)
{
  std::vector<T> v(n);
  std::iota(v.begin(), v.end(), T{0});
  return v;
}

/// The three membership kinds over INT64 build keys {0..4}.
struct membership_trio {
  std::shared_ptr<sirius::op::sirius_dynamic_in_list_filter> in_list;
  std::shared_ptr<sirius::op::sirius_dynamic_small_in_list_filter> small_in_list;
  std::shared_ptr<sirius::op::sirius_dynamic_bloom_filter> bloom;

  explicit membership_trio(rmm::cuda_stream_view stream)
  {
    auto keys = make_int64_keys(5, stream);
    auto mr   = cudf::get_current_device_resource_ref();
    in_list = std::make_shared<sirius::op::sirius_dynamic_in_list_filter>(keys->view(), stream, mr);
    small_in_list =
      std::make_shared<sirius::op::sirius_dynamic_small_in_list_filter>(keys->view(), stream, mr);
    bloom = std::make_shared<sirius::op::sirius_dynamic_bloom_filter>(keys->view(), stream, mr);
    stream.synchronize();
  }

  [[nodiscard]] std::vector<sirius::op::sirius_mask_applicable const*> all() const
  {
    return {in_list.get(), small_in_list.get(), bloom.get()};
  }

  /// The two exact kinds (a Bloom may keep false positives).
  [[nodiscard]] std::vector<sirius::op::sirius_mask_applicable const*> exact() const
  {
    return {in_list.get(), small_in_list.get()};
  }
};

}  // namespace

TEST_CASE("membership filters probe a narrower carrier by widening each value",
          "[dynamic_filter][scan_merge][narrow_carrier]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();
  membership_trio const filters(stream);

  // Compressed materialization stores the same values in a narrower carrier: INT32/INT16/INT8
  // probes of an INT64 key must give the native answer, not "incompatible" (which the gate would
  // read as keep-everything and drop the filter for the whole scan).
  std::vector<std::unique_ptr<cudf::column>> probes;
  probes.push_back(
    make_column(iota_values<int32_t>(10), cudf::data_type{cudf::type_id::INT32}, stream));
  probes.push_back(
    make_column(iota_values<int16_t>(10), cudf::data_type{cudf::type_id::INT16}, stream));
  probes.push_back(
    make_column(iota_values<int8_t>(10), cudf::data_type{cudf::type_id::INT8}, stream));

  for (auto const& probe : probes) {
    for (auto const* filter : filters.all()) {
      auto mask = filter->compute_mask(probe->view(), -1, stream, mr);
      REQUIRE(mask != nullptr);
      auto const host = to_host_bool(mask->view(), stream);
      for (std::size_t row = 0; row < 5; ++row) {
        REQUIRE(host[row] == 1);  // build keys 0..4 are members (Bloom: no false negative)
      }
    }
    for (auto const* filter : filters.exact()) {
      auto const host =
        to_host_bool(filter->compute_mask(probe->view(), -1, stream, mr)->view(), stream);
      REQUIRE(host == std::vector<std::uint8_t>{1, 1, 1, 1, 1, 0, 0, 0, 0, 0});
    }
  }
}

TEST_CASE("membership filters reject a probe that is not a carrier of the key type",
          "[dynamic_filter][scan_merge][narrow_carrier]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();
  membership_trio const int64_filters(stream);

  // A probe wider than the key can hold values the key never could: incompatible.
  auto const int32_keys =
    make_column(iota_values<int32_t>(5), cudf::data_type{cudf::type_id::INT32}, stream);
  sirius::op::sirius_dynamic_in_list_filter int32_filter(int32_keys->view(), stream, mr);
  auto const int64_probe = make_int64_keys(10, stream);
  REQUIRE(int32_filter.compute_mask(int64_probe->view(), -1, stream, mr) == nullptr);

  auto const float_probe =
    make_column(std::vector<double>(10, 1.0), cudf::data_type{cudf::type_id::FLOAT64}, stream);
  for (auto const* filter : int64_filters.all()) {
    REQUIRE(filter->compute_mask(float_probe->view(), -1, stream, mr) == nullptr);
  }
}

TEST_CASE("compute_mask_if yields false without probing where the stencil is false",
          "[dynamic_filter][scan_merge][stencil]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();
  membership_trio const filters(stream);

  auto const probe = make_int64_keys(10, stream);  // members 0..4, non-members 5..9
  std::vector<std::uint8_t> stencil_host(10);
  for (std::size_t i = 0; i < 10; ++i) {
    stencil_host[i] = (i % 2 == 0) ? 1 : 0;
  }
  auto const stencil = make_column(stencil_host, cudf::data_type{cudf::type_id::BOOL8}, stream);
  auto const* const stencil_data = stencil->view().data<bool>();

  for (auto const* filter : filters.all()) {
    auto mask = filter->compute_mask_if(probe->view(), stencil_data, -1, stream, mr);
    REQUIRE(mask != nullptr);
    auto const host = to_host_bool(mask->view(), stream);
    for (std::size_t row = 0; row < 10; ++row) {
      if (row % 2 == 1) {
        REQUIRE(host[row] == 0);  // stencilled out, whatever the membership answer
      } else if (row < 5) {
        REQUIRE(host[row] == 1);  // member, stencil true
      }
    }
  }
  for (auto const* filter : filters.exact()) {
    auto const host = to_host_bool(
      filter->compute_mask_if(probe->view(), stencil_data, -1, stream, mr)->view(), stream);
    REQUIRE(host == std::vector<std::uint8_t>{1, 0, 1, 0, 1, 0, 0, 0, 0, 0});
  }
}

TEST_CASE("and_mask_count folds nulls to false and counts the survivors",
          "[dynamic_filter][scan_merge][mask_ops]")
{
  auto stream      = cudf::get_default_stream();
  auto mr          = cudf::get_current_device_resource_ref();
  auto const bool8 = cudf::data_type{cudf::type_id::BOOL8};

  rmm::device_uvector<cudf::size_type> counts(2, stream, mr);
  cudaMemsetAsync(counts.data(), 0, counts.size() * sizeof(cudf::size_type), stream.value());

  // Accumulator {1,1,0,1,1} with row 4 null: folding alone gives {1,1,0,1,0}, 3 survivors.
  auto acc = make_column(std::vector<std::uint8_t>{1, 1, 0, 1, 1}, bool8, stream, {4});
  sirius::op::and_mask_count(*acc, std::nullopt, counts.data(), stream);
  acc->set_null_mask(rmm::device_buffer{}, 0);
  REQUIRE(to_host_bool(acc->view(), stream) == std::vector<std::uint8_t>{1, 1, 0, 1, 0});

  // Mask {1,0,1,1,1} with row 3 null: conjunction {1,0,0,0,0}, 1 survivor.
  auto const mask = make_column(std::vector<std::uint8_t>{1, 0, 1, 1, 1}, bool8, stream, {3});
  sirius::op::and_mask_count(*acc, mask->view(), counts.data() + 1, stream);
  REQUIRE(to_host_bool(acc->view(), stream) == std::vector<std::uint8_t>{1, 0, 0, 0, 0});

  std::vector<cudf::size_type> host_counts(2);
  cudaMemcpyAsync(host_counts.data(),
                  counts.data(),
                  host_counts.size() * sizeof(cudf::size_type),
                  cudaMemcpyDeviceToHost,
                  stream.value());
  stream.synchronize();
  REQUIRE(host_counts == std::vector<cudf::size_type>{3, 1});

  // A mask of another size cannot be folded.
  auto const short_mask = make_column(std::vector<std::uint8_t>{1}, bool8, stream);
  REQUIRE_THROWS_AS(sirius::op::and_mask_count(*acc, short_mask->view(), counts.data(), stream),
                    std::invalid_argument);
}

namespace {

/// A three-column split view standing in for a pinned lineitem chunk: column 0 is the INT32
/// carrier of an INT64 join key (values 0..9), column 1 an INT64 payload (100 + row), column 2 a
/// pure-filter column the output does not carry. Output layout: {payload, key}.
struct fused_apply_fixture {
  std::unique_ptr<cudf::table> split;
  std::vector<cudf::size_type> output_positions{1, 0};
  sirius::op::scan::probe_position_fn probe_position =
    [](std::size_t output_col) -> std::optional<cudf::size_type> {
    if (output_col == 0) { return 1; }
    if (output_col == 1) { return 0; }
    return std::nullopt;
  };

  explicit fused_apply_fixture(rmm::cuda_stream_view stream,
                               std::vector<cudf::size_type> const& null_key_rows = {})
  {
    std::vector<std::unique_ptr<cudf::column>> cols;
    cols.push_back(make_column(
      iota_values<int32_t>(10), cudf::data_type{cudf::type_id::INT32}, stream, null_key_rows));
    std::vector<int64_t> payload(10);
    for (std::size_t i = 0; i < 10; ++i) {
      payload[i] = 100 + static_cast<int64_t>(i);
    }
    cols.push_back(make_column(payload, cudf::data_type{cudf::type_id::INT64}, stream));
    cols.push_back(
      make_column(iota_values<int32_t>(10), cudf::data_type{cudf::type_id::INT32}, stream));
    split = std::make_unique<cudf::table>(std::move(cols));
  }

  /// Residual "row is even" as a BOOL8 mask over the split.
  [[nodiscard]] std::unique_ptr<cudf::column> even_rows_mask(rmm::cuda_stream_view stream) const
  {
    std::vector<std::uint8_t> values(10);
    for (std::size_t i = 0; i < 10; ++i) {
      values[i] = (i % 2 == 0) ? 1 : 0;
    }
    return make_column(values, cudf::data_type{cudf::type_id::BOOL8}, stream);
  }

  [[nodiscard]] std::unique_ptr<cudf::table> gather(
    std::unique_ptr<cudf::column> residual_mask,
    sirius_dynamic_filter_set const* filters,
    sirius::op::scan::dynamic_filter_gate* gate,
    sirius::op::scan::scan_dynamic_filter_result* applied,
    rmm::cuda_stream_view stream) const
  {
    auto out = sirius::op::scan::gather_view_survivors(split->view(),
                                                       output_positions,
                                                       probe_position,
                                                       std::move(residual_mask),
                                                       filters,
                                                       gate,
                                                       -1,
                                                       stream,
                                                       cudf::get_current_device_resource_ref(),
                                                       applied);
    stream.synchronize();
    return out;
  }
};

}  // namespace

TEST_CASE("gather_view_survivors folds the residual and every membership mask into one gather",
          "[dynamic_filter][scan_merge][fused_apply]")
{
  auto stream = cudf::get_default_stream();
  fused_apply_fixture const fx(stream);

  // Two membership filters on the key (output column 1): keys 0..6, then keys 0..3.
  auto prefix7 = make_in_list_prefix(7, stream);
  auto prefix4 = make_in_list_prefix(4, stream);
  sirius_dynamic_filter_set filters;
  filters.push_filter(1, prefix7);
  filters.push_filter(1, prefix4);
  sirius::op::scan::dynamic_filter_gate gate;
  sirius::op::scan::scan_dynamic_filter_result applied;

  auto out = fx.gather(fx.even_rows_mask(stream), &filters, &gate, &applied, stream);
  REQUIRE(out != nullptr);
  // even {0,2,4,6,8} ∩ {0..6} ∩ {0..3} = {0, 2}; output layout {payload, key}, carriers kept.
  REQUIRE(out->num_columns() == 2);
  REQUIRE(out->view().column(0).type().id() == cudf::type_id::INT64);
  REQUIRE(out->view().column(1).type().id() == cudf::type_id::INT32);
  REQUIRE(to_host_int64(out->view().column(0), stream) == std::vector<int64_t>{100, 102});
  REQUIRE(to_host_int32(out->view().column(1), stream) == std::vector<int32_t>{0, 2});
  REQUIRE(fx.split->num_rows() == 10);  // the view was only read

  // Marginal ratios in cascade order: 5 residual survivors -> 4 (0.8) -> 2 (0.5).
  auto const kept7 = gate.filter_keep_ratio(prefix7.get(), filters.filter_count());
  REQUIRE(kept7.has_value());
  REQUIRE(*kept7 == Approx(0.8));
  auto const kept4 = gate.filter_keep_ratio(prefix4.get(), filters.filter_count());
  REQUIRE(kept4.has_value());
  REQUIRE(*kept4 == Approx(0.5));
  // Scan-level ratio 2/5 keeps the gate active.
  REQUIRE(gate.applicable(filters));

  // Both identities travel to the DYNAMIC_FILTER operator, which then has nothing left to do.
  REQUIRE(applied.applied.size() == 2);
  REQUIRE(applied.observed_filter_count == 2);
  REQUIRE(std::find(applied.applied.begin(), applied.applied.end(), prefix7.get()) !=
          applied.applied.end());
  REQUIRE(std::find(applied.applied.begin(), applied.applied.end(), prefix4.get()) !=
          applied.applied.end());
  auto passthrough = sirius::op::scan::apply_dynamic_filters_gated_view(
    out->view(),
    filters,
    gate,
    stream,
    dynamic_filter_apply_mode::membership_masks_only,
    -1,
    applied.applied);
  REQUIRE(passthrough == nullptr);
}

TEST_CASE("gather_view_survivors drops null keys and works without a residual",
          "[dynamic_filter][scan_merge][fused_apply]")
{
  auto stream = cudf::get_default_stream();
  fused_apply_fixture const fx(stream, /*null_key_rows=*/{1});

  sirius_dynamic_filter_set filters;
  filters.push_filter(1, make_in_list_prefix(4, stream));  // keys 0..3
  sirius::op::scan::dynamic_filter_gate gate;
  sirius::op::scan::scan_dynamic_filter_result applied;

  auto out = fx.gather(/*residual_mask=*/nullptr, &filters, &gate, &applied, stream);
  REQUIRE(out != nullptr);
  // Key 1 is null: a null probe never matches, exactly as apply_boolean_mask drops null masks.
  REQUIRE(to_host_int64(out->view().column(0), stream) == std::vector<int64_t>{100, 102, 103});
  REQUIRE(applied.applied.size() == 1);
  REQUIRE(gate.applicable(filters));
}

TEST_CASE("gather_view_survivors returns null when nothing applies and gathers a residual alone",
          "[dynamic_filter][scan_merge][fused_apply]")
{
  auto stream = cudf::get_default_stream();
  fused_apply_fixture const fx(stream);
  sirius::op::scan::dynamic_filter_gate gate;
  sirius_dynamic_filter_set empty;
  sirius::op::scan::scan_dynamic_filter_result applied;

  // No residual, no filter: the caller materializes as before.
  REQUIRE(fx.gather(nullptr, &empty, &gate, &applied, stream) == nullptr);
  REQUIRE(fx.gather(nullptr, /*filters=*/nullptr, nullptr, nullptr, stream) == nullptr);

  // Residual only: the gather is the residual select; no filter is reported and the gate is
  // not trained (nothing was measured).
  auto out = fx.gather(fx.even_rows_mask(stream), &empty, &gate, &applied, stream);
  REQUIRE(out != nullptr);
  REQUIRE(to_host_int32(out->view().column(1), stream) == std::vector<int32_t>{0, 2, 4, 6, 8});
  REQUIRE(applied.empty());
  REQUIRE_FALSE(gate.applicable(empty));
}

TEST_CASE("gather_view_survivors records an unservable probe as keep-everything",
          "[dynamic_filter][scan_merge][fused_apply]")
{
  auto stream = cudf::get_default_stream();
  auto mr     = cudf::get_current_device_resource_ref();
  fused_apply_fixture const fx(stream);

  // INT32 build keys on output column 0, whose carrier is the INT64 payload: the probe is wider
  // than the key, so no mask can be computed. The cascade records that as kept 1.0 (skippable)
  // and the fused apply must agree, while the usable filter on the key still applies.
  auto const int32_keys =
    make_column(iota_values<int32_t>(3), cudf::data_type{cudf::type_id::INT32}, stream);
  auto unservable =
    std::make_shared<sirius::op::sirius_dynamic_in_list_filter>(int32_keys->view(), stream, mr);
  auto usable = make_in_list_prefix(3, stream);  // keys 0..2
  sirius_dynamic_filter_set filters;
  filters.push_filter(0, unservable);
  filters.push_filter(1, usable);
  sirius::op::scan::dynamic_filter_gate gate;
  sirius::op::scan::scan_dynamic_filter_result applied;

  auto out = fx.gather(nullptr, &filters, &gate, &applied, stream);
  REQUIRE(out != nullptr);
  REQUIRE(to_host_int32(out->view().column(1), stream) == std::vector<int32_t>{0, 1, 2});
  REQUIRE(applied.applied == std::vector<sirius_dynamic_filter const*>{usable.get()});

  auto const unservable_kept = gate.filter_keep_ratio(unservable.get(), filters.filter_count());
  REQUIRE(unservable_kept.has_value());
  REQUIRE(sirius::op::scan::dynamic_filter_gate::filter_skippable(*unservable_kept));
  auto const usable_kept = gate.filter_keep_ratio(usable.get(), filters.filter_count());
  REQUIRE(usable_kept.has_value());
  REQUIRE(*usable_kept == Approx(0.3));
}

TEST_CASE("apply_dynamic_filters_to_view skips filters the scan already applied",
          "[dynamic_filter][scan_merge][fused_apply]")
{
  auto stream  = cudf::get_default_stream();
  auto counted = std::make_shared<counting_in_list_filter>(make_in_list_prefix(2, stream));
  auto other   = make_in_list_prefix(6, stream);
  sirius_dynamic_filter_set filters;
  filters.push_filter(0, counted);
  filters.push_filter(0, other);

  auto table = make_int64_sequence_table(10, stream);
  std::vector<sirius_dynamic_filter const*> const already{counted.get()};
  auto out = sirius::op::scan::apply_dynamic_filters_to_view(
    table->view(),
    filters,
    stream,
    dynamic_filter_apply_mode::membership_masks_only,
    nullptr,
    -1,
    already);
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(out->num_rows() == 6);  // only `other` ran
  REQUIRE(counted->mask_calls() == 0);
}
