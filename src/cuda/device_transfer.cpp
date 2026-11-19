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

#include "memory/device_transfer.hpp"

#include "cuda/device_copy_batch.hpp"
#include "log/logging.hpp"
#include "memory/pool_peer_access.hpp"
#include "memory/proto_xfer.hpp"
#include "memory/runtime_stream_pool.hpp"

#include <rmm/cuda_stream.hpp>

#include <cuda_runtime.h>

#include <cucascade/error.hpp>
#include <cucascade/memory/fixed_size_host_memory_resource.hpp>
#include <cucascade/memory/memory_space.hpp>

#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace sirius::memory {
namespace {

void wait_for_source(transfer_readiness const& ready, ::cuda::stream_ref stream)
{
  if (auto const* event = std::get_if<cucascade::cuda::cuda_event_view>(&ready)) {
    if (event->value() == nullptr) {
      throw std::invalid_argument("[enqueue_device_copy] source-ready event is null");
    }
    event->wait(stream);
  }
}

cucascade::memory::fixed_size_host_memory_resource& staging_resource(
  cucascade::memory::memory_space const* space)
{
  if (space == nullptr) {
    throw std::runtime_error("[enqueue_device_copy] HOST staging is required but unavailable");
  }
  if (space->get_tier() != cucascade::memory::Tier::HOST) {
    throw std::invalid_argument("[enqueue_device_copy] staging requires a HOST memory space");
  }
  auto* resource =
    space->get_memory_resource_as<cucascade::memory::fixed_size_host_memory_resource>();
  if (resource == nullptr) {
    throw std::runtime_error("[enqueue_device_copy] HOST space has no fixed-size staging resource");
  }
  return *resource;
}

cudaError_t drain(::cuda::stream_ref stream, rmm::cuda_device_id device) noexcept
{
  rmm::cuda_set_device_raii guard{device};
  auto const status = cudaStreamSynchronize(stream.get());
  if (status != cudaSuccess) {
    (void)cudaGetLastError();
    log::log_noexcept([&] {
      SIRIUS_LOG_ERROR("[enqueue_device_copy] cleanup synchronization failed on GPU {}: {}",
                       device.value(),
                       cudaGetErrorString(status));
    });
  }
  return status;
}

}  // namespace

//===----------------------------------------------------------------------===//
// PROTOTYPE [xfer-PR]: host_bounce_scope
//===----------------------------------------------------------------------===//
struct host_bounce_scope::impl {
  struct staged {
    std::byte const* source{};
    std::size_t bytes{};
    int device{};
    bool valid{false};
    cucascade::memory::fixed_multiple_blocks_allocation blocks;
    cucascade::cuda::cuda_event done;  // Created on the source GPU
  };
  struct source_stream {
    int device{};
    std::optional<runtime_stream_pool::lease> lease;
    std::optional<rmm::cuda_stream> owned;

    [[nodiscard]] ::cuda::stream_ref get() const
    {
      return lease ? lease->get() : ::cuda::stream_ref{owned->value()};
    }
  };

  [[nodiscard]] staged* find(std::byte const* source, std::size_t bytes, int device) noexcept
  {
    for (auto const& entry : stagings) {
      if (entry->valid && entry->source == source && entry->bytes == bytes &&
          entry->device == device) {
        return entry.get();
      }
    }
    return nullptr;
  }

  /// The scope's stream on @p device, which must be the current device.
  [[nodiscard]] ::cuda::stream_ref stream_on(rmm::cuda_device_id device,
                                             cucascade::memory::memory_space const* space)
  {
    for (auto const& entry : sources) {
      if (entry->device == device.value()) { return entry->get(); }
    }
    auto made    = std::make_unique<source_stream>();
    made->device = device.value();
    if (space) {
      made->lease.emplace(runtime_stream_pool::acquire(*space));
    } else {
      made->owned.emplace(rmm::cuda_stream::flags::non_blocking);
    }
    sources.push_back(std::move(made));
    return sources.back()->get();
  }

  void note_destination(int device, cudaStream_t stream)
  {
    for (auto const& [known_device, known] : destinations) {
      if (known == stream && known_device == device) { return; }
    }
    destinations.emplace_back(device, stream);
  }

  /// Waits for every stream; returns the first failure.
  [[nodiscard]] cudaError_t join() noexcept
  {
    auto failed = cudaSuccess;
    for (auto const& entry : sources) {
      auto const status = drain(entry->get(), rmm::cuda_device_id{entry->device});
      if (failed == cudaSuccess) { failed = status; }
    }
    for (auto const& [device, stream] : destinations) {
      auto const status = drain(::cuda::stream_ref{stream}, rmm::cuda_device_id{device});
      if (failed == cudaSuccess) { failed = status; }
    }
    return failed;
  }

  ~impl()
  {
    if (join() != cudaSuccess) {
      // Unfinished DMA may still touch the pinned blocks: never return them.
      for (auto& entry : stagings) {
        (void)entry->blocks.release();
      }
    }
    for (auto& entry : sources) {
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{entry->device}};
      entry->lease.reset();
      entry->owned.reset();
    }
  }

  std::vector<std::unique_ptr<staged>> stagings;
  std::vector<std::unique_ptr<source_stream>> sources;
  std::vector<std::pair<int, cudaStream_t>> destinations;
};

host_bounce_scope::host_bounce_scope() : _impl{std::make_unique<impl>()} {}
host_bounce_scope::~host_bounce_scope() = default;

void host_bounce_scope::drain()
{
  auto const status = _impl->join();
  if (status != cudaSuccess) {
    throw cucascade::cuda_error{"[host_bounce_scope] synchronization failed: " +
                                std::string{cudaGetErrorName(status)}};
  }
}

namespace {

/// Enqueues one leg between @p device_bytes and the HOST blocks of @p blocks on @p stream.
void enqueue_block_leg(cucascade::memory::fixed_multiple_blocks_allocation const& blocks,
                       std::byte* device_bytes,
                       std::size_t bytes,
                       bool to_host,
                       ::cuda::stream_ref stream)
{
  sirius::cuda::device_copy_batch batch;
  batch.reserve(blocks->size());
  for (std::size_t offset = 0, index = 0; offset < bytes; ++index) {
    auto const block = blocks->at(index);
    auto const count = std::min(block.size(), bytes - offset);
    if (to_host) {
      batch.add(block.data(), device_bytes + offset, count);
    } else {
      batch.add(device_bytes + offset, block.data(), count);
    }
    offset += count;
  }
  CUCASCADE_CUDA_TRY(batch.enqueue(stream));
}

}  // namespace

device_transfer_route enqueue_device_copy(std::span<std::byte> destination,
                                          rmm::cuda_device_id destination_device,
                                          ::cuda::stream_ref destination_stream,
                                          std::span<std::byte const> source,
                                          rmm::cuda_device_id source_device,
                                          cucascade::memory::memory_space const* source_space,
                                          transfer_readiness ready,
                                          cucascade::memory::memory_space const* host_staging_space,
                                          device_transfer_policy policy,
                                          host_bounce_scope* bounce)
{
  if (destination.size() != source.size()) {
    throw std::invalid_argument("[enqueue_device_copy] source and destination extents differ");
  }
  if (source.empty()) { return device_transfer_route::none; }
  if (source.data() == nullptr || destination.data() == nullptr) {
    throw std::invalid_argument("[enqueue_device_copy] nonempty copies require non-null pointers");
  }
  if (source_space && (source_space->get_tier() != cucascade::memory::Tier::GPU ||
                       source_space->get_device_id() != source_device.value())) {
    throw std::invalid_argument("[enqueue_device_copy] source space does not match its GPU");
  }
  rmm::cuda_set_device_raii destination_guard{destination_device};
  int stream_device = -1;
  CUCASCADE_CUDA_TRY(cudaStreamGetDevice(destination_stream.get(), &stream_device));
  if (stream_device != destination_device.value()) {
    throw std::invalid_argument("[enqueue_device_copy] destination stream belongs to another GPU");
  }
  if (policy == device_transfer_policy::automatic) {
    if (source_device == destination_device) {
      wait_for_source(ready, destination_stream);
      CUCASCADE_CUDA_TRY(cudaMemcpyAsync(destination.data(),
                                         source.data(),
                                         source.size(),
                                         cudaMemcpyDeviceToDevice,
                                         destination_stream.get()));
      return device_transfer_route::local;
    }
    if (source_space && try_grant_pool_access(*source_space, destination_device.value())) {
      wait_for_source(ready, destination_stream);
      CUCASCADE_CUDA_TRY(cudaMemcpyPeerAsync(destination.data(),
                                             destination_device.value(),
                                             source.data(),
                                             source_device.value(),
                                             source.size(),
                                             destination_stream.get()));
      return device_transfer_route::peer_copy;
    }
  }

  auto& resource = staging_resource(host_staging_space);
  if (bounce != nullptr) {
    // PROTOTYPE [xfer-PR]: stage the source extent once, asynchronously; every destination uploads
    // from that HOST copy after the download's event. The scope's owner waits.
    auto& scope = bounce->state();
    auto* entry = scope.find(source.data(), source.size(), source_device.value());
    if (entry == nullptr) {
      rmm::cuda_set_device_raii source_guard{source_device};
      auto made    = std::make_unique<host_bounce_scope::impl::staged>();
      made->source = source.data();
      made->bytes  = source.size();
      made->device = source_device.value();
      made->blocks = resource.allocate_multiple_blocks(source.size());
      auto const source_stream = scope.stream_on(source_device, source_space);
      scope.stagings.push_back(std::move(made));
      auto* const staged = scope.stagings.back().get();
      try {
        wait_for_source(ready, source_stream);
        enqueue_block_leg(
          staged->blocks, const_cast<std::byte*>(source.data()), source.size(), true, source_stream);
        CUCASCADE_CUDA_TRY(cudaEventRecord(staged->done.get(), source_stream.get()));
      } catch (...) {
        (void)drain(source_stream, source_device);
        throw;
      }
      staged->valid = true;
      entry         = staged;
      proto_xfer::stats().staged_sources.fetch_add(1, std::memory_order_relaxed);
    } else {
      proto_xfer::stats().staged_once_hits.fetch_add(1, std::memory_order_relaxed);
    }
    scope.note_destination(destination_device.value(), destination_stream.get());
    try {
      CUCASCADE_CUDA_TRY(cudaStreamWaitEvent(destination_stream.get(), entry->done.get(), 0));
      enqueue_block_leg(
        entry->blocks, destination.data(), destination.size(), false, destination_stream);
    } catch (...) {
      (void)drain(destination_stream, destination_device);
      throw;
    }
    return device_transfer_route::host_staging;
  }
  auto staging   = resource.allocate_multiple_blocks(source.size());
  std::vector<void*> d2h_destinations;
  std::vector<void const*> d2h_sources;
  std::vector<void*> h2d_destinations;
  std::vector<void const*> h2d_sources;
  std::vector<std::size_t> sizes;
  d2h_destinations.reserve(staging->size());
  d2h_sources.reserve(staging->size());
  h2d_destinations.reserve(staging->size());
  h2d_sources.reserve(staging->size());
  sizes.reserve(staging->size());
  for (std::size_t offset = 0, block_index = 0; offset < source.size(); ++block_index) {
    auto const block = staging->at(block_index);
    auto const bytes = std::min(block.size(), source.size() - offset);
    d2h_destinations.push_back(block.data());
    d2h_sources.push_back(source.data() + offset);
    h2d_destinations.push_back(destination.data() + offset);
    h2d_sources.push_back(block.data());
    sizes.push_back(bytes);
    offset += bytes;
  }
  std::optional<runtime_stream_pool::lease> source_lease;
  std::optional<rmm::cuda_stream> owned_source_stream;
  rmm::cuda_set_device_raii source_guard{source_device};
  if (source_space) {
    source_lease.emplace(runtime_stream_pool::acquire(*source_space));
  } else {
    owned_source_stream.emplace(rmm::cuda_stream::flags::non_blocking);
  }
  ::cuda::stream_ref const source_stream =
    source_lease ? source_lease->get() : ::cuda::stream_ref{owned_source_stream->value()};
  bool source_pending      = false;
  bool destination_pending = false;
  try {
    wait_for_source(ready, source_stream);
    {
      rmm::cuda_set_device_raii guard{source_device};
      source_pending = true;
      if (sizes.size() == 1) {
        CUCASCADE_CUDA_TRY(cudaMemcpyAsync(d2h_destinations.front(),
                                           d2h_sources.front(),
                                           sizes.front(),
                                           cudaMemcpyDeviceToHost,
                                           source_stream.get()));
      } else {
#if CUDART_VERSION >= 12080
        cudaMemcpyAttributes attributes{};
        attributes.srcAccessOrder = cudaMemcpySrcAccessOrderStream;
        attributes.flags          = cudaMemcpyFlagDefault;
#if CUDART_VERSION < 13000
        CUCASCADE_CUDA_TRY(cudaMemcpyBatchAsync(d2h_destinations.data(),
                                                d2h_sources.data(),
                                                sizes.data(),
                                                sizes.size(),
                                                attributes,
                                                nullptr,
                                                source_stream.get()));
#else
        CUCASCADE_CUDA_TRY(cudaMemcpyBatchAsync(d2h_destinations.data(),
                                                d2h_sources.data(),
                                                sizes.data(),
                                                sizes.size(),
                                                attributes,
                                                source_stream.get()));
#endif
#else
        for (std::size_t i = 0; i < sizes.size(); ++i) {
          CUCASCADE_CUDA_TRY(cudaMemcpyAsync(d2h_destinations[i],
                                             d2h_sources[i],
                                             sizes[i],
                                             cudaMemcpyDeviceToHost,
                                             source_stream.get()));
        }
#endif
      }
      CUCASCADE_CUDA_TRY(cudaStreamSynchronize(source_stream.get()));
      source_pending = false;
    }
    {
      rmm::cuda_set_device_raii guard{destination_device};
      destination_pending = true;
      if (sizes.size() == 1) {
        CUCASCADE_CUDA_TRY(cudaMemcpyAsync(h2d_destinations.front(),
                                           h2d_sources.front(),
                                           sizes.front(),
                                           cudaMemcpyHostToDevice,
                                           destination_stream.get()));
      } else {
#if CUDART_VERSION >= 12080
        cudaMemcpyAttributes attributes{};
        attributes.srcAccessOrder = cudaMemcpySrcAccessOrderStream;
        attributes.flags          = cudaMemcpyFlagDefault;
#if CUDART_VERSION < 13000
        CUCASCADE_CUDA_TRY(cudaMemcpyBatchAsync(h2d_destinations.data(),
                                                h2d_sources.data(),
                                                sizes.data(),
                                                sizes.size(),
                                                attributes,
                                                nullptr,
                                                destination_stream.get()));
#else
        CUCASCADE_CUDA_TRY(cudaMemcpyBatchAsync(h2d_destinations.data(),
                                                h2d_sources.data(),
                                                sizes.data(),
                                                sizes.size(),
                                                attributes,
                                                destination_stream.get()));
#endif
#else
        for (std::size_t i = 0; i < sizes.size(); ++i) {
          CUCASCADE_CUDA_TRY(cudaMemcpyAsync(h2d_destinations[i],
                                             h2d_sources[i],
                                             sizes[i],
                                             cudaMemcpyHostToDevice,
                                             destination_stream.get()));
        }
#endif
      }
      CUCASCADE_CUDA_TRY(cudaStreamSynchronize(destination_stream.get()));
      destination_pending = false;
    }
  } catch (...) {
    // Synchronization retires accepted DMA before this block can return to its pool, including when
    // the checked wait reported an execution failure.
    auto const source_status = source_pending ? drain(source_stream, source_device) : cudaSuccess;
    auto const destination_status =
      destination_pending ? drain(destination_stream, destination_device) : cudaSuccess;
    if (source_status != cudaSuccess || destination_status != cudaSuccess) {
      auto const status = source_status != cudaSuccess ? source_status : destination_status;
      std::throw_with_nested(
        cucascade::cuda_error{"[enqueue_device_copy] cleanup synchronization failed: " +
                              std::string{cudaGetErrorName(status)}});
    }
    throw;
  }
  return device_transfer_route::host_staging;
}

}  // namespace sirius::memory
