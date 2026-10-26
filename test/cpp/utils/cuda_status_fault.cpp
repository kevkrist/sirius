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

// Linked only into sirius_cuda_fault_test, together with --wrap for these calls.
#include "utils/cuda_status_fault.hpp"

#include "cuda/device_copy_batch.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace {
thread_local int armed_operation      = 0;
thread_local cudaError_t armed_status = cudaSuccess;

cudaError_t take_status(int operation)
{
  if (armed_operation != operation) return cudaSuccess;
  armed_operation = 0;
  return armed_status;
}

struct pool_access_fault {
  cudaMemPool_t pool;
  int accessor;
  std::size_t skip;
  cudaError_t status;
};
std::mutex pool_access_mutex;
std::optional<pool_access_fault> armed_pool_access;
bool pool_access_consumed = false;
}  // namespace

namespace sirius::test {
scoped_cuda_status_fault::scoped_cuda_status_fault(operation op, cudaError_t status)
{
  REQUIRE(armed_operation == 0);
  armed_status    = status;
  armed_operation = op;
}
scoped_cuda_status_fault::~scoped_cuda_status_fault() { armed_operation = 0; }
bool scoped_cuda_status_fault::consumed() { return armed_operation == 0; }

scoped_pool_access_fault::scoped_pool_access_fault(cudaMemPool_t pool,
                                                   int accessor,
                                                   std::size_t skip,
                                                   cudaError_t status)
{
  std::scoped_lock lock(pool_access_mutex);
  REQUIRE_FALSE(armed_pool_access.has_value());
  armed_pool_access    = pool_access_fault{pool, accessor, skip, status};
  pool_access_consumed = false;
}
scoped_pool_access_fault::~scoped_pool_access_fault()
{
  std::scoped_lock lock(pool_access_mutex);
  armed_pool_access.reset();
}
bool scoped_pool_access_fault::consumed() const
{
  std::scoped_lock lock(pool_access_mutex);
  return pool_access_consumed;
}
}  // namespace sirius::test

extern "C" cudaError_t __real_cudaStreamWaitEvent(cudaStream_t, cudaEvent_t, unsigned int);
extern "C" cudaError_t __real_cudaDeviceSynchronize();
extern "C" cudaError_t __real_cudaMemPoolGetAccess(cudaMemAccessFlags*,
                                                   cudaMemPool_t,
                                                   cudaMemLocation*);

extern "C" cudaError_t __wrap_cudaStreamWaitEvent(cudaStream_t stream,
                                                  cudaEvent_t event,
                                                  unsigned int flags)
{
  if (auto status = take_status(sirius::test::scoped_cuda_status_fault::wait_event);
      status != cudaSuccess)
    return status;
  return __real_cudaStreamWaitEvent(stream, event, flags);
}

extern "C" cudaError_t __wrap_cudaDeviceSynchronize()
{
  if (auto status = take_status(sirius::test::scoped_cuda_status_fault::synchronize_device);
      status != cudaSuccess)
    return status;
  return __real_cudaDeviceSynchronize();
}

extern "C" cudaError_t __wrap_cudaMemPoolGetAccess(cudaMemAccessFlags* flags,
                                                   cudaMemPool_t pool,
                                                   cudaMemLocation* location)
{
  {
    std::scoped_lock lock(pool_access_mutex);
    auto& fault = armed_pool_access;
    if (fault && pool == fault->pool && location && location->id == fault->accessor) {
      if (fault->skip == 0) {
        auto const status = fault->status;
        fault.reset();
        pool_access_consumed = true;
        return status;
      }
      --fault->skip;
    }
  }
  return __real_cudaMemPoolGetAccess(flags, pool, location);
}

#include <array>
#include <chrono>
#include <thread>

namespace {
struct publication_fault_state {
  sirius::test::scoped_publication_fault::operation operation;
  std::size_t skip;
  cudaError_t status;
  std::chrono::milliseconds delay;
  bool consumed     = false;
  bool invalid_join = false;
  struct stream_state {
    cudaStream_t stream = nullptr;
    bool joined         = false;
  };
  std::array<stream_state, 32> streams{};
  std::size_t stream_count = 0;
};
thread_local std::optional<publication_fault_state> publication_fault;

cudaError_t publication_status(sirius::test::scoped_publication_fault::operation op,
                               cudaStream_t stream = nullptr)
{
  auto& state = publication_fault;
  if (!state || state->consumed || state->operation != op) { return cudaSuccess; }
  if (state->skip != 0) {
    --state->skip;
    return cudaSuccess;
  }
  state->consumed = true;
  if (state->delay.count() != 0 && stream != nullptr) {
    auto duration     = std::make_unique<std::chrono::milliseconds>(state->delay);
    auto const status = cudaLaunchHostFunc(
      stream,
      [](void* value) {
        std::unique_ptr<std::chrono::milliseconds> retained{
          static_cast<std::chrono::milliseconds*>(value)};
        std::this_thread::sleep_for(*retained);
      },
      duration.get());
    if (status != cudaSuccess) { return status; }
    (void)duration.release();
  }
  return state->status;
}

void submitted(cudaStream_t stream)
{
  auto& state = publication_fault;
  if (!state) { return; }
  for (std::size_t i = 0; i < state->stream_count; ++i) {
    if (state->streams[i].stream == stream) {
      state->streams[i].joined = false;
      return;
    }
  }
  if (state->stream_count == state->streams.size()) { std::terminate(); }
  state->streams[state->stream_count++] = {stream, false};
}
}  // namespace

namespace sirius::test {
scoped_publication_fault::scoped_publication_fault(operation op,
                                                   std::size_t skip,
                                                   cudaError_t status,
                                                   std::chrono::milliseconds delay)
{
  REQUIRE_FALSE(publication_fault);
  publication_fault.emplace(op, skip, status, delay);
}
scoped_publication_fault::~scoped_publication_fault() { publication_fault.reset(); }
bool scoped_publication_fault::consumed() const noexcept { return publication_fault->consumed; }
std::size_t scoped_publication_fault::observed_stream_count() const noexcept
{
  return publication_fault->stream_count;
}
bool scoped_publication_fault::invalid_join() const noexcept
{
  return publication_fault->invalid_join;
}
bool scoped_publication_fault::all_submitted_streams_joined() const noexcept
{
  for (std::size_t i = 0; i < publication_fault->stream_count; ++i) {
    if (!publication_fault->streams[i].joined) { return false; }
  }
  return true;
}
}  // namespace sirius::test

extern "C" cudaError_t __real_cudaMemcpyPeerAsync(
  void*, int, void const*, int, std::size_t, cudaStream_t);
// Wrap the Sirius call boundary: libcudf's internal runtime calls cannot be link-wrapped here.
extern "C" cudaError_t __real__ZNK6sirius4cuda17device_copy_batch7enqueueEN4cuda3__410stream_refE(
  sirius::cuda::device_copy_batch const*, ::cuda::stream_ref);
extern "C" cudaError_t __real_cudaEventRecord(cudaEvent_t, cudaStream_t);
extern "C" cudaError_t __real_cudaStreamSynchronize(cudaStream_t);
extern "C" cudaError_t __real_cudaEventCreateWithFlags(cudaEvent_t*, unsigned int);

extern "C" cudaError_t __wrap_cudaMemcpyPeerAsync(void* dst,
                                                  int dst_device,
                                                  void const* src,
                                                  int src_device,
                                                  std::size_t bytes,
                                                  cudaStream_t stream)
{
  auto const injected =
    publication_status(sirius::test::scoped_publication_fault::peer_copy, stream);
  submitted(stream);
  if (injected != cudaSuccess) { return injected; }
  return __real_cudaMemcpyPeerAsync(dst, dst_device, src, src_device, bytes, stream);
}
extern "C" cudaError_t __wrap__ZNK6sirius4cuda17device_copy_batch7enqueueEN4cuda3__410stream_refE(
  sirius::cuda::device_copy_batch const* batch, ::cuda::stream_ref stream)
{
  auto const injected =
    publication_status(sirius::test::scoped_publication_fault::batch_copy, stream.get());
  submitted(stream.get());
  if (injected != cudaSuccess) { return injected; }
  return __real__ZNK6sirius4cuda17device_copy_batch7enqueueEN4cuda3__410stream_refE(batch, stream);
}
extern "C" cudaError_t __wrap_cudaEventRecord(cudaEvent_t event, cudaStream_t stream)
{
  auto const injected =
    publication_status(sirius::test::scoped_publication_fault::record_event, stream);
  submitted(stream);
  if (injected != cudaSuccess) { return injected; }
  return __real_cudaEventRecord(event, stream);
}
extern "C" cudaError_t __wrap_cudaStreamSynchronize(cudaStream_t stream)
{
  auto const injected =
    publication_status(sirius::test::scoped_publication_fault::synchronize_stream, stream);
  if (injected != cudaSuccess) {
    submitted(stream);
    return injected;
  }
  auto const status = __real_cudaStreamSynchronize(stream);
  if (publication_fault) {
    publication_fault->invalid_join |= status == cudaErrorInvalidResourceHandle;
    if (status == cudaSuccess) {
      for (std::size_t i = 0; i < publication_fault->stream_count; ++i) {
        if (publication_fault->streams[i].stream == stream) {
          publication_fault->streams[i].joined = true;
        }
      }
    }
  }
  return status;
}
extern "C" cudaError_t __wrap_cudaEventCreateWithFlags(cudaEvent_t* event, unsigned int flags)
{
  auto const injected = publication_status(sirius::test::scoped_publication_fault::create_event);
  if (injected != cudaSuccess) { return injected; }
  return __real_cudaEventCreateWithFlags(event, flags);
}
