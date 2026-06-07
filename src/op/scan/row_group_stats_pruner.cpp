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

#include "op/scan/row_group_stats_pruner.hpp"

#include "op/scan/parquet_schema_mapping.hpp"

#include <duckdb/common/enums/filter_propagate_result.hpp>
#include <duckdb/common/types.hpp>
#include <duckdb/common/types/date.hpp>
#include <duckdb/common/types/string_type.hpp>
#include <duckdb/common/types/value.hpp>
#include <duckdb/storage/statistics/base_statistics.hpp>
#include <duckdb/storage/statistics/numeric_stats.hpp>
#include <duckdb/storage/statistics/string_stats.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace sirius::op::scan {

namespace {

using cudf::io::parquet::FileMetaData;
using cudf::io::parquet::Type;
using stat_bytes = std::vector<uint8_t>;

// Parquet fixed-width statistics are stored little-endian; Sirius targets little-endian hosts
// (x86_64), so a raw copy reproduces the value. (Mirrors DuckDB's `Load<T>` in
// extension/parquet/parquet_statistics.cpp.)
static_assert(std::endian::native == std::endian::little,
              "row_group_stats_pruner assumes a little-endian host for parquet stat decoding");

template <typename T>
[[nodiscard]] T load_le(stat_bytes const& bytes)
{
  T value{};
  std::memcpy(&value, bytes.data(), sizeof(T));
  return value;
}

// Decode raw parquet min/max stat bytes into a typed DuckDB `Value` of `type`, mirroring the safe
// subset of DuckDB's `ParquetStatisticsUtils::ConvertValueInternal`. Returns a NULL `Value` for any
// type/encoding we don't model or any size mismatch — the caller then leaves that bound unset,
// which only ever forgoes a pruning opportunity (never prunes incorrectly). Strings are handled by
// the caller via `StringStats`, not here.
[[nodiscard]] duckdb::Value decode_numeric_stat(duckdb::LogicalType const& type,
                                                Type physical_type, stat_bytes const& bytes)
{
  using duckdb::Value;
  using id = duckdb::LogicalTypeId;

  duckdb::Value decoded;
  switch (type.id()) {
    case id::BOOLEAN:
      if (bytes.size() == sizeof(bool)) { decoded = Value::BOOLEAN(bytes[0] != 0); }
      break;
    case id::TINYINT:
    case id::SMALLINT:
    case id::INTEGER:  // parquet stores TINYINT/SMALLINT/INTEGER stats as physical INT32
      if (bytes.size() == sizeof(int32_t)) { decoded = Value::INTEGER(load_le<int32_t>(bytes)); }
      break;
    case id::BIGINT:
      if (bytes.size() == sizeof(int64_t)) { decoded = Value::BIGINT(load_le<int64_t>(bytes)); }
      break;
    case id::UTINYINT:
    case id::USMALLINT:
    case id::UINTEGER:
      if (bytes.size() == sizeof(uint32_t)) { decoded = Value::UINTEGER(load_le<uint32_t>(bytes)); }
      break;
    case id::UBIGINT:
      if (bytes.size() == sizeof(uint64_t)) { decoded = Value::UBIGINT(load_le<uint64_t>(bytes)); }
      break;
    case id::FLOAT:
      if (bytes.size() == sizeof(float)) {
        auto const value = load_le<float>(bytes);
        if (Value::FloatIsFinite(value)) { decoded = Value::FLOAT(value); }
      }
      break;
    case id::DOUBLE:
      if (bytes.size() == sizeof(double)) {
        auto const value = load_le<double>(bytes);
        if (Value::DoubleIsFinite(value)) { decoded = Value::DOUBLE(value); }
      }
      break;
    case id::DATE:  // parquet DATE is days since epoch in a physical INT32
      if (bytes.size() == sizeof(int32_t)) {
        decoded = Value::DATE(duckdb::date_t(load_le<int32_t>(bytes)));
      }
      break;
    case id::DECIMAL: {
      // Only INT32/INT64-backed decimals are decoded here; FIXED_LEN_BYTE_ARRAY / BYTE_ARRAY
      // big-endian decimals are left unmodelled (kept) to avoid a fragile variable-length decode.
      auto const width = duckdb::DecimalType::GetWidth(type);
      auto const scale = duckdb::DecimalType::GetScale(type);
      if (physical_type == Type::INT32 && bytes.size() == sizeof(int32_t)) {
        decoded = Value::DECIMAL(load_le<int32_t>(bytes), width, scale);
      } else if (physical_type == Type::INT64 && bytes.size() == sizeof(int64_t)) {
        decoded = Value::DECIMAL(load_le<int64_t>(bytes), width, scale);
      }
      break;
    }
    default:
      break;  // timestamps, nested, etc. — unmodelled, conservatively keep
  }

  if (decoded.IsNull()) { return decoded; }
  // Normalize to the column's exact type (e.g. INTEGER literal -> TINYINT column). A failed cast
  // (out-of-range) yields NULL -> bound unset -> no pruning, which is safe.
  duckdb::Value casted;
  std::string error;
  if (decoded.DefaultTryCastAs(type, casted, &error)) { return casted; }
  return duckdb::Value();
}

[[nodiscard]] duckdb::string_t as_string_t(stat_bytes const& bytes)
{
  return duckdb::string_t(reinterpret_cast<char const*>(bytes.data()),
                          static_cast<uint32_t>(bytes.size()));
}

// Build a DuckDB `BaseStatistics` for one column chunk from its modern (`min_value`/`max_value`)
// parquet statistics. Returns std::nullopt when no usable bound can be produced (the deprecated
// signed-byte `min`/`max` are intentionally ignored — their sort order is wrong for several types,
// which could over-prune). A bound is set only for the side that decodes successfully, so partial
// stats still drive one-sided comparisons.
[[nodiscard]] std::optional<duckdb::BaseStatistics> build_column_stats(
  duckdb::LogicalType const& type, Type physical_type,
  cudf::io::parquet::Statistics const& parquet_stats)
{
  if (!parquet_stats.min_value.has_value() && !parquet_stats.max_value.has_value()) {
    return std::nullopt;
  }

  auto const tid           = type.id();
  bool const is_string_col = tid == duckdb::LogicalTypeId::VARCHAR ||
                             tid == duckdb::LogicalTypeId::BLOB;

  if (is_string_col) {
    auto stats = duckdb::StringStats::CreateUnknown(type);
    if (parquet_stats.min_value.has_value()) {
      duckdb::StringStats::SetMin(stats, as_string_t(*parquet_stats.min_value));
    }
    if (parquet_stats.max_value.has_value()) {
      duckdb::StringStats::SetMax(stats, as_string_t(*parquet_stats.max_value));
    }
    return stats;
  }

  auto stats        = duckdb::NumericStats::CreateUnknown(type);
  bool any_bound_set = false;
  if (parquet_stats.min_value.has_value()) {
    auto const value = decode_numeric_stat(type, physical_type, *parquet_stats.min_value);
    if (!value.IsNull()) {
      duckdb::NumericStats::SetMin(stats, value);
      any_bound_set = true;
    }
  }
  if (parquet_stats.max_value.has_value()) {
    auto const value = decode_numeric_stat(type, physical_type, *parquet_stats.max_value);
    if (!value.IsNull()) {
      duckdb::NumericStats::SetMax(stats, value);
      any_bound_set = true;
    }
  }
  if (!any_bound_set) { return std::nullopt; }
  return stats;
}

// A filter resolved to a concrete column-chunk position for one file. `chunk_pos` indexes
// `RowGroup::columns` (the value `leaf_indices_for_column` returns — a position in the column-chunk
// list, NOT a flattened-schema `schema_idx`). Column-chunk order is consistent across row groups
// within a file, so one position resolved from the first row group applies to all; `column_name`
// is re-verified per row group as a cheap safety check.
struct resolved_filter {
  std::size_t chunk_pos;
  std::string const* column_name;
  duckdb::LogicalType const* type;
  duckdb::TableFilter const* filter;
};

}  // namespace

std::vector<cudf::size_type> prune_row_groups_by_stats(
  FileMetaData const& metadata, std::vector<cudf::size_type> const& input_row_groups,
  std::vector<stats_prune_filter> const& filters)
{
  if (filters.empty() || input_row_groups.empty()) { return input_row_groups; }

  // Resolve each filter to a single leaf schema index in THIS file. Columns that are missing or
  // map to more than one leaf (nested) can't be pruned here and are dropped from consideration.
  std::vector<resolved_filter> resolved;
  resolved.reserve(filters.size());
  for (auto const& f : filters) {
    auto const leaves = detail::leaf_indices_for_column(metadata, f.column_name);
    if (leaves.size() != 1) { continue; }
    resolved.push_back(resolved_filter{leaves.front(), &f.column_name, &f.column_type, f.filter});
  }
  if (resolved.empty()) { return input_row_groups; }

  std::vector<cudf::size_type> kept;
  kept.reserve(input_row_groups.size());
  for (auto const rg : input_row_groups) {
    auto const& row_group = metadata.row_groups[static_cast<std::size_t>(rg)];
    bool prune            = false;
    for (auto const& rf : resolved) {
      if (rf.chunk_pos >= row_group.columns.size()) { continue; }
      auto const& chunk = row_group.columns[rf.chunk_pos];
      // Safety: column-chunk order is assumed stable within a file; verify the resolved position
      // still names the expected column before trusting its statistics.
      auto const& path = chunk.meta_data.path_in_schema;
      if (path.empty() || path.front() != *rf.column_name) { continue; }

      auto stats = build_column_stats(*rf.type, chunk.meta_data.type, chunk.meta_data.statistics);
      if (!stats.has_value()) { continue; }

      if (rf.filter->CheckStatistics(*stats) ==
          duckdb::FilterPropagateResult::FILTER_ALWAYS_FALSE) {
        prune = true;
        break;
      }
    }
    if (!prune) { kept.push_back(rg); }
  }
  return kept;
}

}  // namespace sirius::op::scan
