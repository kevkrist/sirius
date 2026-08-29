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

//! @file
//! Public entry for the GPU-native DuckDB scan decode path. Takes per-column
//! descriptors that point at on-device segment bytes and decodes them into a
//! `cudf::table`. The dispatcher does no I/O — the caller stages segment bytes
//! on device first. Codec metadata (bitpacking mode, dictionary refs, FSST
//! symbol table, ...) lives inside the segment bytes; per-codec kernels parse
//! their own headers.

#include "cuda/scan/decode_descriptor_arena.cuh"

#include <cudf/table/table.hpp>
#include <cudf/types.hpp>

#include <rmm/cuda_stream_view.hpp>
#include <rmm/resource_ref.hpp>

#include <duckdb/common/enums/compression_type.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace sirius::cuda::scan {

/// One DuckDB segment, already staged on device.
///
/// `d_bytes`     device pointer to the segment's raw bytes.
/// `bytes_size`  size of the buffer behind `d_bytes`. Used as a sanity bound
///               by codec kernels (e.g. CONSTANT requires at least one full
///               value of bytes).
/// `row_offset`  starting row index of this segment within the column. The
///               column's segments tile a `[0, total_rows)` row range.
/// `row_count`   number of rows this segment produces.
struct gpu_segment_desc {
  const uint8_t* d_bytes;
  uint32_t bytes_size;
  uint32_t row_offset;
  uint32_t row_count;
};

/// A run of segments that share the same codec. Per-codec kernels consume the
/// full `segments` vector in one launch (this is the batching unit). A column
/// may have multiple runs if its segments use different codecs.
struct gpu_codec_run {
  duckdb::CompressionType codec;
  std::vector<gpu_segment_desc> segments;
};

/// Inputs for one column. `data` and `validity` are runs grouped by codec.
/// Fits codecs whose state is per-segment.
struct gpu_column_decode_input {
  cudf::data_type out_type;
  std::vector<gpu_codec_run> data;
  std::vector<gpu_codec_run> validity;
  uint32_t total_rows;
  bool has_nulls;
};

/// Deferred kernel work for one prepared codec run. `prepare_*` functions build and stage a run's
/// descriptors into a `decode_descriptor_arena` and return one of these; `launch` enqueues the
/// run's kernels after the arena has been flushed. Concrete launchers own any device working
/// buffers whose pointers were baked into the staged descriptors.
class decode_run_launcher {
 public:
  virtual ~decode_run_launcher()                                                       = default;
  virtual void launch(rmm::cuda_stream_view stream, rmm::device_async_resource_ref mr) = 0;
};

/// A table decode split into its two phases: `prepare_table_decode` validates the inputs,
/// allocates the output buffers, builds every codec run's descriptors, and stages them into the
/// caller's `decode_descriptor_arena`; `launch` then enqueues all decode kernels and assembles the
/// `cudf::table`. The split exists so `decode_duckdb_native_split` can pack the descriptors of
/// every column in a scan split (fixed-width, varchar, and array children) into one arena and
/// upload them with a single host-to-device copy before any kernel runs.
class prepared_table_decode {
 public:
  prepared_table_decode(prepared_table_decode&&) noexcept;
  prepared_table_decode& operator=(prepared_table_decode&&) noexcept;
  ~prepared_table_decode();

  /// Enqueues every prepared run's kernels and builds the table. The arena passed to
  /// `prepare_table_decode` must have been flushed (throws `std::logic_error` otherwise) and must
  /// outlive the enqueued kernels. Synchronises the stream once for the batched null count, or
  /// not at all when no column staged validity runs (their null counts are host-known zero).
  std::unique_ptr<cudf::table> launch(rmm::cuda_stream_view stream,
                                      rmm::device_async_resource_ref mr);

 private:
  friend prepared_table_decode prepare_table_decode(std::span<gpu_column_decode_input const> cols,
                                                    decode_descriptor_arena& arena,
                                                    rmm::cuda_stream_view stream,
                                                    rmm::device_async_resource_ref mr);
  struct impl;
  explicit prepared_table_decode(std::unique_ptr<impl> state);
  std::unique_ptr<impl> _impl;
};

/// Phase one of a table decode (see `prepared_table_decode`). Throws `std::runtime_error` on
/// viability violations (unsupported codec, non-fixed-width type, malformed validity offset, row
/// count > `cudf::size_type` max); callers are expected to pre-filter unsupported columns, so
/// these throws are a defensive backstop, not the primary gate.
prepared_table_decode prepare_table_decode(std::span<gpu_column_decode_input const> cols,
                                           decode_descriptor_arena& arena,
                                           rmm::cuda_stream_view stream,
                                           rmm::device_async_resource_ref mr);

/// Decode every column in `cols` into a `cudf::table` in one call: prepares against a private
/// arena, flushes it, and launches. All device work runs on `stream`; columns come back with
/// their `null_count` populated. Allocations go through `mr`.
std::unique_ptr<cudf::table> gpu_decode_table(std::vector<gpu_column_decode_input> const& cols,
                                              rmm::cuda_stream_view stream,
                                              rmm::device_async_resource_ref mr);

}  // namespace sirius::cuda::scan
