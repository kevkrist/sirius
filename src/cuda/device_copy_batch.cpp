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

#include "cuda/device_copy_batch.hpp"

#include <cudf/detail/utilities/cuda_memcpy.hpp>

#include <rmm/cuda_device.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <thread>

namespace sirius::cuda {

cudaError_t drain_copy_stream_or_terminate(::cuda::stream_ref stream,
                                           int device,
                                           detail::copy_completion_api api) noexcept
{
  try {
    if (device < 0 && cudaGetDevice(&device) != cudaSuccess) { std::terminate(); }
    rmm::cuda_set_device_raii const guard{rmm::cuda_device_id{device}};
    auto const status = api.synchronize(stream.get());
    if (status == cudaSuccess) { return status; }
    std::atomic<bool> completed{false};
    auto const callback = [](cudaStream_t, cudaError_t, void* state) noexcept {
      static_cast<std::atomic<bool>*>(state)->store(true, std::memory_order_release);
    };
    if (api.add_callback(stream.get(), callback, &completed, 0) != cudaSuccess) {
      std::terminate();
    }
    auto delay = std::chrono::microseconds{4};
    while (!completed.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(delay);
      delay = std::min(std::chrono::microseconds{256}, delay * 2);
    }
    return status;
  } catch (...) {
    std::terminate();
  }
}

void device_copy_batch::reserve(std::size_t n)
{
  _dsts.reserve(n);
  _srcs.reserve(n);
  _sizes.reserve(n);
}

void device_copy_batch::add(void* dst, void const* src, std::size_t bytes)
{
  if (bytes == 0 || dst == nullptr || src == nullptr) { return; }
  _dsts.push_back(dst);
  _srcs.push_back(src);
  _sizes.push_back(bytes);
  _bytes += bytes;
}

cudaError_t device_copy_batch::enqueue(::cuda::stream_ref stream) const
{
  if (_dsts.empty()) { return cudaSuccess; }
  return cudf::detail::memcpy_batch_async(
    _dsts.data(), _srcs.data(), _sizes.data(), _dsts.size(), stream);
}

void device_copy_batch::clear() noexcept
{
  _dsts.clear();
  _srcs.clear();
  _sizes.clear();
  _bytes = 0;
}

}  // namespace sirius::cuda
