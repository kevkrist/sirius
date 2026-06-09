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

//===----------------------------------------------------------------------===//
// Build a realistic multi-segment varchar column (DuckDB on-disk format,
// staged on device) from a vector<string>, for any of the supported string
// codecs. Shared by the selective-decode verify test and the late-mat
// microbench. The returned `built_string_column` owns the device storage and
// keeps the original strings as ground truth.
//===----------------------------------------------------------------------===//

#include "scan/strings_synth.hpp"

#include <cuda/scan/gpu_decode_strings.cuh>

#include <rmm/cuda_stream_view.hpp>
#include <rmm/device_buffer.hpp>

#include <cuda_runtime.h>
#include <duckdb/common/enums/compression_type.hpp>

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sirius::test::decode::strings {

/// A decode-ready varchar column plus the device buffer backing its segments.
struct built_string_column {
  rmm::device_buffer storage;  ///< owns every segment's bytes (must outlive use)
  ::sirius::cuda::scan::gpu_string_column_decode_input col;
};

/// Split rows into ~`rows_per_seg`-row plain-DICTIONARY segments, each with a
/// per-chunk local dictionary (entry 0 reserved NULL). Mirrors
/// `make_dict_fsst_segments_chunked` but for the plain DICTIONARY codec.
inline std::vector<std::pair<std::vector<uint8_t>, uint32_t>> make_dict_segments_chunked(
  std::vector<std::string> const& strings, uint32_t rows_per_seg = 4096u)
{
  std::vector<std::pair<std::vector<uint8_t>, uint32_t>> out;
  uint32_t const total_rows = static_cast<uint32_t>(strings.size());
  for (uint32_t i = 0; i < total_rows; i += rows_per_seg) {
    uint32_t const n = std::min(rows_per_seg, total_rows - i);
    std::unordered_map<std::string, uint32_t> dict_map;
    std::vector<std::string> dict;
    dict.emplace_back();  // index 0 = NULL slot
    std::vector<uint32_t> selections(n);
    for (uint32_t r = 0; r < n; ++r) {
      auto const& s = strings[i + r];
      auto it       = dict_map.find(s);
      uint32_t idx;
      if (it == dict_map.end()) {
        idx          = static_cast<uint32_t>(dict.size());
        dict_map[s]  = idx;
        dict.push_back(s);
      } else {
        idx = it->second;
      }
      selections[r] = idx;
    }
    out.emplace_back(make_dict_segment(dict, selections), n);
  }
  return out;
}

/// Split rows into fixed-row UNCOMPRESSED segments.
inline std::vector<std::pair<std::vector<uint8_t>, uint32_t>> make_uncompressed_segments_chunked(
  std::vector<std::string> const& strings, uint32_t rows_per_seg = 4096u)
{
  std::vector<std::pair<std::vector<uint8_t>, uint32_t>> out;
  uint32_t const total_rows = static_cast<uint32_t>(strings.size());
  for (uint32_t i = 0; i < total_rows; i += rows_per_seg) {
    uint32_t const n = std::min(rows_per_seg, total_rows - i);
    std::vector<std::string> slice(strings.begin() + i, strings.begin() + i + n);
    out.emplace_back(make_uncompressed_segment(slice), n);
  }
  return out;
}

/// Build a decode-ready column from `strings` under `codec`. For DICT_FSST,
/// `dict_fsst_mode` selects 0=DICTIONARY / 1=DICT_FSST / 2=FSST_ONLY.
inline built_string_column build_string_column(std::vector<std::string> const& strings,
                                               duckdb::CompressionType codec,
                                               rmm::cuda_stream_view stream,
                                               uint8_t dict_fsst_mode = 1)
{
  using duckdb::CompressionType;

  // Produce (segment_bytes, row_count) blobs in the requested codec.
  std::vector<std::pair<std::vector<uint8_t>, uint32_t>> seg_blobs;
  switch (codec) {
    case CompressionType::COMPRESSION_UNCOMPRESSED:
      seg_blobs = make_uncompressed_segments_chunked(strings);
      break;
    case CompressionType::COMPRESSION_DICTIONARY:
      seg_blobs = make_dict_segments_chunked(strings);
      break;
    case CompressionType::COMPRESSION_FSST: seg_blobs = make_fsst_segments_chunked(strings); break;
    case CompressionType::COMPRESSION_DICT_FSST:
      seg_blobs = make_dict_fsst_segments_chunked(strings, dict_fsst_mode);
      break;
    default: throw std::runtime_error("build_string_column: unsupported codec");
  }

  // Per-segment max decoded string length (the walker's upper bound), computed
  // from the original rows so the orchestrator's chars sizing has a valid bound.
  built_string_column out;
  constexpr size_t SEG_ALIGN = 16;  // matches the decoder's device-segment alignment
  std::vector<size_t> seg_offsets(seg_blobs.size());
  size_t alloc_bytes = 0;
  for (size_t k = 0; k < seg_blobs.size(); ++k) {
    seg_offsets[k] = alloc_bytes;
    alloc_bytes += (seg_blobs[k].first.size() + SEG_ALIGN - 1) & ~(SEG_ALIGN - 1);
  }
  out.storage        = rmm::device_buffer(alloc_bytes > 0 ? alloc_bytes : 1u, stream);
  auto const* d_base = static_cast<uint8_t const*>(out.storage.data());

  std::vector<::sirius::cuda::scan::gpu_string_segment_desc> segs;
  segs.reserve(seg_blobs.size());
  uint32_t row_cursor   = 0;
  uint32_t total_rows   = 0;
  for (size_t k = 0; k < seg_blobs.size(); ++k) {
    auto const& [bytes, rc] = seg_blobs[k];
    uint32_t max_len        = 0;
    for (uint32_t r = 0; r < rc; ++r) {
      max_len = std::max(max_len, static_cast<uint32_t>(strings[row_cursor + r].size()));
    }
    cudaMemcpyAsync(const_cast<uint8_t*>(d_base) + seg_offsets[k],
                    bytes.data(),
                    bytes.size(),
                    cudaMemcpyHostToDevice,
                    stream.value());
    segs.push_back({d_base + seg_offsets[k],
                    static_cast<uint32_t>(bytes.size()),
                    row_cursor,
                    rc,
                    /*seg_row_start=*/0u,
                    /*max_string_length=*/max_len});
    row_cursor += rc;
    total_rows += rc;
  }
  cudaStreamSynchronize(stream.value());

  out.col.total_rows = total_rows;
  out.col.has_nulls  = false;
  out.col.data.push_back({codec, std::move(segs)});
  return out;
}

}  // namespace sirius::test::decode::strings
