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
 * @file test_duckdb_native_df_fold.cpp
 * @brief Tests for the dynamic-filter membership fold in
 *        duckdb_native_gpu_ingestible::post_filter_and_project and its helper
 *        fold_membership_probes_into_mask: published membership probes join the static
 *        filter's gather (hit/miss), while an unpublished channel, a channel with no
 *        mask-capable filters, or a scan without a static filter all take today's path
 *        unchanged.
 */

#include "test_utils.hpp"

#include <cudf/column/column_factories.hpp>
#include <cudf/filling.hpp>
#include <cudf/null_mask.hpp>
#include <cudf/scalar/scalar.hpp>
#include <cudf/table/table.hpp>
#include <cudf/utilities/memory_resource.hpp>

#include <rmm/cuda_stream.hpp>

#include <cuda_runtime.h>

#include <catch.hpp>
#include <duckdb.hpp>
#include <duckdb/catalog/catalog.hpp>
#include <duckdb/catalog/catalog_entry/duck_table_entry.hpp>
#include <duckdb/common/column_index.hpp>
#include <duckdb/main/client_context.hpp>
#include <duckdb/planner/filter/constant_filter.hpp>
#include <duckdb/planner/table_filter.hpp>
#include <duckdb/storage/data_table.hpp>
#include <op/dynamic_filter/sirius_dynamic_filter.hpp>
#include <op/scan/duckdb_native_gpu_ingestible.hpp>
#include <op/scan/gpu_ingestible.hpp>
#include <op/scan/sirius_gpu_scan_operator_data.hpp>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace sirius;
using namespace sirius::op::scan;

namespace {

void exec_ok(duckdb::Connection& con, std::string const& q)
{
  auto result = con.Query(q);
  REQUIRE(result);
  if (result->HasError()) {
    INFO("query failed: " << q << "\n  error: " << result->GetError());
    REQUIRE_FALSE(result->HasError());
  }
}

// File-backed database; the ingestible's metadata walk reads checkpointed row groups.
struct df_fold_test_db {
  std::string path;
  std::unique_ptr<duckdb::DuckDB> db;
  std::unique_ptr<duckdb::Connection> con;

  df_fold_test_db()
  {
    static int counter = 0;
    path               = "/tmp/sirius_df_fold_test_" + std::to_string(::getpid()) + "_" +
           std::to_string(counter++) + ".db";
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());
    db  = std::make_unique<duckdb::DuckDB>(path);
    con = std::make_unique<duckdb::Connection>(*db);
    con->Query("SET gpu_execution = false;");
  }

  ~df_fold_test_db()
  {
    con.reset();
    db.reset();
    std::remove(path.c_str());
    std::remove((path + ".wal").c_str());
  }
};

// Requires an active transaction because catalog access needs one.
duckdb::DataTable& resolve_storage(duckdb::Connection& con, std::string const& table_name)
{
  auto& ctx     = *con.context;
  auto& catalog = duckdb::Catalog::GetCatalog(ctx, "");
  duckdb::CatalogTransaction txn(catalog, ctx);
  auto& schema = catalog.GetSchema(txn, "main");
  auto entry   = schema.GetEntry(txn, duckdb::CatalogType::TABLE_ENTRY, table_name);
  REQUIRE(entry);
  return entry->Cast<duckdb::DuckTableEntry>().GetStorage();
}

projected_column real_col(duckdb::idx_t col_id)
{
  projected_column pc;
  pc.storage_idx = duckdb::StorageIndex(col_id);
  pc.is_rowid    = false;
  return pc;
}

/// Static row filter "v < 6" keyed on scan-relative column 1 (identity column_ids map it
/// to storage column 1).
duckdb::unique_ptr<duckdb::TableFilterSet> make_v_less_than_6_filter()
{
  auto filters        = duckdb::make_uniq<duckdb::TableFilterSet>();
  filters->filters[1] = duckdb::make_uniq<duckdb::ConstantFilter>(
    duckdb::ExpressionType::COMPARE_LESSTHAN, duckdb::Value::BIGINT(6));
  return filters;
}

/// Build the two-BIGINT-column t(k, v) ingestible over the checkpointed test table, with an
/// optional static filter, an optional published channel, and @p output_arity of 1 or 2 (1
/// makes v a pure-filter column the projection must fold away).
std::shared_ptr<duckdb_native_gpu_ingestible> make_test_ingestible(
  df_fold_test_db& env,
  duckdb::unique_ptr<duckdb::TableFilterSet> table_filters,
  std::shared_ptr<sirius::op::sirius_dynamic_filter_set> channel,
  std::size_t output_arity = 2)
{
  auto& storage = resolve_storage(*env.con, "t");

  auto info             = std::make_unique<duckdb_native_ingestible_table_info>();
  info->storage         = &storage;
  info->context         = env.con->context.get();
  info->db_path         = env.path;
  auto const bigint     = sirius::logical_type::make(sirius::type_id::BIGINT);
  info->projected_cols  = {real_col(0), real_col(1)};
  info->projected_types = {bigint, bigint};
  info->returned_types  = {bigint, bigint};
  info->names           = {"k", "v"};
  info->column_ids.push_back(duckdb::ColumnIndex(0));
  info->column_ids.push_back(duckdb::ColumnIndex(1));
  for (std::size_t i = 0; i < output_arity; ++i) {
    info->output_types.push_back(bigint);
  }
  info->table_filters          = std::move(table_filters);
  info->sirius_dynamic_filters = std::move(channel);
  return make_ingestible(std::move(info));
}

/// GPU input table with two INT64 columns k = v = [0..rows).
std::unique_ptr<cudf::table> make_kv_sequence_table(cudf::size_type rows,
                                                    rmm::cuda_stream_view stream)
{
  std::vector<std::unique_ptr<cudf::column>> cols;
  for (int c = 0; c < 2; ++c) {
    cols.push_back(cudf::sequence(rows,
                                  cudf::numeric_scalar<int64_t>(0, true, stream),
                                  cudf::numeric_scalar<int64_t>(1, true, stream),
                                  stream));
  }
  return std::make_unique<cudf::table>(std::move(cols));
}

/// Single INT64 GPU column from explicit host values.
std::unique_ptr<cudf::column> make_int64_column(std::vector<int64_t> const& values,
                                                rmm::cuda_stream_view stream)
{
  auto col = cudf::make_numeric_column(cudf::data_type{cudf::type_id::INT64},
                                       static_cast<cudf::size_type>(values.size()),
                                       cudf::mask_state::UNALLOCATED,
                                       stream);
  cudaMemcpyAsync(col->mutable_view().data<int64_t>(),
                  values.data(),
                  values.size() * sizeof(int64_t),
                  cudaMemcpyHostToDevice,
                  stream.value());
  stream.synchronize();
  return col;
}

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

std::shared_ptr<sirius::op::sirius_dynamic_in_list_filter> make_in_list(
  std::vector<int64_t> const& keys, rmm::cuda_stream_view stream)
{
  auto key_col = make_int64_column(keys, stream);
  return std::make_shared<sirius::op::sirius_dynamic_in_list_filter>(
    key_col->view(), stream, cudf::get_current_device_resource_ref());
}

std::unique_ptr<cudf::table> run_post_filter(duckdb_native_gpu_ingestible& ingestible,
                                             cucascade::memory::memory_space& mem_space,
                                             rmm::cuda_stream_view stream,
                                             cudf::size_type rows = 10)
{
  filtered_table input{.table = owning_table_view{make_kv_sequence_table(rows, stream)},
                       .state = filter_state::UNFILTERED};
  return ingestible.post_filter_and_project(std::move(input),
                                            mem_space,
                                            stream,
                                            /*like_swar_fastpath=*/false,
                                            /*like_cache=*/nullptr,
                                            /*survivors=*/nullptr,
                                            /*elided=*/{});
}

}  // namespace

//===----------------------------------------------------------------------===//
// post_filter_and_project — gate + fold end to end
//===----------------------------------------------------------------------===//

TEST_CASE("published membership probes fold into the static filter's gather",
          "[scan][duckdb_native_df_fold][dynamic_filter]")
{
  df_fold_test_db env;
  exec_ok(*env.con, "CREATE TABLE t(k BIGINT, v BIGINT)");
  exec_ok(*env.con, "INSERT INTO t SELECT range, range FROM range(0, 10)");
  exec_ok(*env.con, "CHECKPOINT");
  exec_ok(*env.con, "BEGIN TRANSACTION");

  auto mem_mgr    = initialize_memory_manager();
  auto* gpu_space = sirius::scan_test_utils::get_space(*mem_mgr, cucascade::memory::Tier::GPU);
  REQUIRE(gpu_space != nullptr);
  rmm::cuda_stream stream;

  auto channel = std::make_shared<sirius::op::sirius_dynamic_filter_set>();
  REQUIRE(channel->push_filter(0, make_in_list({2, 3, 7}, stream.view())));
  auto ingestible = make_test_ingestible(env, make_v_less_than_6_filter(), channel);

  // Static filter keeps v < 6 ({0..5}); membership keeps k in {2,3,7}; the fold's single
  // gather must land on the conjunction {2,3} — the same rows the downstream operator's
  // cascade would produce.
  auto out = run_post_filter(*ingestible, *gpu_space, stream.view());
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(out->num_columns() == 2);
  REQUIRE(to_host_int64(out->view().column(0), stream.view()) == std::vector<int64_t>{2, 3});
  REQUIRE(to_host_int64(out->view().column(1), stream.view()) == std::vector<int64_t>{2, 3});
}

TEST_CASE("an all-keys membership probe folds to exactly the static-only rows",
          "[scan][duckdb_native_df_fold][dynamic_filter]")
{
  df_fold_test_db env;
  exec_ok(*env.con, "CREATE TABLE t(k BIGINT, v BIGINT)");
  exec_ok(*env.con, "INSERT INTO t SELECT range, range FROM range(0, 10)");
  exec_ok(*env.con, "CHECKPOINT");
  exec_ok(*env.con, "BEGIN TRANSACTION");

  auto mem_mgr    = initialize_memory_manager();
  auto* gpu_space = sirius::scan_test_utils::get_space(*mem_mgr, cucascade::memory::Tier::GPU);
  REQUIRE(gpu_space != nullptr);
  rmm::cuda_stream stream;

  auto channel = std::make_shared<sirius::op::sirius_dynamic_filter_set>();
  REQUIRE(channel->push_filter(0, make_in_list({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, stream.view())));
  auto ingestible = make_test_ingestible(env, make_v_less_than_6_filter(), channel);

  auto out = run_post_filter(*ingestible, *gpu_space, stream.view());
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(to_host_int64(out->view().column(0), stream.view()) ==
          std::vector<int64_t>{0, 1, 2, 3, 4, 5});
}

TEST_CASE("an unpublished channel falls through to the static-only path",
          "[scan][duckdb_native_df_fold][dynamic_filter]")
{
  df_fold_test_db env;
  exec_ok(*env.con, "CREATE TABLE t(k BIGINT, v BIGINT)");
  exec_ok(*env.con, "INSERT INTO t SELECT range, range FROM range(0, 10)");
  exec_ok(*env.con, "CHECKPOINT");
  exec_ok(*env.con, "BEGIN TRANSACTION");

  auto mem_mgr    = initialize_memory_manager();
  auto* gpu_space = sirius::scan_test_utils::get_space(*mem_mgr, cucascade::memory::Tier::GPU);
  REQUIRE(gpu_space != nullptr);
  rmm::cuda_stream stream;

  // Wired but nothing published yet (the mid-scan race the fold must never wait on).
  auto channel    = std::make_shared<sirius::op::sirius_dynamic_filter_set>();
  auto ingestible = make_test_ingestible(env, make_v_less_than_6_filter(), channel);

  auto out = run_post_filter(*ingestible, *gpu_space, stream.view());
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(out->num_columns() == 2);
  REQUIRE(to_host_int64(out->view().column(0), stream.view()) ==
          std::vector<int64_t>{0, 1, 2, 3, 4, 5});
}

TEST_CASE("a channel with no mask-capable filters falls through to the static-only path",
          "[scan][duckdb_native_df_fold][dynamic_filter]")
{
  df_fold_test_db env;
  exec_ok(*env.con, "CREATE TABLE t(k BIGINT, v BIGINT)");
  exec_ok(*env.con, "INSERT INTO t SELECT range, range FROM range(0, 10)");
  exec_ok(*env.con, "CHECKPOINT");
  exec_ok(*env.con, "BEGIN TRANSACTION");

  auto mem_mgr    = initialize_memory_manager();
  auto* gpu_space = sirius::scan_test_utils::get_space(*mem_mgr, cucascade::memory::Tier::GPU);
  REQUIRE(gpu_space != nullptr);
  rmm::cuda_stream stream;

  // A zone-map filter is AST-lowerable but not mask-applicable, so has_filters() is true
  // while the snapshot attaches zero probes.
  auto make_scalar = [&](int64_t v) -> std::unique_ptr<cudf::scalar> {
    return std::make_unique<cudf::numeric_scalar<int64_t>>(
      v, true, stream.view(), cudf::get_current_device_resource_ref());
  };
  std::vector<sirius::op::zone_map_entry> zones;
  zones.push_back({make_scalar(2), make_scalar(7)});
  auto channel = std::make_shared<sirius::op::sirius_dynamic_filter_set>();
  REQUIRE(channel->push_filter(
    0, std::make_shared<sirius::op::sirius_dynamic_zone_map_filter>(std::move(zones))));
  auto ingestible = make_test_ingestible(env, make_v_less_than_6_filter(), channel);

  auto out = run_post_filter(*ingestible, *gpu_space, stream.view());
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(to_host_int64(out->view().column(0), stream.view()) ==
          std::vector<int64_t>{0, 1, 2, 3, 4, 5});
}

TEST_CASE("without a static filter a published channel drops no rows in the scan",
          "[scan][duckdb_native_df_fold][dynamic_filter]")
{
  df_fold_test_db env;
  exec_ok(*env.con, "CREATE TABLE t(k BIGINT, v BIGINT)");
  exec_ok(*env.con, "INSERT INTO t SELECT range, range FROM range(0, 10)");
  exec_ok(*env.con, "CHECKPOINT");
  exec_ok(*env.con, "BEGIN TRANSACTION");

  auto mem_mgr    = initialize_memory_manager();
  auto* gpu_space = sirius::scan_test_utils::get_space(*mem_mgr, cucascade::memory::Tier::GPU);
  REQUIRE(gpu_space != nullptr);
  rmm::cuda_stream stream;

  // The fold is gated on a static filter: with none, the downstream dynamic-filter operator
  // stays the sole membership applier, and a late-materialization consumer keeps seeing an
  // unfiltered batch (the batch IS the chunk).
  auto channel = std::make_shared<sirius::op::sirius_dynamic_filter_set>();
  REQUIRE(channel->push_filter(0, make_in_list({2, 3}, stream.view())));
  auto ingestible = make_test_ingestible(env, /*table_filters=*/nullptr, channel);

  auto out = run_post_filter(*ingestible, *gpu_space, stream.view());
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(out->num_rows() == 10);
}

TEST_CASE("the fold gathers only the output columns when projection is required",
          "[scan][duckdb_native_df_fold][dynamic_filter]")
{
  df_fold_test_db env;
  exec_ok(*env.con, "CREATE TABLE t(k BIGINT, v BIGINT)");
  exec_ok(*env.con, "INSERT INTO t SELECT range, range FROM range(0, 10)");
  exec_ok(*env.con, "CHECKPOINT");
  exec_ok(*env.con, "BEGIN TRANSACTION");

  auto mem_mgr    = initialize_memory_manager();
  auto* gpu_space = sirius::scan_test_utils::get_space(*mem_mgr, cucascade::memory::Tier::GPU);
  REQUIRE(gpu_space != nullptr);
  rmm::cuda_stream stream;

  // output_arity 1: v is a pure-filter column, so the fold's gather must emit only k, at
  // the combined selectivity.
  auto channel = std::make_shared<sirius::op::sirius_dynamic_filter_set>();
  REQUIRE(channel->push_filter(0, make_in_list({2, 3, 7}, stream.view())));
  auto ingestible =
    make_test_ingestible(env, make_v_less_than_6_filter(), channel, /*output_arity=*/1);

  auto out = run_post_filter(*ingestible, *gpu_space, stream.view());
  stream.synchronize();
  REQUIRE(out != nullptr);
  REQUIRE(out->num_columns() == 1);
  REQUIRE(to_host_int64(out->view().column(0), stream.view()) == std::vector<int64_t>{2, 3});
}

//===----------------------------------------------------------------------===//
// fold_membership_probes_into_mask — NULL semantics and probe skips
//===----------------------------------------------------------------------===//

namespace {

/// BOOL8 mask column from host bytes, with rows in @p null_rows nulled.
std::unique_ptr<cudf::column> make_bool8_mask(std::vector<int8_t> const& values,
                                              std::vector<cudf::size_type> const& null_rows,
                                              rmm::cuda_stream_view stream)
{
  auto col = cudf::make_numeric_column(
    cudf::data_type{cudf::type_id::BOOL8},
    static_cast<cudf::size_type>(values.size()),
    null_rows.empty() ? cudf::mask_state::UNALLOCATED : cudf::mask_state::ALL_VALID,
    stream);
  cudaMemcpyAsync(col->mutable_view().head<int8_t>(),
                  values.data(),
                  values.size() * sizeof(int8_t),
                  cudaMemcpyHostToDevice,
                  stream.value());
  for (auto const row : null_rows) {
    cudf::set_null_mask(col->mutable_view().null_mask(), row, row + 1, false, stream);
  }
  col->set_null_count(static_cast<cudf::size_type>(null_rows.size()));
  stream.synchronize();
  return col;
}

}  // namespace

TEST_CASE("fold_membership_probes_into_mask ANDs real probe closures with null-as-drop semantics",
          "[scan][duckdb_native_df_fold][dynamic_filter]")
{
  rmm::cuda_stream stream;
  auto const mr = cudf::get_current_device_resource_ref();

  // Keys [0..4]; static mask [t, NULL, t, f, t]; membership {0,1,2}. Row 0: t AND member ->
  // kept. Row 1: NULL propagates -> dropped. Row 2: t AND member -> kept. Row 3: static
  // false -> dropped. Row 4: t AND miss -> dropped. Exactly the rows a cascade of
  // apply_boolean_mask(static) then apply_boolean_mask(membership) keeps.
  auto keys = make_int64_column({0, 1, 2, 3, 4}, stream.view());
  std::vector<std::unique_ptr<cudf::column>> cols;
  cols.push_back(std::move(keys));
  cudf::table input(std::move(cols));

  sirius::op::sirius_dynamic_filter_set set;
  REQUIRE(set.push_filter(0, make_in_list({0, 1, 2}, stream.view())));
  auto snapshot = snapshot_membership_probes(set, 1);
  REQUIRE(snapshot.attached_probes == 1);

  auto mask = make_bool8_mask({1, 1, 1, 0, 1}, /*null_rows=*/{1}, stream.view());
  auto combined =
    fold_membership_probes_into_mask(std::move(mask), input.view(), snapshot, stream.view(), mr);
  REQUIRE(combined != nullptr);

  auto out = cudf::apply_boolean_mask(input.view(), combined->view(), stream.view(), mr);
  stream.synchronize();
  REQUIRE(to_host_int64(out->view().column(0), stream.view()) == std::vector<int64_t>{0, 2});
}

TEST_CASE("fold_membership_probes_into_mask skips a probe that declines its key column",
          "[scan][duckdb_native_df_fold][dynamic_filter]")
{
  rmm::cuda_stream stream;
  auto const mr = cudf::get_current_device_resource_ref();

  auto keys = make_int64_column({0, 1, 2, 3, 4}, stream.view());
  std::vector<std::unique_ptr<cudf::column>> cols;
  cols.push_back(std::move(keys));
  cudf::table input(std::move(cols));

  // A compute_mask closure returns null for an incompatible probe; the fold must skip it and
  // leave the static mask's verdicts untouched.
  membership_snapshot snapshot;
  snapshot.probes.resize(1);
  snapshot.probes[0].push_back(sirius::membership_probe{
    [](cudf::column_view const&, rmm::cuda_stream_view, rmm::device_async_resource_ref)
      -> std::unique_ptr<cudf::column> { return nullptr; },
    /*selectivity_rank=*/0,
    /*num_keys=*/0});
  snapshot.attached_probes = 1;

  auto mask = make_bool8_mask({1, 0, 1, 0, 1}, /*null_rows=*/{}, stream.view());
  auto combined =
    fold_membership_probes_into_mask(std::move(mask), input.view(), snapshot, stream.view(), mr);
  REQUIRE(combined != nullptr);

  auto out = cudf::apply_boolean_mask(input.view(), combined->view(), stream.view(), mr);
  stream.synchronize();
  REQUIRE(to_host_int64(out->view().column(0), stream.view()) == std::vector<int64_t>{0, 2, 4});
}
