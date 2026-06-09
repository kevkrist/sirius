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

//===----------------------------------------------------------------------===//
// Late-materialization (selective) string-decode correctness.
//
// For each codec, decode a realistic multi-segment column two ways:
//   (a) full decode of every row (the reference), and
//   (b) selective decode driven by a sorted selection vector.
// Assert that the compacted column (b) equals (a) gathered by the selection,
// i.e. b[i] == a[d_sel[i]] for all i. This is the invariant the scan late-mat
// path relies on: decode-with-selection == decode-then-gather.
//===----------------------------------------------------------------------===//

#include "scan/decode_test_utils.hpp"
#include "scan/strings_multiseg_synth.hpp"

#include <cudf/column/column.hpp>
#include <cudf/strings/strings_column_view.hpp>

#include <rmm/cuda_stream.hpp>
#include <rmm/device_buffer.hpp>
#include <rmm/mr/cuda_async_memory_resource.hpp>

#include <cuda/scan/gpu_decode_strings.cuh>

#include <catch.hpp>
#include <duckdb/common/enums/compression_type.hpp>

#include <cstdint>
#include <string>
#include <vector>

using duckdb::CompressionType;
using sirius::cuda::scan::gpu_decode_strings_column;
using sirius::cuda::scan::string_decode_selection;
using sirius::test::decode::download;
using sirius::test::decode::upload;
using sirius::test::decode::strings::build_string_column;

namespace {

/// Read back a cudf strings column to host strings.
std::vector<std::string> to_host_strings(cudf::column const& column, rmm::cuda_stream_view stream)
{
  cudf::strings_column_view scv(column.view());
  auto const n = static_cast<uint32_t>(column.size());
  std::vector<std::string> out(n);
  if (n == 0) return out;
  auto offsets = download<int32_t>(scv.offsets().data<int32_t>(), n + 1, stream.value());
  if (scv.chars_size(stream) > 0) {
    auto chars = download<uint8_t>(scv.chars_begin(stream), scv.chars_size(stream), stream.value());
    for (uint32_t i = 0; i < n; ++i) {
      out[i].assign(reinterpret_cast<char const*>(chars.data() + offsets[i]),
                    static_cast<size_t>(offsets[i + 1] - offsets[i]));
    }
  }
  return out;
}

/// Deterministic sorted selection vector keeping roughly every `stride`-th row
/// (plus a fixed jitter), simulating a filter that passes ~1/stride of rows.
std::vector<uint32_t> make_selection(uint32_t total_rows, uint32_t stride)
{
  std::vector<uint32_t> sel;
  if (stride == 0) return sel;
  for (uint32_t i = 0; i < total_rows; ++i) {
    // Pseudo-random but deterministic membership; keeps ~1/stride of rows.
    uint64_t h = (static_cast<uint64_t>(i) * 0x9E3779B97F4A7C15ull) >> 40;
    if (stride == 1 || (h % stride) == 0) sel.push_back(i);
  }
  return sel;
}

/// Decode `cols` fully, then selectively for each selection, asserting
/// selective[i] == full[sel[i]].
///
/// `unique_rows` forces every row distinct — required for DICT_FSST FSST_ONLY
/// (mode 2), which DuckDB only emits for all-distinct segments.
void check_codec(CompressionType codec, uint8_t dict_fsst_mode = 1, bool unique_rows = false)
{
  rmm::cuda_stream stream;
  rmm::mr::cuda_async_memory_resource mr;

  // ~40 K rows of TPC-H-like comments => many segments + multi-CTA chunks.
  uint32_t const rows = 40000u;
  std::vector<std::string> synth(rows);
  for (uint32_t i = 0; i < rows; ++i) {
    synth[i] = sirius::test::decode::strings::make_tpch_like_comment(
      static_cast<uint64_t>(i) * 0x9E3779B97F4A7C15ull + 7u, 5u, 150u);
    if (unique_rows) { synth[i] += " #" + std::to_string(i); }  // guarantee distinctness
  }

  auto built = build_string_column(synth, codec, stream.view(), dict_fsst_mode);

  // Reference: full decode (no selection).
  auto full_col  = gpu_decode_strings_column(built.col, stream.view(), mr);
  auto full_host = to_host_strings(*full_col, stream.view());
  REQUIRE(full_host.size() == rows);

  for (uint32_t stride : {1u, 2u, 5u, 50u}) {
    auto sel = make_selection(rows, stride);
    INFO("codec=" << static_cast<int>(codec) << " mode=" << static_cast<int>(dict_fsst_mode)
                  << " stride=" << stride << " selected=" << sel.size());
    auto d_sel = upload(sel, stream.view());
    string_decode_selection selection{static_cast<uint32_t const*>(d_sel.data()),
                                       static_cast<uint32_t>(sel.size()),
                                       /*active=*/true};
    auto sel_col  = gpu_decode_strings_column(built.col, stream.view(), mr, selection);
    auto sel_host = to_host_strings(*sel_col, stream.view());

    REQUIRE(sel_host.size() == sel.size());
    for (size_t i = 0; i < sel.size(); ++i) {
      REQUIRE(sel_host[i] == full_host[sel[i]]);
    }
  }

  // Empty selection (0 survivors) => empty column. This mirrors the scan path
  // exactly: when nothing survives the filter, the compacted index column has
  // zero rows and its device pointer is NULL, so d_sel is null while the
  // selection is still active. `active=true` (not the pointer) must drive the
  // empty result without dereferencing d_sel.
  {
    string_decode_selection selection{/*d_sel=*/nullptr, /*num_selected=*/0u, /*active=*/true};
    auto sel_col = gpu_decode_strings_column(built.col, stream.view(), mr, selection);
    REQUIRE(sel_col->size() == 0);
  }
}

}  // namespace

TEST_CASE("selective decode UNCOMPRESSED matches decode-then-gather",
          "[scan][decode][strings][selective]")
{
  check_codec(CompressionType::COMPRESSION_UNCOMPRESSED);
}

TEST_CASE("selective decode DICTIONARY matches decode-then-gather",
          "[scan][decode][strings][selective]")
{
  check_codec(CompressionType::COMPRESSION_DICTIONARY);
}

TEST_CASE("selective decode FSST matches decode-then-gather", "[scan][decode][strings][selective]")
{
  check_codec(CompressionType::COMPRESSION_FSST);
}

TEST_CASE("selective decode DICT_FSST mode 0 matches decode-then-gather",
          "[scan][decode][strings][selective]")
{
  check_codec(CompressionType::COMPRESSION_DICT_FSST, /*mode=*/0);
}

TEST_CASE("selective decode DICT_FSST mode 1 matches decode-then-gather",
          "[scan][decode][strings][selective]")
{
  check_codec(CompressionType::COMPRESSION_DICT_FSST, /*mode=*/1);
}

TEST_CASE("selective decode DICT_FSST mode 2 matches decode-then-gather",
          "[scan][decode][strings][selective]")
{
  check_codec(CompressionType::COMPRESSION_DICT_FSST, /*mode=*/2, /*unique_rows=*/true);
}
