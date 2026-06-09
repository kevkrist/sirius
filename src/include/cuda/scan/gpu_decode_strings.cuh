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

// String-codec decode entry for the GPU-native scan path. Sibling to
// `gpu_decode_table` for fixed-width columns. Codecs: DICTIONARY, FSST,
// DICT_FSST. Headers parse on device; FSST symbol tables go through the
// opaque `duckdb_fsst_import` on host before upload.

#include "cuda/scan/gpu_native_decode.cuh"

#include <cudf/column/column.hpp>

#include <rmm/cuda_stream_view.hpp>
#include <rmm/resource_ref.hpp>

#include <duckdb/common/enums/compression_type.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace sirius::cuda::scan {

/// Per-segment descriptor. Mirrors `gpu_segment_desc` plus `seg_row_start`
/// for the chunking path. Codec-specific metadata is parsed on device.
struct gpu_string_segment_desc {
  uint8_t const* d_bytes;      ///< device pointer to the segment's first byte
  uint32_t bytes_size;         ///< size of the buffer behind d_bytes
  uint32_t row_offset;         ///< column-relative starting row index
  uint32_t row_count;          ///< rows produced by this segment (or chunk slice)
  uint32_t seg_row_start;      ///< rows skipped at the segment head when chunked
  uint32_t max_string_length;  ///< DuckDB segment-stat upper bound on decoded
                               ///  chars per row; 0 = unknown (forces a sync)
};

/// A run of segments sharing one codec.
struct gpu_string_codec_run {
  duckdb::CompressionType codec;
  std::vector<gpu_string_segment_desc> segments;
};

/// Inputs for one varchar column. Mixed-codec columns share one prefix sum
/// across all runs. `validity` is UNCOMPRESSED-only, matching the fixed-
/// width dispatcher's contract.
struct gpu_string_column_decode_input {
  std::vector<gpu_string_codec_run> data;
  std::vector<gpu_codec_run> validity;
  uint32_t total_rows;
  bool has_nulls;
};

/// Optional late-materialization selection for @ref gpu_decode_strings_column.
///
/// When `d_sel != nullptr`, the decoder emits ONLY the `num_selected` rows
/// whose column-relative global indices appear in `d_sel` (which must be
/// sorted ascending and in-range `[0, col.total_rows)`), producing a
/// compacted strings column of exactly `num_selected` rows. The selection is
/// applied inside the decode kernels at the Pass-1 length computation — the
/// earliest point in the pipeline — so the chars buffer is sized to and only
/// ever holds the selected rows' bytes. Unselected rows are never decoded,
/// gathered, or counted.
///
/// This is the kernel-level half of scan late materialization: decode the
/// filter columns, build `d_sel` from the surviving rows, then materialize
/// the (non-filter) string columns straight into compacted form, skipping the
/// decode + gather work for filtered-out rows.
///
/// When `active == false` the call decodes all `col.total_rows` rows exactly
/// as before (identity selection); this is the default.
///
/// `active` — not `d_sel != nullptr` — gates the selection, because an empty
/// selection (num_selected == 0, nothing survived the filter) legitimately
/// carries a null `d_sel`: the scan derives `d_sel` from a compacted index
/// column whose device pointer is null when it has zero rows. Inferring
/// activeness from the pointer would alias that empty-but-active case onto the
/// decode-all default and wrongly materialize every row.
struct string_decode_selection {
  uint32_t const* d_sel  = nullptr;  ///< device: sorted selected global row indices
  uint32_t num_selected  = 0;        ///< length of d_sel == output row count
  bool active            = false;    ///< true => apply the selection (even if empty)
};

/// Decode one varchar column to a cudf strings column. Async modulo at most
/// one host sync (the chars-buffer sizing read-back, which only fires when
/// the per-segment length upper bound is unknown or pathological — or always,
/// for the selection path, which sizes chars to the exact selected total).
/// Throws on malformed segment metadata or unsupported codecs.
///
/// @param selection  Optional late-materialization selection (see
///                   @ref string_decode_selection). Default = decode all rows.
std::unique_ptr<cudf::column> gpu_decode_strings_column(
  gpu_string_column_decode_input const& col,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr,
  string_decode_selection const& selection = {});

}  // namespace sirius::cuda::scan
