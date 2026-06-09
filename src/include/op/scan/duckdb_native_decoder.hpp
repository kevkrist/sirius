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

#pragma once

// sirius
#include <io/io_context.hpp>  // sirius_ioctx + sirius_io_object
#include <op/scan/duckdb_native_metadata.hpp>

// cudf
#include <cudf/column/column.hpp>
#include <cudf/table/table.hpp>
#include <cudf/table/table_view.hpp>

// rmm
#include <rmm/cuda_stream_view.hpp>

// cucascade
#include <cucascade/memory/memory_space.hpp>

// standard library
#include <functional>
#include <memory>
#include <vector>

namespace sirius::op::scan {

class duckdb_native_ingestible_table_info;

//===----------------------------------------------------------------------===//
// duckdb_native_split_payload
//===----------------------------------------------------------------------===//
/**
 * @brief Per-split payload consumed by @ref decode_duckdb_native_split.
 *
 * Built by @c duckdb_native_gpu_ingestible::next_split_provider from a
 * contiguous run of the walker's @c duckdb_row_group_metadata. The
 * @ref table_info pointer aliases the ingestible's owned bind data and is
 * valid for the lifetime of every emitted split (the ingestible outlives
 * its splits).
 */
struct duckdb_native_split_payload {
  std::vector<duckdb_row_group_metadata> row_groups;
  /// Stable alias into the ingestible's owned bind data. Carries
  /// @c storage / @c context / @c projected_cols / @c projected_types that
  /// the decoder reads.
  duckdb_native_ingestible_table_info const* table_info = nullptr;
  /// Sirius IO substrate handles. Set by the regular (non-cached) split
  /// path so the decoder can read .db blocks via @c sirius_ioctx::host_read
  /// instead of going through DuckDB's BufferManager. Both null when the
  /// scan_manager runs without sirius_datasource (decoder falls back to
  /// BufferManager).
  std::shared_ptr<sirius::io::sirius_ioctx> io_ctx;
  std::shared_ptr<sirius::io::sirius_io_object> db_io_object;
  /// Per-split late-materialization decision (query-level viability AND the
  /// split clears late_materialize_min_rows). Set by next_split_provider;
  /// materialize_table builds the selection plan and reports
  /// filter_state::ROW_FILTERED iff this is true, keeping the post-decode filter
  /// (apply_filter) in lockstep.
  bool late_materialize = false;
};

//===----------------------------------------------------------------------===//
// late_materialization_plan
//===----------------------------------------------------------------------===//
/**
 * @brief Drives the decoder's late-materialization path.
 *
 * When a non-null plan is passed to @ref decode_duckdb_native_split, the
 * decoder: (1) decodes the fixed-width columns, (2) calls @ref evaluate_mask to
 * obtain a boolean row filter, (3) compacts the surviving rows into a selection
 * vector and gathers the fixed-width (+ rowid) columns, and (4) decodes the
 * varchar columns straight into compacted form via the in-kernel selection
 * path (@ref sirius::cuda::scan::gpu_decode_strings_column with a selection) —
 * so the decode + chars-write work for filtered-out rows is never done.
 *
 * The returned table holds only the surviving rows, in projected-column order
 * (no projection-down; the caller drops any trailing filter-only columns).
 */
struct late_materialization_plan {
  /**
   * @brief Evaluate the pushed-down filter and return a BOOL8 keep-mask.
   *
   * The argument is the decoded fixed-width columns as a table_view, in decode
   * order: the fixed-width projected columns in projected order, excluding
   * varchar and rowid columns. The closure's filter references columns by that
   * dense fixed-width index (the caller builds the filter to match). The
   * result must be a BOOL8 column with one row per input row (true == keep).
   */
  std::function<std::unique_ptr<cudf::column>(cudf::table_view)> evaluate_mask;
};

//===----------------------------------------------------------------------===//
// decode_duckdb_native_split
//===----------------------------------------------------------------------===//
/// Decode a single split (contiguous run of row-group metadata from the
/// walker) into a cudf::table. Supported: fixed-width data (UNCOMPRESSED /
/// RLE / BITPACKING / CONSTANT), varchar data (UNCOMPRESSED / DICTIONARY /
/// FSST / DICT_FSST), validity (UNCOMPRESSED / EMPTY / CONSTANT / ROARING),
/// rowid synthesis. Throws std::runtime_error on any codec the walker
/// accepted but this decoder does not implement.
///
/// When @p lm is non-null, the varchar columns are late-materialized against
/// the filter it carries (see @ref late_materialization_plan); the returned
/// table then contains only the surviving rows. When null, every row of every
/// projected column is decoded (the default eager path).
std::unique_ptr<cudf::table> decode_duckdb_native_split(
  duckdb_native_split_payload const& split,
  cucascade::memory::memory_space& mem_space,
  rmm::cuda_stream_view stream,
  late_materialization_plan const* lm = nullptr);

}  // namespace sirius::op::scan
