/*
 * Copyright 2025, Sirius Contributors.
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

// Deterministic unit tests for host-side parquet row-group pruning
// (op::scan::prune_row_groups_by_stats). Builds FileMetaData by hand with known min/max statistics
// and checks that row groups are pruned iff a DuckDB TableFilter is provably unsatisfiable, across
// the integer / decimal / date / string decode paths, the multi-column position mapping (the case
// where a filter must read its own column's stats, not another's), and the conservative fallbacks.

#include <catch.hpp>

#include <op/scan/row_group_stats_pruner.hpp>

#include <cudf/io/parquet_schema.hpp>

#include <duckdb/common/enums/expression_type.hpp>
#include <duckdb/common/types.hpp>
#include <duckdb/common/types/value.hpp>
#include <duckdb/planner/filter/constant_filter.hpp>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

using cudf::io::parquet::ColumnChunk;
using cudf::io::parquet::FileMetaData;
using cudf::io::parquet::RowGroup;
using cudf::io::parquet::Type;
using stat = std::vector<uint8_t>;
using sirius::op::scan::prune_row_groups_by_stats;
using sirius::op::scan::stats_prune_filter;

template <typename T>
stat le(T v)
{
  stat b(sizeof(T));
  std::memcpy(b.data(), &v, sizeof(T));
  return b;
}

stat str_stat(std::string const& s) { return stat(s.begin(), s.end()); }

ColumnChunk make_chunk(std::string name, Type type, std::optional<stat> min_value,
                       std::optional<stat> max_value)
{
  ColumnChunk c;
  c.meta_data.path_in_schema    = {std::move(name)};
  c.meta_data.type              = type;
  c.meta_data.statistics.min_value = std::move(min_value);
  c.meta_data.statistics.max_value = std::move(max_value);
  return c;
}

// One file, one column per chunk vector entry, `chunks_per_rg` repeated for each row group.
FileMetaData make_meta(std::vector<std::vector<ColumnChunk>> row_groups)
{
  FileMetaData meta;
  for (auto& cols : row_groups) {
    RowGroup rg;
    rg.columns = std::move(cols);
    meta.row_groups.push_back(std::move(rg));
  }
  return meta;
}

std::vector<cudf::size_type> all_rgs(FileMetaData const& m)
{
  std::vector<cudf::size_type> v(m.row_groups.size());
  for (cudf::size_type i = 0; i < static_cast<cudf::size_type>(v.size()); ++i) { v[i] = i; }
  return v;
}

}  // namespace

TEST_CASE("prune_row_groups_by_stats: integer range filter prunes only out-of-range groups",
          "[scan][row_group_pruner]")
{
  // rg0 values in [0,10]; rg1 values in [0,100]. Filter: x >= 50.
  auto meta = make_meta({
    {make_chunk("x", Type::INT32, le<int32_t>(0), le<int32_t>(10))},
    {make_chunk("x", Type::INT32, le<int32_t>(0), le<int32_t>(100))},
  });
  duckdb::ConstantFilter filter(duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
                                duckdb::Value::INTEGER(50));
  std::vector<stats_prune_filter> filters{{"x", duckdb::LogicalType::INTEGER, &filter}};

  auto const kept = prune_row_groups_by_stats(meta, all_rgs(meta), filters);
  REQUIRE(kept == std::vector<cudf::size_type>{1});  // rg0 pruned (max 10 < 50), rg1 kept
}

TEST_CASE("prune_row_groups_by_stats: DECIMAL(15,2) INT64-backed filter", "[scan][row_group_pruner]")
{
  // l_quantity-like DECIMAL(15,2) stored as INT64 unscaled. rg0 in [1.00,50.00], rg1 in [30.00,50.00].
  // Filter: q < 24.00  (raw < 2400). rg0 keeps (min 1.00 < 24), rg1 prunes (min 30.00 >= 24).
  auto const dtype = duckdb::LogicalType::DECIMAL(15, 2);
  auto meta        = make_meta({
    {make_chunk("q", Type::INT64, le<int64_t>(100), le<int64_t>(5000))},
    {make_chunk("q", Type::INT64, le<int64_t>(3000), le<int64_t>(5000))},
  });
  duckdb::ConstantFilter filter(duckdb::ExpressionType::COMPARE_LESSTHAN,
                                duckdb::Value::DECIMAL(int64_t{2400}, 15, 2));
  std::vector<stats_prune_filter> filters{{"q", dtype, &filter}};

  auto const kept = prune_row_groups_by_stats(meta, all_rgs(meta), filters);
  REQUIRE(kept == std::vector<cudf::size_type>{0});
}

TEST_CASE("prune_row_groups_by_stats: DATE filter", "[scan][row_group_pruner]")
{
  // rg0 days [0,100], rg1 days [9000,9100]. Filter: d >= day 9000.
  auto meta = make_meta({
    {make_chunk("d", Type::INT32, le<int32_t>(0), le<int32_t>(100))},
    {make_chunk("d", Type::INT32, le<int32_t>(9000), le<int32_t>(9100))},
  });
  duckdb::ConstantFilter filter(duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
                                duckdb::Value::DATE(duckdb::date_t(9000)));
  std::vector<stats_prune_filter> filters{{"d", duckdb::LogicalType::DATE, &filter}};

  auto const kept = prune_row_groups_by_stats(meta, all_rgs(meta), filters);
  REQUIRE(kept == std::vector<cudf::size_type>{1});
}

TEST_CASE("prune_row_groups_by_stats: VARCHAR filter", "[scan][row_group_pruner]")
{
  // rg0 strings in [A,C], rg1 in [A,Z]. Filter: s = 'M'.
  auto meta = make_meta({
    {make_chunk("s", Type::BYTE_ARRAY, str_stat("A"), str_stat("C"))},
    {make_chunk("s", Type::BYTE_ARRAY, str_stat("A"), str_stat("Z"))},
  });
  duckdb::ConstantFilter filter(duckdb::ExpressionType::COMPARE_EQUAL, duckdb::Value("M"));
  std::vector<stats_prune_filter> filters{{"s", duckdb::LogicalType::VARCHAR, &filter}};

  auto const kept = prune_row_groups_by_stats(meta, all_rgs(meta), filters);
  REQUIRE(kept == std::vector<cudf::size_type>{1});  // 'M' > 'C' so rg0 pruned
}

TEST_CASE("prune_row_groups_by_stats: filter reads its own column's stats (position mapping)",
          "[scan][row_group_pruner]")
{
  // Two columns per row group: 'a' then 'b'. The filter is on 'b'. 'a' carries stats that WOULD
  // prune if mis-read; 'b' carries stats that keep. Correct behaviour: keep (read b, not a).
  // This is the regression guard for the column-position bug (matching list position, not schema_idx).
  auto meta = make_meta({
    {make_chunk("a", Type::INT32, le<int32_t>(0), le<int32_t>(0)),       // a: all 0
     make_chunk("b", Type::INT32, le<int32_t>(0), le<int32_t>(100))},    // b: [0,100]
  });
  duckdb::ConstantFilter filter(duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
                                duckdb::Value::INTEGER(50));
  std::vector<stats_prune_filter> on_b{{"b", duckdb::LogicalType::INTEGER, &filter}};
  REQUIRE(prune_row_groups_by_stats(meta, all_rgs(meta), on_b) ==
          std::vector<cudf::size_type>{0});  // kept (b's max 100 >= 50)

  // Same filter value but on 'a' (all 0): a's max 0 < 50 -> pruned. Proves stats are column-specific.
  std::vector<stats_prune_filter> on_a{{"a", duckdb::LogicalType::INTEGER, &filter}};
  REQUIRE(prune_row_groups_by_stats(meta, all_rgs(meta), on_a).empty());
}

TEST_CASE("prune_row_groups_by_stats: conservative when stats absent or filters empty",
          "[scan][row_group_pruner]")
{
  auto meta = make_meta({
    {make_chunk("x", Type::INT32, std::nullopt, std::nullopt)},  // no modern stats
  });
  duckdb::ConstantFilter filter(duckdb::ExpressionType::COMPARE_GREATERTHANOREQUALTO,
                                duckdb::Value::INTEGER(50));

  // No stats -> cannot prove unsatisfiable -> keep.
  std::vector<stats_prune_filter> filters{{"x", duckdb::LogicalType::INTEGER, &filter}};
  REQUIRE(prune_row_groups_by_stats(meta, all_rgs(meta), filters) ==
          std::vector<cudf::size_type>{0});

  // No filters -> identity.
  REQUIRE(prune_row_groups_by_stats(meta, all_rgs(meta), {}) == all_rgs(meta));
}
