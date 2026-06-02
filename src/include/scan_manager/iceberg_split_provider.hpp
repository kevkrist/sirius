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

#pragma once

#include "op/scan/iceberg_metadata_reader.hpp"
#include "op/scan/post_convert_fn.hpp"
#include "scan_manager/parquet_split_provider.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace sirius::scan_manager {

/**
 * @brief Split provider for Iceberg tables on the GPU-native scan path.
 *
 * Wraps @ref parquet_split_provider with two iceberg-specific behaviors:
 *
 *  1. Force-projects equality-delete key columns into the scan when they
 *     would otherwise be pruned by the planner.  The original output arity
 *     is preserved via @c scan_output_arity so @c assemble_scan_output drops
 *     the extras after the delete-filter hook has used them.
 *
 *  2. Builds a delete-filter @c scan_post_decode_hook_t once at construction
 *     (positional + equality + V3 deletion vectors, as configured by
 *     @c IcebergDeleteData) and attaches it to every emitted
 *     @c parquet_scan_data.  The gpu parquet scan operator invokes it after
 *     the parquet decoder produces a cudf::table and before output assembly.
 *
 * Splits are forced to a single data file each (@c max_file_processed = 1)
 * so the operator can attribute a batch's @c first_row to one absolute file
 * offset, which the positional-delete filter requires.
 */
class iceberg_split_provider : public parquet_split_provider {
 public:
  iceberg_split_provider(
    duckdb::vector<sirius::logical_type> const& returned_types,
    std::vector<std::string> const& file_paths,
    duckdb::vector<duckdb::ColumnIndex> const& column_ids,
    duckdb::vector<duckdb::idx_t> const& projection_ids,
    duckdb::vector<std::string> const& names,
    std::size_t scan_output_arity,
    duckdb::unique_ptr<duckdb::TableFilterSet> table_filter_set,
    duckdb::vector<duckdb::HivePartitioningIndex> const& partition_indices,
    std::size_t approximate_batch_size,
    sirius_scan_manager& scan_manager,
    std::unordered_map<int, std::shared_ptr<sirius::io::sirius_ioctx>> const& gpu_ioctxs,
    std::shared_ptr<const op::scan::IcebergDeleteData> delete_data);

  ~iceberg_split_provider() override;

  iceberg_split_provider(iceberg_split_provider const&)            = delete;
  iceberg_split_provider& operator=(iceberg_split_provider const&) = delete;
  iceberg_split_provider(iceberg_split_provider&&)                 = delete;
  iceberg_split_provider& operator=(iceberg_split_provider&&)      = delete;

  /// Override that wraps the base's callable so each emitted
  /// @c parquet_scan_data gets its @c post_decode_hook set to the shared
  /// pipeline.  When @c _hook is null (V1 table or no deletes survived
  /// resolution) this falls through to the base behavior.
  std::function<std::vector<std::unique_ptr<op::operator_data>>()> next_split_provider() override;

 private:
  /// Pre-resolved projection inputs.  Carries the (possibly widened)
  /// @c column_ids / @c projection_ids that the base @c parquet_split_provider
  /// consumes, plus the schema indices that index every selected column for
  /// equality-delete key matching.
  struct widened {
    duckdb::vector<duckdb::ColumnIndex> column_ids;
    duckdb::vector<duckdb::idx_t> projection_ids;
    /// Source-schema primary indices, in the order @c parquet_split_provider's
    /// @c scan_plan adds them to @c data_columns — i.e. unique primary indices
    /// in @c projection_ids order (or @c column_ids order when
    /// @c projection_ids is empty).
    std::vector<std::size_t> selected_schema_indices;
  };

  /// Compute widened column_ids / projection_ids that force the equality-delete
  /// key columns into the read.  Returns the original arrays unchanged when
  /// @p delete_data is null / has no equality deletes / all keys are already
  /// covered.
  static widened widen_projection(
    duckdb::vector<duckdb::ColumnIndex> const& column_ids,
    duckdb::vector<duckdb::idx_t> const& projection_ids,
    duckdb::vector<std::string> const& names,
    op::scan::IcebergDeleteData const* delete_data);

  /// Build the post-decode hook for this provider.  Reads the first data
  /// file's footer once to extract the @c field_id → batch_position map for
  /// equality-delete key resolution.  Returns null when @p delete_data is
  /// empty (V1 / no surviving filters).
  static op::scan::scan_post_decode_hook_t build_hook(
    std::vector<std::string> const& file_paths,
    duckdb::vector<std::string> const& names,
    std::vector<std::size_t> const& selected_schema_indices,
    sirius_scan_manager& scan_manager,
    std::unordered_map<int, std::shared_ptr<sirius::io::sirius_ioctx>> const& gpu_ioctxs,
    std::shared_ptr<const op::scan::IcebergDeleteData> const& delete_data);

  /// Private delegating constructor — the public ctor forwards the
  /// pre-computed @c widened so it can be threaded into the base ctor's
  /// member-init-list AND retained for hook construction in the body.
  iceberg_split_provider(
    duckdb::vector<sirius::logical_type> const& returned_types,
    std::vector<std::string> const& file_paths,
    widened w,
    duckdb::vector<std::string> const& names,
    std::size_t scan_output_arity,
    duckdb::unique_ptr<duckdb::TableFilterSet> table_filter_set,
    duckdb::vector<duckdb::HivePartitioningIndex> const& partition_indices,
    std::size_t approximate_batch_size,
    sirius_scan_manager& scan_manager,
    std::unordered_map<int, std::shared_ptr<sirius::io::sirius_ioctx>> const& gpu_ioctxs,
    std::shared_ptr<const op::scan::IcebergDeleteData> delete_data);

  /// The composed delete-filter hook.  Null when nothing to filter (V1 table
  /// or every group failed key resolution); behaves as plain parquet scan.
  op::scan::scan_post_decode_hook_t _hook;
};

}  // namespace sirius::scan_manager
