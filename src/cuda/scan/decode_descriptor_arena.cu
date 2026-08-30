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

#include "cuda/scan/decode_descriptor_arena.cuh"

#include <rmm/detail/error.hpp>

#include <cuda_runtime.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace sirius::cuda::scan {

namespace {

/// Every staged array starts on this boundary; a safe over-alignment for all descriptor types.
constexpr std::size_t ARENA_ALIGN = 16;

constexpr std::size_t MIN_SLAB_BYTES = std::size_t{1} << 20;  // 1 MiB

std::size_t align_up(std::size_t v, std::size_t a) { return (v + a - 1) & ~(a - 1); }

/// Thread-local pool of pinned slabs backing `decode_descriptor_arena`. A released slab carries a
/// CUDA event recorded after its blob upload; `acquire` hands it out again only once that event
/// has completed (checked with a non-blocking `cudaEventQuery`), so slab reuse never waits on the
/// GPU from the task thread. Slabs are freed at thread exit; errors there are ignored because the
/// CUDA context may already be gone (matching the `pinned_host_pool` teardown in dict_fsst.cu).
class pinned_slab_pool {
 public:
  struct slab {
    void* ptr            = nullptr;
    std::size_t capacity = 0;
    cudaEvent_t ready    = nullptr;  ///< created lazily on first release-with-upload
    bool pending         = false;    ///< a recorded `ready` event may not have completed yet
    bool in_use          = false;
  };

  /// Returns the index of an idle slab with at least `min_bytes` capacity, allocating a new one
  /// when every candidate is too small or still pending.
  std::size_t acquire(std::size_t min_bytes)
  {
    for (std::size_t i = 0; i < _slabs.size(); ++i) {
      auto& s = _slabs[i];
      if (s.in_use || s.capacity < min_bytes) { continue; }
      if (s.pending) {
        if (cudaEventQuery(s.ready) != cudaSuccess) { continue; }
        s.pending = false;
      }
      s.in_use = true;
      return i;
    }
    slab s;
    s.capacity = std::max(std::bit_ceil(min_bytes), MIN_SLAB_BYTES);
    RMM_CUDA_TRY(cudaMallocHost(&s.ptr, s.capacity));
    s.in_use = true;
    _slabs.push_back(s);
    return _slabs.size() - 1;
  }

  slab& at(std::size_t index) { return _slabs.at(index); }

  /// Returns a slab to the pool. When `upload_stream` is non-null the slab's bytes are the source
  /// of an in-flight upload, so an event is recorded behind it and gates the next `acquire`.
  void release(std::size_t index, cudaStream_t upload_stream)
  {
    auto& s = _slabs.at(index);
    if (upload_stream != nullptr) {
      if (s.ready == nullptr) {
        RMM_CUDA_TRY(cudaEventCreateWithFlags(&s.ready, cudaEventDisableTiming));
      }
      RMM_CUDA_TRY(cudaEventRecord(s.ready, upload_stream));
      s.pending = true;
    }
    s.in_use = false;
  }

  ~pinned_slab_pool() noexcept
  {
    // Thread-local teardown can run after the CUDA driver has begun (or finished) shutting
    // down, so no live context may be assumed and nothing may escape (a throwing TLS destructor
    // is std::terminate). The calls below are CUDA runtime C APIs that error-return
    // (cudaErrorCudartUnloading) rather than throw; failures are swallowed and the slab is
    // deliberately leaked -- the OS reclaims it at process exit.
    for (auto& s : _slabs) {
      if (s.ready != nullptr) { (void)cudaEventDestroy(s.ready); }
      if (s.ptr != nullptr) { (void)cudaFreeHost(s.ptr); }
    }
  }

 private:
  std::vector<slab> _slabs;
};

pinned_slab_pool& thread_pool()
{
  static thread_local pinned_slab_pool pool;
  return pool;
}

}  // namespace

decode_descriptor_arena::decode_descriptor_arena() = default;

decode_descriptor_arena::~decode_descriptor_arena()
{
  if (_slab.index == SIZE_MAX) { return; }
  // A flushed arena's slab is reuse-gated by the event recorded in flush(); an unflushed one
  // (exception unwind before flush) issued no upload and is immediately reusable.
  thread_pool().release(_slab.index, /*upload_stream=*/nullptr);
}

std::size_t decode_descriptor_arena::stage_bytes(void const* src, std::size_t bytes)
{
  if (_flushed) { throw std::logic_error("decode_descriptor_arena: stage() after flush()"); }
  auto const offset = align_up(_size, ARENA_ALIGN);
  auto const needed = offset + bytes;
  if (_slab.index == SIZE_MAX || needed > _slab.capacity) {
    auto& pool           = thread_pool();
    auto const new_index = pool.acquire(needed);
    auto& new_slab       = pool.at(new_index);
    if (_slab.index != SIZE_MAX) {
      // Pre-flush growth: move the packed bytes and hand the old slab straight back (nothing has
      // been uploaded from it, so no event gate is needed).
      std::memcpy(new_slab.ptr, _slab.ptr, _size);
      pool.release(_slab.index, /*upload_stream=*/nullptr);
    }
    _slab = {new_slab.ptr, new_slab.capacity, new_index};
  }
  std::memcpy(static_cast<std::byte*>(_slab.ptr) + offset, src, bytes);
  _size = needed;
  return offset;
}

void decode_descriptor_arena::flush(rmm::cuda_stream_view stream, rmm::device_async_resource_ref mr)
{
  if (_flushed) { throw std::logic_error("decode_descriptor_arena: flush() called twice"); }
  if (_size > 0) {
    _device_blob = rmm::device_buffer(_size, stream, mr);
    RMM_CUDA_TRY(cudaMemcpyAsync(
      _device_blob.data(), _slab.ptr, _size, cudaMemcpyHostToDevice, stream.value()));
    // Gate the slab's next reuse on this upload; the arena itself never waits.
    auto& s = thread_pool().at(_slab.index);
    if (s.ready == nullptr) {
      RMM_CUDA_TRY(cudaEventCreateWithFlags(&s.ready, cudaEventDisableTiming));
    }
    RMM_CUDA_TRY(cudaEventRecord(s.ready, stream.value()));
    s.pending = true;
  }
  _flushed = true;
}

std::byte const* decode_descriptor_arena::device_base() const
{
  if (!_flushed) { throw std::logic_error("decode_descriptor_arena: device_ptr() before flush()"); }
  return static_cast<std::byte const*>(_device_blob.data());
}

}  // namespace sirius::cuda::scan
