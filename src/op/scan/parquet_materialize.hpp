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

// Turning a set of parquet row-group slices into a cudf table, by whichever
// route the backend serving them prefers.
//
// Kept in a neutral header (like row_group_metadata.hpp) so the scan operator
// and the io benchmarks can share it without either depending on the other's
// headers -- parquet_gpu_ingestible.hpp pulls in DuckDB, which a benchmark has
// no business including.

#include "io/sirius_datasource.hpp"

#include <cudf/io/experimental/hybrid_scan.hpp>
#include <cudf/io/parquet.hpp>
#include <cudf/io/parquet_schema.hpp>
#include <cudf/io/text/byte_range_info.hpp>
#include <cudf/table/table.hpp>
#include <cudf/types.hpp>

#include <rmm/resource_ref.hpp>

#include <cuda/stream>

#include <memory>
#include <span>
#include <vector>

namespace sirius::op::scan {

/// One file's contribution to a split: where to read it from, its parsed
/// footer, and which row groups are wanted.  Mirrors the fields of
/// @c row_group_slice that materialization actually needs, without dragging in
/// the estimate/accounting ones.
struct parquet_source {
  std::shared_ptr<io::sirius_datasource> datasource;
  std::shared_ptr<cudf::io::parquet::FileMetaData const> metadata;
  std::vector<cudf::size_type> row_group_indices;
};

/// Column-chunk byte ranges a read fetches for @p row_group_indices, honoring
/// @p options' column projection.  Empty when there are no row groups.
[[nodiscard]] std::vector<cudf::io::text::byte_range_info> column_chunk_ranges(
  cudf::io::parquet::FileMetaData const& metadata,
  cudf::io::parquet_reader_options const& options,
  std::vector<cudf::size_type> const& row_group_indices);

/// @ref column_chunk_ranges over many row-group subsets, keeping the reader of
/// the last file and reader options it was asked about.
///
/// Building a reader copies the whole footer, so a file split into many
/// batches costs one copy instead of one per batch. The kept reader holds that
/// copy until the next file, or until @ref clear.
class column_chunk_range_cache {
 public:
  /// Same result as @ref column_chunk_ranges for @p metadata, @p options and
  /// @p row_group_indices.
  [[nodiscard]] std::vector<cudf::io::text::byte_range_info> ranges(
    std::shared_ptr<cudf::io::parquet::FileMetaData const> const& metadata,
    std::shared_ptr<cudf::io::parquet_reader_options const> const& options,
    std::vector<cudf::size_type> const& row_group_indices);

  /// Release the kept reader and its footer copy.
  void clear() noexcept;

 private:
  std::shared_ptr<cudf::io::parquet::FileMetaData const> _metadata;
  std::shared_ptr<cudf::io::parquet_reader_options const> _options;
  std::unique_ptr<cudf::io::parquet::experimental::hybrid_scan_reader> _reader;
};

/// Materialize @p sources with the caller's current device, stream and allocator. Eligible sources
/// use one vectored read per file and hybrid decode; others use `cudf::io::read_parquet`. Bulk
/// validates all original schemas, prunes with the final filter, prepares all destinations before
/// issuance, and drains every submitted read before propagating an error or releasing destinations.
/// No failed bulk read is retried through the general reader.
///
/// @param ranges Column-chunk ranges in source order. Unfiltered bulk reads may reuse them only
/// when metadata, row groups and projection match those used to derive them. Pass an empty span
/// after changing any of these. Unless every source has a nonempty entry, all ranges are derived;
/// filtered reads always recompute ranges after final statistics pruning. The caller retains the
/// filter AST until this call returns.
[[nodiscard]] std::unique_ptr<cudf::table> materialize_parquet(
  std::span<parquet_source const> sources,
  cudf::io::parquet_reader_options const& options,
  std::span<std::vector<cudf::io::text::byte_range_info> const> ranges,
  ::cuda::stream_ref stream,
  rmm::device_async_resource_ref mr);

/// Whether every source has metadata, selected groups and either bulk I/O preference or a
/// prefetching cache, and @p options contains only default decoding settings plus optional name
/// projection/filter. Inspects options before any read publication; copying custom schema settings
/// may allocate.
[[nodiscard]] bool prefers_bulk_materialize(std::span<parquet_source const> sources,
                                            cudf::io::parquet_reader_options const& options);

/// Throw unless every entry of @p sources has the same footer schema as the
/// first, compared as cudf::io::read_parquet compares several sources.  The
/// multi-file hybrid scan reader skips that check, so the bulk route calls
/// this first.  Each source must have non-null metadata.
void require_same_parquet_schema(std::span<parquet_source const> sources);

}  // namespace sirius::op::scan
