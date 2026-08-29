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

//! @file
//! Pinned-host descriptor staging for the GPU-native scan decode path. The codec prepare functions
//! (see `gpu_native_decode.cuh` and `gpu_decode_strings.cuh`) pack their kernel descriptor arrays
//! into one `decode_descriptor_arena` per split; `decode_duckdb_native_split` then uploads the
//! whole blob with a single host-to-device copy before launching any decode kernel. This replaces
//! the previous per-run pageable `cudaMemcpyAsync` uploads and the per-column synchronizations
//! that guarded their source lifetimes. The pinned backing memory comes from a thread-local slab
//! pool; a slab is reused only after a CUDA event recorded behind its upload has completed, so no
//! host wait is ever added to the decode path.

#include <rmm/cuda_stream_view.hpp>
#include <rmm/device_buffer.hpp>
#include <rmm/resource_ref.hpp>

#include <cstddef>
#include <cstdint>

namespace sirius::cuda::scan {

/// Handle to one descriptor array staged in a `decode_descriptor_arena`. Resolves to a device
/// pointer via `decode_descriptor_arena::device_ptr` once the arena has been flushed.
struct arena_slot {
  std::size_t offset = 0;  ///< byte offset of the array inside the arena blob
  std::size_t count  = 0;  ///< number of staged elements (0 = nothing staged)
};

/// One split's packed descriptor blob: pinned host staging plus its device mirror.
///
/// Usage protocol: `stage()` any number of descriptor arrays, `flush()` exactly once, then resolve
/// slots with `device_ptr()` and launch the consuming kernels. The arena must outlive every kernel
/// launch that consumes a resolved pointer: the device mirror is freed stream-ordered on the flush
/// stream when the arena is destroyed, so destruction must happen after those launches are
/// enqueued (function-local arenas that span the launches satisfy this naturally).
///
/// Not thread-safe; one arena belongs to one task thread.
class decode_descriptor_arena {
 public:
  decode_descriptor_arena();
  ~decode_descriptor_arena();

  decode_descriptor_arena(decode_descriptor_arena const&)            = delete;
  decode_descriptor_arena& operator=(decode_descriptor_arena const&) = delete;
  decode_descriptor_arena(decode_descriptor_arena&&)                 = delete;
  decode_descriptor_arena& operator=(decode_descriptor_arena&&)      = delete;

  /// Copies `count` elements from `src` into the pinned blob (16-byte aligned) and returns the
  /// slot. Must be called before `flush()`; throws `std::logic_error` afterwards.
  template <typename T>
  arena_slot stage(T const* src, std::size_t count)
  {
    if (count == 0) { return {}; }
    auto const offset = stage_bytes(src, count * sizeof(T));
    return {offset, count};
  }

  /// Uploads everything staged so far with one host-to-device copy on `stream` and records the
  /// slab-reuse event behind it. Idempotent by contract violation: a second call throws
  /// `std::logic_error`. A no-descriptor arena flushes to an empty device mirror without any copy.
  void flush(rmm::cuda_stream_view stream, rmm::device_async_resource_ref mr);

  [[nodiscard]] bool flushed() const noexcept { return _flushed; }

  /// Device pointer for a staged slot; `nullptr` for an empty slot. Only valid after `flush()`
  /// (throws `std::logic_error` before). `T` must match the type the slot was staged with.
  template <typename T>
  [[nodiscard]] T const* device_ptr(arena_slot slot) const
  {
    if (slot.count == 0) { return nullptr; }
    return reinterpret_cast<T const*>(device_base() + slot.offset);
  }

 private:
  /// Appends `bytes` from `src` at the next 16-byte-aligned blob offset, growing the pinned slab
  /// as needed, and returns that offset.
  std::size_t stage_bytes(void const* src, std::size_t bytes);

  /// Base of the device mirror; throws `std::logic_error` when not yet flushed.
  [[nodiscard]] std::byte const* device_base() const;

  struct slab_ref {
    void* ptr            = nullptr;
    std::size_t capacity = 0;
    std::size_t index    = SIZE_MAX;  ///< owning slot in the thread-local pool
  };

  slab_ref _slab;
  std::size_t _size = 0;
  rmm::device_buffer _device_blob;
  bool _flushed = false;
};

}  // namespace sirius::cuda::scan
