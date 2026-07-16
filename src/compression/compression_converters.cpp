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

#include "compression_converters.hpp"

#include "compressed_representation.hpp"

#include <cudf/column/column.hpp>
#include <cudf/table/table.hpp>

#include <rmm/device_buffer.hpp>

#include <cuda_runtime.h>

#include <api/compressed_table_io.hpp>
#include <api/simpatico_codegen.hpp>
#include <cucascade/cudf/gpu_data_representation.hpp>
#include <cucascade/data/representation_converter.hpp>
#include <cucascade/memory/memory_space.hpp>
#include <log/logging.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sirius {

namespace {
std::atomic<int> g_decompress_column_threads{1};
}  // namespace

void set_decompress_column_threads(int n) noexcept
{
  g_decompress_column_threads.store(n, std::memory_order_relaxed);
}
int decompress_column_threads() noexcept
{
  return g_decompress_column_threads.load(std::memory_order_relaxed);
}

namespace {

// Rebind a column's buffers (recursively) to `s` for their eventual async free.
// Parallel decode allocates on a converter-scoped stream pool. Its work is already
// synchronized, so re-pointing the free stream to a long-lived caller stream lets
// the pool be destroyed without leaving dangling stream handles in the result.
std::unique_ptr<cudf::column> rebind_column_stream(std::unique_ptr<cudf::column> col,
                                                   rmm::cuda_stream_view s)
{
  if (!col) { return col; }
  const auto type = col->type();
  const auto size = col->size();
  const auto nc   = col->null_count();
  auto contents   = col->release();

  if (!contents.data) {
    throw std::logic_error("[compression_converters] released column has no data buffer");
  }
  auto data = std::move(*contents.data);
  data.set_stream(s);

  rmm::device_buffer null_mask =
    contents.null_mask ? std::move(*contents.null_mask) : rmm::device_buffer{};
  null_mask.set_stream(s);

  std::vector<std::unique_ptr<cudf::column>> children;
  children.reserve(contents.children.size());
  for (auto& ch : contents.children) {
    children.push_back(rebind_column_stream(std::move(ch), s));
  }
  return std::make_unique<cudf::column>(
    type, size, std::move(data), std::move(null_mask), nc, std::move(children));
}

cucascade::memory::memory_space& require_gpu_target(
  const cucascade::memory::memory_space* target_memory_space)
{
  if (target_memory_space == nullptr) {
    throw std::invalid_argument(
      "[compression_converters] a non-null target GPU memory space is required");
  }
  if (target_memory_space->get_tier() != cucascade::memory::Tier::GPU) {
    throw std::invalid_argument("[compression_converters] target memory space must be GPU-tier");
  }
  return *const_cast<cucascade::memory::memory_space*>(target_memory_space);
}

void validate_payload_fetch_range(std::uint64_t offset,
                                  std::size_t size,
                                  std::uint64_t payload_bytes)
{
  if (offset > payload_bytes || std::cmp_greater(size, payload_bytes - offset)) {
    throw std::out_of_range(
      "[compression_converters] compressed payload fetch exceeds the declared payload size");
  }
}

// Reconstruct + project + decompress a compressed_table into a GPU table
// representation. Shared by the host and device compression converters — only
// the byte transport (how `fetch` pulls the payload) differs between them.
std::unique_ptr<cucascade::idata_representation> reconstruct_and_decompress_to_gpu(
  std::span<const std::uint8_t> header,
  simpatico::payload_fetch_fn const& fetch,
  const std::optional<std::vector<std::size_t>>& selected_indices,
  cucascade::memory::memory_space& target_memory_space,
  rmm::cuda_stream_view stream)
{
  auto const mr = target_memory_space.get_default_allocator();
  std::optional<std::span<const std::size_t>> selection;
  if (selected_indices.has_value()) { selection.emplace(*selected_indices); }

  std::string read_error;
  auto compressed =
    simpatico::read_compressed_table_from_memory(header, fetch, stream, mr, &read_error, selection);
  if (!read_error.empty()) {
    throw std::runtime_error("[compression_converters] read_compressed_table_from_memory failed: " +
                             read_error);
  }

  // Selection was applied while reading, so unrequested payloads were never fetched.
  auto const configured_threads = decompress_column_threads();
  auto const max_threads =
    configured_threads > 1 ? static_cast<std::size_t>(configured_threads) : std::size_t{1};
  auto const n_threads = static_cast<int>(std::min(max_threads, compressed.num_columns()));

  // Keep the pool alive until every result buffer has been rebound. This also makes exception
  // unwinding destroy pool-backed columns before destroying their CUDA streams.
  simpatico::stream_pool decode_streams;
  std::unique_ptr<cudf::table> decompressed;
  if (n_threads > 1) {
    // Input payload copies were enqueued on `stream`, while pool streams are non-blocking.
    // Establish their producer/consumer ordering before parallel decode starts.
    stream.synchronize();
    if (!decode_streams.init(static_cast<std::size_t>(n_threads))) {
      throw std::runtime_error(
        "[compression_converters] failed to initialize parallel decompression streams");
    }

    decompressed = simpatico::decompress(compressed, decode_streams, mr);

    auto columns = decompressed->release();
    for (auto& column : columns) {
      column = rebind_column_stream(std::move(column), stream);
    }
    decompressed = std::make_unique<cudf::table>(std::move(columns));
  } else {
    decompressed = simpatico::decompress(std::move(compressed), stream, mr);
  }

  SIRIUS_LOG_DEBUG("[compression_converters] decompressed cols={} rows={} → GPU device={}",
                   decompressed->num_columns(),
                   decompressed->num_rows(),
                   target_memory_space.get_device_id());

  return std::make_unique<cucascade::gpu_table_representation>(
    std::move(decompressed), target_memory_space, stream);
}

// compressed_host_representation (pinned host) → GPU.
std::unique_ptr<cucascade::idata_representation> decompress_host_to_gpu(
  cucascade::idata_representation& source,
  const cucascade::memory::memory_space* target_memory_space,
  rmm::cuda_stream_view stream)
{
  auto& rep = source.cast<compressed_host_representation>();

  auto& target_space = require_gpu_target(target_memory_space);

  auto const payload_bytes = rep.payload_bytes();
  if (std::cmp_greater(payload_bytes, rep.payload_capacity_bytes())) {
    throw std::runtime_error(
      "[compression_converters] declared host payload exceeds its backing allocation");
  }

  // Pull each compressed leaf buffer straight from the pinned host payload into
  // device memory (block-aware, since the payload is a multi-block allocation).
  simpatico::payload_fetch_fn fetch =
    [&rep, payload_bytes](std::uint64_t off, std::size_t sz, void* dst, rmm::cuda_stream_view s) {
      validate_payload_fetch_range(off, sz, payload_bytes);
      if (sz == 0) { return; }
      copy_pinned_blocks_to_device(rep.payload(), off, dst, sz, s);
    };

  return reconstruct_and_decompress_to_gpu(
    rep.header(), fetch, rep.selected_indices(), target_space, stream);
}

// compressed_device_representation (device memory) → GPU.
std::unique_ptr<cucascade::idata_representation> decompress_device_to_gpu(
  cucascade::idata_representation& source,
  const cucascade::memory::memory_space* target_memory_space,
  rmm::cuda_stream_view stream)
{
  auto& rep          = source.cast<compressed_device_representation>();
  auto& target_space = require_gpu_target(target_memory_space);

  if (target_space.get_device_id() != rep.get_memory_space().get_device_id()) {
    throw std::invalid_argument(
      "[compression_converters] cross-device compressed payload conversion is unsupported; "
      "route decompression to the payload's device");
  }

  auto const payload_bytes = rep.payload_bytes();
  if (std::cmp_greater(payload_bytes, rep.payload_capacity_bytes())) {
    throw std::runtime_error(
      "[compression_converters] declared device payload exceeds its backing allocation");
  }

  // The payload is already a single contiguous device buffer — each leaf buffer
  // is one device→device copy at its offset.
  const auto* payload_base          = static_cast<const std::byte*>(rep.payload_device_ptr());
  simpatico::payload_fetch_fn fetch = [payload_base, payload_bytes](std::uint64_t off,
                                                                    std::size_t sz,
                                                                    void* dst,
                                                                    rmm::cuda_stream_view s) {
    validate_payload_fetch_range(off, sz, payload_bytes);
    if (sz == 0) { return; }
    if (dst == nullptr) {
      throw std::invalid_argument("[compression_converters] payload destination is null");
    }
    if (payload_base == nullptr) {
      throw std::runtime_error("[compression_converters] device payload storage is null");
    }
    CUCASCADE_CUDA_TRY(cudaMemcpyAsync(
      dst, payload_base + static_cast<std::size_t>(off), sz, cudaMemcpyDeviceToDevice, s.value()));
  };

  return reconstruct_and_decompress_to_gpu(
    rep.header(), fetch, rep.selected_indices(), target_space, stream);
}

}  // namespace

void register_compression_converters(cucascade::representation_converter_registry& registry)
{
  // Decompression paths used by prepare_for_processing / convert_to.
  if (!registry
         .has_converter<compressed_host_representation, cucascade::gpu_table_representation>()) {
    registry
      .register_converter<compressed_host_representation, cucascade::gpu_table_representation>(
        decompress_host_to_gpu);
  }
  if (!registry
         .has_converter<compressed_device_representation, cucascade::gpu_table_representation>()) {
    registry
      .register_converter<compressed_device_representation, cucascade::gpu_table_representation>(
        decompress_device_to_gpu);
  }
}

}  // namespace sirius
