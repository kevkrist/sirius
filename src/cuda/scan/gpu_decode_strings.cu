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

//===----------------------------------------------------------------------===//
// String column decode — orchestrator.
//
// A VARCHAR column decodes in two phases. prepare_strings_column_decode runs
// the per-codec prepares and stages their descriptors into the caller's
// decode_descriptor_arena:
//   prepare_*         build per-run descriptors (some also run on-device prep,
//                     e.g. FSST symbol tables, DICT_FSST dictionary predecode)
// After the arena is flushed, prepared_strings_column_decode::launch drives
// the shared two-phase flow:
//   1. each codec writes its rows' lengths into a single d_lengths array
//      (launch_*_lengths)
//   2. one exclusive scan turns d_lengths into the column's d_offsets
//   3. each codec gathers its bytes into d_chars at those offsets
//      (launch_*_gather)
// then the cudf strings column is assembled (offsets + chars + null mask).
// gpu_decode_strings_column wraps both phases around a private arena.
//
// Each codec's on-disk segment layout is documented at the top of its file
// (strings/<codec>.cu).
//===----------------------------------------------------------------------===//

#include "cuda/scan/gpu_decode_strings.cuh"
#include "cuda/scan/strings/common.cuh"
#include "cuda/scan/strings/dict_fsst.cuh"
#include "cuda/scan/strings/dictionary.cuh"
#include "cuda/scan/strings/fsst.cuh"
#include "cuda/scan/strings/uncompressed.cuh"

#include <cudf/column/column.hpp>
#include <cudf/column/column_factories.hpp>
#include <cudf/null_mask.hpp>
#include <cudf/types.hpp>

#include <rmm/detail/error.hpp>
#include <rmm/device_buffer.hpp>
#include <rmm/device_uvector.hpp>

#include <cub/cub.cuh>
#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sirius::cuda::scan {

namespace {

/// Overlays an UNCOMPRESSED validity run onto the null mask (sibling to
/// `prepare_validity_run` in gpu_native_decode.cu).
void overlay_validity_run(gpu_codec_run const& run, uint8_t* d_mask, rmm::cuda_stream_view stream)
{
  if (run.codec != duckdb::CompressionType::COMPRESSION_UNCOMPRESSED) {
    throw std::runtime_error(
      "gpu_decode_strings_column: viability invariant violated — "
      "validity codec " +
      std::to_string(static_cast<int>(run.codec)) + " not implemented");
  }
  for (auto const& seg : run.segments) {
    if (seg.row_count == 0) continue;
    if (seg.row_offset % 8 != 0) {
      throw std::runtime_error("gpu_decode_strings_column: validity row_offset (" +
                               std::to_string(seg.row_offset) + ") not byte-aligned");
    }
    auto const bytes  = ::cuda::ceil_div(seg.row_count, 8);
    auto const offset = seg.row_offset / 8;
    if (seg.bytes_size < bytes) {
      throw std::runtime_error("gpu_decode_strings_column: validity segment bytes_size (" +
                               std::to_string(seg.bytes_size) + ") < required " +
                               std::to_string(bytes));
    }
    RMM_CUDA_TRY(cudaMemcpyAsync(
      d_mask + offset, seg.d_bytes, bytes, cudaMemcpyDeviceToDevice, stream.value()));
  }
}

}  // namespace

struct prepared_strings_column_decode::impl {
  decode_descriptor_arena const* arena = nullptr;
  uint32_t total_rows                  = 0;
  bool has_nulls                       = false;
  std::vector<gpu_codec_run> validity;  ///< copied from the input for the launch-phase overlay

  // Staged descriptor slots. Counts double as the kernel launch sizes.
  arena_slot uncomp_chunks;
  arena_slot dict_chunks_short;
  arena_slot dict_chunks_long;
  arena_slot dict_fsst_chunks;
  arena_slot fsst_length_descs;
  arena_slot fsst_gather_chunks;
  arena_slot fsst_row_starts;
  arena_slot dict_fsst_descs;
  arena_slot dict_fsst_decoders;
  arena_slot dict_fsst_byte_offsets;
  arena_slot dict_fsst_decoded_offsets;

  uint32_t total_fsst_row_count  = 0;
  bool any_inline_nulls          = false;
  uint32_t total_predecode_bytes = 0;
  size_t cum_chars_upper         = 0;
  bool needs_exact_total         = false;
};

prepared_strings_column_decode::prepared_strings_column_decode(std::unique_ptr<impl> state)
  : _impl(std::move(state))
{
}
prepared_strings_column_decode::prepared_strings_column_decode(
  prepared_strings_column_decode&&) noexcept = default;
prepared_strings_column_decode& prepared_strings_column_decode::operator=(
  prepared_strings_column_decode&&) noexcept                      = default;
prepared_strings_column_decode::~prepared_strings_column_decode() = default;

/// Phase one (see file banner): aggregate per-codec prepared state and stage every kernel
/// descriptor array into @p arena. DICT_FSST's `prepare_dict_fsst` keeps its inherent on-device
/// header/results round trips (with their own syncs); only its resulting host descriptor arrays
/// join the packed blob.
prepared_strings_column_decode prepare_strings_column_decode(
  gpu_string_column_decode_input const& col,
  decode_descriptor_arena& arena,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr)
{
  auto state   = std::make_unique<prepared_strings_column_decode::impl>();
  state->arena = &arena;

  uint32_t const total_rows = col.total_rows;
  if (total_rows == 0) { return prepared_strings_column_decode{std::move(state)}; }
  if (total_rows > static_cast<uint32_t>(std::numeric_limits<cudf::size_type>::max())) {
    throw std::runtime_error("gpu_decode_strings_column: total_rows (" +
                             std::to_string(total_rows) + ") > cudf::size_type max");
  }
  state->total_rows = total_rows;
  state->has_nulls  = col.has_nulls;
  state->validity   = col.validity;

  prepared_uncomp prep_uncomp;
  prepared_dict prep_dict;
  prepared_fsst prep_fsst;
  prepared_dict_fsst prep_dict_fsst;
  prep_dict_fsst.any_inline_nulls      = false;
  prep_dict_fsst.total_predecode_bytes = 0;
  size_t cum_chars_upper               = 0;
  bool needs_exact_total               = false;
  for (auto const& run : col.data) {
    switch (run.codec) {
      case duckdb::CompressionType::COMPRESSION_DICTIONARY: {
        auto p = prepare_dict(run);
        prep_dict.descs_short.insert(
          prep_dict.descs_short.end(), p.descs_short.begin(), p.descs_short.end());
        prep_dict.descs_long.insert(
          prep_dict.descs_long.end(), p.descs_long.begin(), p.descs_long.end());
        break;
      }
      case duckdb::CompressionType::COMPRESSION_FSST: {
        auto p = prepare_fsst(run);
        // Rebase row_starts + decoder indices into the merged FSST set.
        auto const row_count_base     = prep_fsst.total_fsst_row_count;
        auto const decoder_count_base = static_cast<uint32_t>(prep_fsst.decoders.size());
        for (auto& s : p.row_starts) {
          s += row_count_base;
        }
        for (auto& c : p.gather_chunks) {
          c.fsst_row_start += row_count_base;
          c.seg_decoder_idx += decoder_count_base;
        }
        prep_fsst.length_descs.insert(
          prep_fsst.length_descs.end(), p.length_descs.begin(), p.length_descs.end());
        prep_fsst.row_starts.insert(
          prep_fsst.row_starts.end(), p.row_starts.begin(), p.row_starts.end());
        prep_fsst.decoders.insert(prep_fsst.decoders.end(), p.decoders.begin(), p.decoders.end());
        prep_fsst.gather_chunks.insert(
          prep_fsst.gather_chunks.end(), p.gather_chunks.begin(), p.gather_chunks.end());
        prep_fsst.total_fsst_row_count += p.total_fsst_row_count;
        break;
      }
      case duckdb::CompressionType::COMPRESSION_DICT_FSST: {
        auto p                    = prepare_dict_fsst(run, stream, mr);
        auto const bo_base        = static_cast<uint32_t>(prep_dict_fsst.byte_offsets.size());
        auto const dec_base       = static_cast<uint32_t>(prep_dict_fsst.decoders.size());
        auto const predecode_base = prep_dict_fsst.total_predecode_bytes;
        for (auto& d : p.descs) {
          d.seg_dict_offset_base += bo_base;
          d.seg_decoder_idx += dec_base;
          if (d.mode == DICT_FSST_MODE_DICT_FSST) { d.predecode_seg_offset += predecode_base; }
        }
        prep_dict_fsst.byte_offsets.insert(
          prep_dict_fsst.byte_offsets.end(), p.byte_offsets.begin(), p.byte_offsets.end());
        prep_dict_fsst.decoded_offsets.insert(
          prep_dict_fsst.decoded_offsets.end(), p.decoded_offsets.begin(), p.decoded_offsets.end());
        prep_dict_fsst.decoders.insert(
          prep_dict_fsst.decoders.end(), p.decoders.begin(), p.decoders.end());
        prep_dict_fsst.descs.insert(prep_dict_fsst.descs.end(), p.descs.begin(), p.descs.end());
        prep_dict_fsst.any_inline_nulls = prep_dict_fsst.any_inline_nulls || p.any_inline_nulls;
        prep_dict_fsst.total_predecode_bytes += p.total_predecode_bytes;
        break;
      }
      case duckdb::CompressionType::COMPRESSION_UNCOMPRESSED: {
        auto p = prepare_uncomp(run);
        prep_uncomp.descs.insert(prep_uncomp.descs.end(), p.descs.begin(), p.descs.end());
        break;
      }
      default:
        throw std::runtime_error(
          "gpu_decode_strings_column: viability invariant violated — "
          "data codec " +
          std::to_string(static_cast<int>(run.codec)) + " not implemented");
    }
    // Upper-bound from walker stats; 0 means unknown → take the sync path.
    for (auto const& seg : run.segments) {
      if (seg.max_string_length == 0u) {
        needs_exact_total = true;
        continue;
      }
      cum_chars_upper += size_t{seg.row_count} * seg.max_string_length;
    }
  }

  // Per-row kernels take chunked descriptors; predecode + mark_nulls stay
  // per-segment via prep_dict_fsst.descs.
  auto const target_ctas       = get_target_ctas();
  auto const uncomp_chunks     = expand_chunks(prep_uncomp.descs, target_ctas);
  auto const dict_chunks_short = expand_chunks(prep_dict.descs_short, target_ctas);
  auto const dict_chunks_long  = expand_chunks(prep_dict.descs_long, target_ctas);
  auto const dict_fsst_chunks  = expand_chunks(prep_dict_fsst.descs, target_ctas);

  state->uncomp_chunks     = arena.stage(uncomp_chunks.data(), uncomp_chunks.size());
  state->dict_chunks_short = arena.stage(dict_chunks_short.data(), dict_chunks_short.size());
  state->dict_chunks_long  = arena.stage(dict_chunks_long.data(), dict_chunks_long.size());
  state->dict_fsst_chunks  = arena.stage(dict_fsst_chunks.data(), dict_fsst_chunks.size());
  state->fsst_length_descs =
    arena.stage(prep_fsst.length_descs.data(), prep_fsst.length_descs.size());
  state->fsst_gather_chunks =
    arena.stage(prep_fsst.gather_chunks.data(), prep_fsst.gather_chunks.size());
  state->fsst_row_starts = arena.stage(prep_fsst.row_starts.data(), prep_fsst.row_starts.size());
  state->dict_fsst_descs = arena.stage(prep_dict_fsst.descs.data(), prep_dict_fsst.descs.size());
  state->dict_fsst_decoders =
    arena.stage(prep_dict_fsst.decoders.data(), prep_dict_fsst.decoders.size());
  state->dict_fsst_byte_offsets =
    arena.stage(prep_dict_fsst.byte_offsets.data(), prep_dict_fsst.byte_offsets.size());
  state->dict_fsst_decoded_offsets =
    arena.stage(prep_dict_fsst.decoded_offsets.data(), prep_dict_fsst.decoded_offsets.size());

  state->total_fsst_row_count  = prep_fsst.total_fsst_row_count;
  state->any_inline_nulls      = prep_dict_fsst.any_inline_nulls;
  state->total_predecode_bytes = prep_dict_fsst.total_predecode_bytes;
  state->cum_chars_upper       = cum_chars_upper;
  state->needs_exact_total     = needs_exact_total;

  return prepared_strings_column_decode{std::move(state)};
}

std::unique_ptr<cudf::column> prepared_strings_column_decode::launch(
  rmm::cuda_stream_view stream, rmm::device_async_resource_ref mr)
{
  auto& state               = *_impl;
  uint32_t const total_rows = state.total_rows;
  if (total_rows == 0) { return cudf::make_empty_column(cudf::data_type{cudf::type_id::STRING}); }
  if (!state.arena->flushed()) {
    throw std::logic_error(
      "prepared_strings_column_decode::launch before the descriptor arena was flushed");
  }

  // Allocate output and intermediate buffers. The FSST decoder array is a device-built scratch
  // output (kernel_build_fsst_decoders fills it from each segment's symbol-table bytes), so it is
  // allocated here rather than staged in the arena; one slot per FSST length descriptor.
  rmm::device_uvector<uint32_t> d_lengths(size_t{total_rows} + 1, stream, mr);
  rmm::device_uvector<int32_t> d_offsets(size_t{total_rows} + 1, stream, mr);
  rmm::device_buffer d_comp_offsets(state.total_fsst_row_count * sizeof(uint32_t), stream, mr);
  rmm::device_buffer d_fsst_decoders_buf(
    state.fsst_length_descs.count * sizeof(fsst_decoder_compact), stream, mr);

  // Descriptor device pointers resolve into the flushed arena blob. No host
  // sync is needed before the kernels consume them: the sources are pinned
  // arena memory whose reuse is gated on the upload's completion event.
  auto* d_comp_offsets_p        = static_cast<uint32_t*>(d_comp_offsets.data());
  auto const* d_uncomp_chunks_p = state.arena->device_ptr<string_chunk_desc>(state.uncomp_chunks);
  auto const* d_dict_short_p = state.arena->device_ptr<string_chunk_desc>(state.dict_chunks_short);
  auto const* d_dict_long_p  = state.arena->device_ptr<string_chunk_desc>(state.dict_chunks_long);
  auto const* d_fsst_lengths_p =
    state.arena->device_ptr<string_chunk_desc>(state.fsst_length_descs);
  auto const* d_fsst_chunks_p = state.arena->device_ptr<fsst_chunk_desc>(state.fsst_gather_chunks);
  auto const* d_fsst_starts_p = state.arena->device_ptr<uint32_t>(state.fsst_row_starts);
  auto* d_fsst_decs_p         = static_cast<fsst_decoder_compact*>(d_fsst_decoders_buf.data());
  auto const* d_dict_fsst_p   = state.arena->device_ptr<dict_fsst_desc>(state.dict_fsst_descs);
  auto const* d_dict_fsst_chunks_p =
    state.arena->device_ptr<dict_fsst_desc>(state.dict_fsst_chunks);
  auto const* d_dict_fsst_decs_p =
    state.arena->device_ptr<fsst_decoder_compact>(state.dict_fsst_decoders);
  auto const* d_byte_off_p    = state.arena->device_ptr<uint32_t>(state.dict_fsst_byte_offsets);
  auto const* d_decoded_off_p = state.arena->device_ptr<uint32_t>(state.dict_fsst_decoded_offsets);

  // Pass 1: lengths. Same kernel for short/long DICTIONARY — only gather forks.
  launch_uncomp_lengths(
    d_uncomp_chunks_p, d_lengths.data(), static_cast<uint32_t>(state.uncomp_chunks.count), stream);
  launch_dict_lengths(
    d_dict_short_p, d_lengths.data(), static_cast<uint32_t>(state.dict_chunks_short.count), stream);
  launch_dict_lengths(
    d_dict_long_p, d_lengths.data(), static_cast<uint32_t>(state.dict_chunks_long.count), stream);
  launch_fsst_lengths(d_fsst_decs_p,
                      d_comp_offsets_p,
                      d_lengths.data(),
                      d_fsst_lengths_p,
                      d_fsst_starts_p,
                      d_fsst_chunks_p,
                      static_cast<uint32_t>(state.fsst_length_descs.count),
                      static_cast<uint32_t>(state.fsst_gather_chunks.count),
                      stream);
  // Predecode buffer holds decoded dict bytes for mode-1 segments.
  rmm::device_buffer d_predecode_buf(
    state.total_predecode_bytes > 0 ? state.total_predecode_bytes : 1u, stream, mr);
  auto* d_predecode_p = static_cast<uint8_t*>(d_predecode_buf.data());

  // Lengths chunk for SM-fill; predecode stays per-segment (one decode/dict).
  launch_dict_fsst_lengths(d_dict_fsst_chunks_p,
                           d_lengths.data(),
                           d_decoded_off_p,
                           static_cast<uint32_t>(state.dict_fsst_chunks.count),
                           stream);
  launch_dict_fsst_predecode(d_dict_fsst_p,
                             d_byte_off_p,
                             d_decoded_off_p,
                             d_dict_fsst_decs_p,
                             d_predecode_p,
                             static_cast<uint32_t>(state.dict_fsst_descs.count),
                             state.total_predecode_bytes,
                             stream);

  // Prefix-sum lengths → byte offsets per row.
  size_t cub_bytes  = 0;
  auto const scan_n = static_cast<int>(total_rows) + 1;
  cub::DeviceScan::ExclusiveSum(nullptr,
                                cub_bytes,
                                d_lengths.data(),
                                reinterpret_cast<uint32_t*>(d_offsets.data()),
                                scan_n,
                                stream.value());
  rmm::device_buffer cub_temp_buf(cub_bytes, stream, mr);
  cub::DeviceScan::ExclusiveSum(cub_temp_buf.data(),
                                cub_bytes,
                                d_lengths.data(),
                                reinterpret_cast<uint32_t*>(d_offsets.data()),
                                scan_n,
                                stream.value());

  // cudf strings offsets are int32; reject up front if the upper bound exceeds it.
  constexpr auto INT32_MAX_SIZE = static_cast<size_t>(std::numeric_limits<int32_t>::max());
  if (!state.needs_exact_total && state.cum_chars_upper > INT32_MAX_SIZE) {
    throw std::runtime_error("gpu_decode_strings_column: estimated total_chars (" +
                             std::to_string(state.cum_chars_upper) + ") exceeds int32 max");
  }
  size_t alloc_chars = 0;
  if (!state.needs_exact_total && state.cum_chars_upper <= HOST_UPPER_BOUND_LIMIT) {
    alloc_chars = state.cum_chars_upper;
  } else {
    RMM_CUDA_TRY(cudaStreamSynchronize(stream.value()));
    uint32_t total_chars_u = 0;
    RMM_CUDA_TRY(cudaMemcpy(
      &total_chars_u, d_offsets.data() + total_rows, sizeof(uint32_t), cudaMemcpyDeviceToHost));
    if (total_chars_u > static_cast<uint32_t>(INT32_MAX_SIZE)) {
      throw std::runtime_error("gpu_decode_strings_column: total_chars (" +
                               std::to_string(total_chars_u) + ") exceeds int32 max");
    }
    alloc_chars = total_chars_u;
  }

  rmm::device_buffer d_chars(alloc_chars > 0 ? alloc_chars : 1u, stream, mr);
  auto* d_chars_p = static_cast<uint8_t*>(d_chars.data());

  // Pass 2: gather. See DICT_WARP_COOP_MIN_LEN for the partition rationale.
  launch_uncomp_gather(d_uncomp_chunks_p,
                       d_offsets.data(),
                       d_chars_p,
                       static_cast<uint32_t>(state.uncomp_chunks.count),
                       stream);
  launch_dict_gather_short(d_dict_short_p,
                           d_offsets.data(),
                           d_chars_p,
                           static_cast<uint32_t>(state.dict_chunks_short.count),
                           stream);
  launch_dict_gather_long(d_dict_long_p,
                          d_offsets.data(),
                          d_chars_p,
                          static_cast<uint32_t>(state.dict_chunks_long.count),
                          stream);
  launch_fsst_gather(d_fsst_chunks_p,
                     d_offsets.data(),
                     d_chars_p,
                     d_comp_offsets_p,
                     d_fsst_decs_p,
                     static_cast<uint32_t>(state.fsst_gather_chunks.count),
                     stream);
  launch_dict_fsst_gather(d_dict_fsst_chunks_p,
                          d_offsets.data(),
                          d_chars_p,
                          d_byte_off_p,
                          d_decoded_off_p,
                          d_predecode_p,
                          d_dict_fsst_decs_p,
                          static_cast<uint32_t>(state.dict_fsst_chunks.count),
                          stream);

  // All-valid → overlay UNCOMPRESSED validity → fold in DICT_FSST inline NULLs.
  rmm::device_buffer null_mask{};
  cudf::size_type null_count = 0;
  bool need_mask             = state.has_nulls || state.any_inline_nulls;
  if (need_mask) {
    null_mask = cudf::create_null_mask(
      static_cast<cudf::size_type>(total_rows), cudf::mask_state::ALL_VALID, stream, mr);
    for (auto const& run : state.validity) {
      overlay_validity_run(run, static_cast<uint8_t*>(null_mask.data()), stream);
    }
    if (state.any_inline_nulls) {
      launch_dict_fsst_mark_nulls(d_dict_fsst_p,
                                  static_cast<uint8_t*>(null_mask.data()),
                                  static_cast<uint32_t>(state.dict_fsst_descs.count),
                                  stream);
    }
    null_count = cudf::null_count(static_cast<cudf::bitmask_type const*>(null_mask.data()),
                                  0,
                                  static_cast<cudf::size_type>(total_rows),
                                  stream);
  }

  auto offsets_col = std::make_unique<cudf::column>(cudf::data_type{cudf::type_id::INT32},
                                                    static_cast<cudf::size_type>(total_rows + 1u),
                                                    d_offsets.release(),
                                                    rmm::device_buffer{0, stream, mr},
                                                    0);

  RMM_CUDA_TRY(cudaPeekAtLastError());
  return cudf::make_strings_column(static_cast<cudf::size_type>(total_rows),
                                   std::move(offsets_col),
                                   std::move(d_chars),
                                   null_count,
                                   std::move(null_mask));
}

std::unique_ptr<cudf::column> gpu_decode_strings_column(gpu_string_column_decode_input const& col,
                                                        rmm::cuda_stream_view stream,
                                                        rmm::device_async_resource_ref mr)
{
  decode_descriptor_arena arena;
  auto prepared = prepare_strings_column_decode(col, arena, stream, mr);
  arena.flush(stream, mr);
  return prepared.launch(stream, mr);
}

}  // namespace sirius::cuda::scan
