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

#include "compressed_representation.hpp"

#include <cuda_runtime.h>

#include <cucascade/error.hpp>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace sirius {

namespace {

template <typename Blob>
void require_backing_blob(const std::shared_ptr<Blob>& blob)
{
  if (!blob) {
    throw std::invalid_argument("[compressed_representation] backing blob must not be null");
  }
}

void validate_block_copy_range(std::uint64_t offset, std::size_t size, std::size_t capacity)
{
  if (std::cmp_greater(offset, capacity) || size > capacity - static_cast<std::size_t>(offset)) {
    throw std::out_of_range(
      "[compressed_representation] pinned block copy exceeds the backing allocation");
  }
}

std::vector<std::size_t> resolve_absolute_indices(
  std::span<const std::size_t> indices,
  const std::optional<std::vector<std::size_t>>& existing_selection,
  std::size_t num_all_columns);

}  // namespace

// ── Block-aware pinned copies ────────────────────────────────────────────────

void copy_device_to_pinned_blocks(
  const void* src_device,
  cucascade::memory::fixed_size_host_memory_resource::multiple_blocks_allocation& dst,
  std::uint64_t dst_offset,
  std::size_t size,
  rmm::cuda_stream_view stream)
{
  validate_block_copy_range(dst_offset, size, dst.size_bytes());
  if (size == 0) return;
  if (src_device == nullptr) {
    throw std::invalid_argument(
      "[compressed_representation] device-to-host copy source must not be null");
  }
  const std::size_t bs = dst.block_size();
  auto const offset    = static_cast<std::size_t>(dst_offset);
  std::size_t d_idx    = offset / bs;
  std::size_t d_off    = offset % bs;
  const auto* src      = static_cast<const std::byte*>(src_device);
  std::size_t copied   = 0;
  while (copied < size) {
    const std::size_t chunk = std::min(size - copied, bs - d_off);
    CUCASCADE_CUDA_TRY(cudaMemcpyAsync(
      dst.at(d_idx).data() + d_off, src + copied, chunk, cudaMemcpyDeviceToHost, stream.value()));
    copied += chunk;
    d_off += chunk;
    if (d_off == bs) {
      ++d_idx;
      d_off = 0;
    }
  }
}

void copy_pinned_blocks_to_device(
  const cucascade::memory::fixed_size_host_memory_resource::multiple_blocks_allocation& src,
  std::uint64_t src_offset,
  void* dst_device,
  std::size_t size,
  rmm::cuda_stream_view stream)
{
  validate_block_copy_range(src_offset, size, src.size_bytes());
  if (size == 0) return;
  if (dst_device == nullptr) {
    throw std::invalid_argument(
      "[compressed_representation] host-to-device copy destination must not be null");
  }
  const std::size_t bs = src.block_size();
  auto const offset    = static_cast<std::size_t>(src_offset);
  std::size_t s_idx    = offset / bs;
  std::size_t s_off    = offset % bs;
  auto* dst            = static_cast<std::byte*>(dst_device);
  std::size_t copied   = 0;
  while (copied < size) {
    const std::size_t chunk = std::min(size - copied, bs - s_off);
    CUCASCADE_CUDA_TRY(cudaMemcpyAsync(
      dst + copied, src.at(s_idx).data() + s_off, chunk, cudaMemcpyHostToDevice, stream.value()));
    copied += chunk;
    s_off += chunk;
    if (s_off == bs) {
      ++s_idx;
      s_off = 0;
    }
  }
}

// ── Owning constructor ───────────────────────────────────────────────────────

compressed_host_representation::compressed_host_representation(
  cucascade::memory::memory_space& memory_space,
  std::shared_ptr<pinned_compressed_blob> blob,
  std::vector<std::string> column_names,
  std::size_t compressed_bytes,
  std::size_t uncompressed_bytes,
  std::int64_t num_rows)
  : cucascade::idata_representation(memory_space),
    _blob(std::move(blob)),
    _column_names(std::move(column_names)),
    _compressed_bytes(compressed_bytes),
    _uncompressed_bytes(uncompressed_bytes),
    _num_rows(num_rows)
{
  require_backing_blob(_blob);
}

// ── Sharing constructor (private) ────────────────────────────────────────────

compressed_host_representation::compressed_host_representation(
  cucascade::memory::memory_space& memory_space,
  std::shared_ptr<pinned_compressed_blob> blob,
  std::vector<std::string> column_names,
  std::size_t compressed_bytes,
  std::size_t uncompressed_bytes,
  std::int64_t num_rows,
  std::optional<std::vector<std::size_t>> selected_indices)
  : cucascade::idata_representation(memory_space),
    _blob(std::move(blob)),
    _column_names(std::move(column_names)),
    _compressed_bytes(compressed_bytes),
    _uncompressed_bytes(uncompressed_bytes),
    _num_rows(num_rows),
    _selected_indices(std::move(selected_indices))
{
  require_backing_blob(_blob);
}

const cucascade::memory::fixed_size_host_memory_resource::multiple_blocks_allocation&
compressed_host_representation::payload() const
{
  if (!_blob->payload) {
    throw std::logic_error(
      "[compressed_host_representation::payload] backing allocation is not present");
  }
  return *_blob->payload;
}

// ── idata_representation interface ───────────────────────────────────────────

std::unique_ptr<cucascade::idata_representation> compressed_host_representation::clone(
  rmm::cuda_stream_view /*stream*/)
{
  // Share the same backing blob — no byte copy needed.
  return std::unique_ptr<compressed_host_representation>(
    new compressed_host_representation(get_memory_space(),
                                       _blob,
                                       _column_names,
                                       _compressed_bytes,
                                       _uncompressed_bytes,
                                       _num_rows,
                                       _selected_indices));
}

// ── Projection ───────────────────────────────────────────────────────────────

std::unique_ptr<compressed_host_representation> compressed_host_representation::select_columns(
  std::span<const std::size_t> indices) const
{
  auto absolute = resolve_absolute_indices(indices, _selected_indices, _column_names.size());

  // const_cast is safe: select_columns is logically const (it creates a
  // projection sharing the same blob) but the base-class constructor requires
  // a non-const memory_space& — the underlying object is non-const.
  return std::unique_ptr<compressed_host_representation>(new compressed_host_representation(
    const_cast<cucascade::memory::memory_space&>(get_memory_space()),
    _blob,
    _column_names,
    _compressed_bytes,
    _uncompressed_bytes,
    _num_rows,
    std::move(absolute)));
}

// ── compressed_device_representation ─────────────────────────────────────────

namespace {

// Resolve caller-relative column indices to absolute indices into the chunk's
// full column list, honoring any projection already applied. Shared by the host
// and device select_columns() implementations.
std::vector<std::size_t> resolve_absolute_indices(
  std::span<const std::size_t> indices,
  const std::optional<std::vector<std::size_t>>& existing_selection,
  std::size_t num_all_columns)
{
  std::vector<std::size_t> absolute;
  absolute.reserve(indices.size());
  std::vector<std::uint8_t> seen(num_all_columns, 0);
  for (auto idx : indices) {
    std::size_t absolute_index;
    if (existing_selection.has_value()) {
      if (idx >= existing_selection->size()) {
        throw std::out_of_range("[compressed_representation::select_columns] index out of range");
      }
      absolute_index = (*existing_selection)[idx];
    } else {
      if (idx >= num_all_columns) {
        throw std::out_of_range("[compressed_representation::select_columns] index out of range");
      }
      absolute_index = idx;
    }
    if (absolute_index >= num_all_columns) {
      throw std::out_of_range(
        "[compressed_representation::select_columns] resolved index out of range");
    }
    if (seen[absolute_index]) {
      throw std::invalid_argument(
        "[compressed_representation::select_columns] duplicate column index");
    }
    seen[absolute_index] = 1;
    absolute.push_back(absolute_index);
  }
  return absolute;
}

}  // namespace

compressed_device_representation::compressed_device_representation(
  cucascade::memory::memory_space& memory_space,
  std::shared_ptr<device_compressed_blob> blob,
  std::vector<std::string> column_names,
  std::size_t compressed_bytes,
  std::size_t uncompressed_bytes,
  std::int64_t num_rows)
  : cucascade::idata_representation(memory_space),
    _blob(std::move(blob)),
    _column_names(std::move(column_names)),
    _compressed_bytes(compressed_bytes),
    _uncompressed_bytes(uncompressed_bytes),
    _num_rows(num_rows)
{
  require_backing_blob(_blob);
}

compressed_device_representation::compressed_device_representation(
  cucascade::memory::memory_space& memory_space,
  std::shared_ptr<device_compressed_blob> blob,
  std::vector<std::string> column_names,
  std::size_t compressed_bytes,
  std::size_t uncompressed_bytes,
  std::int64_t num_rows,
  std::optional<std::vector<std::size_t>> selected_indices)
  : cucascade::idata_representation(memory_space),
    _blob(std::move(blob)),
    _column_names(std::move(column_names)),
    _compressed_bytes(compressed_bytes),
    _uncompressed_bytes(uncompressed_bytes),
    _num_rows(num_rows),
    _selected_indices(std::move(selected_indices))
{
  require_backing_blob(_blob);
}

std::unique_ptr<cucascade::idata_representation> compressed_device_representation::clone(
  rmm::cuda_stream_view /*stream*/)
{
  // Share the same backing blob — no byte copy needed.
  return std::unique_ptr<compressed_device_representation>(
    new compressed_device_representation(get_memory_space(),
                                         _blob,
                                         _column_names,
                                         _compressed_bytes,
                                         _uncompressed_bytes,
                                         _num_rows,
                                         _selected_indices));
}

std::unique_ptr<compressed_device_representation> compressed_device_representation::select_columns(
  std::span<const std::size_t> indices) const
{
  auto absolute = resolve_absolute_indices(indices, _selected_indices, _column_names.size());

  // const_cast is safe: select_columns is logically const (it creates a
  // projection sharing the same blob) but the base-class constructor requires
  // a non-const memory_space& — the underlying object is non-const.
  return std::unique_ptr<compressed_device_representation>(new compressed_device_representation(
    const_cast<cucascade::memory::memory_space&>(get_memory_space()),
    _blob,
    _column_names,
    _compressed_bytes,
    _uncompressed_bytes,
    _num_rows,
    std::move(absolute)));
}

}  // namespace sirius
