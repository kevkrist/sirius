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

#include "io/deferred_device_batch.hpp"

#include "io/cache/prefetching_cache.hpp"
#include "io/io_context.hpp"
#include "io/sirius_datasource.hpp"

#include <rmm/cuda_device.hpp>

#include <cuda/stream>

#include <log/logging.hpp>

#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace sirius::io {

//===----------------------------------------------------------------------===//
// deferred_device_batch
//===----------------------------------------------------------------------===//
deferred_device_batch::deferred_device_batch(
  sirius_datasource& owner,
  cudaStream_t stream,
  int device,
  std::weak_ptr<deferred_batch_registry> registry) noexcept
  : _owner{&owner}, _stream{stream}, _device{device}, _registry{std::move(registry)}
{
}

struct deferred_device_batch::member_registration {
  explicit member_registration(std::shared_ptr<deferred_device_batch> owner) noexcept
    : batch{std::move(owner)}
  {
  }

  ~member_registration() noexcept
  {
    if (active) { batch->release_member(); }
  }

  member_registration(member_registration const&)            = delete;
  member_registration& operator=(member_registration const&) = delete;

  std::shared_ptr<deferred_device_batch> batch;
  bool active{false};
};

std::optional<std::future<std::size_t>> deferred_device_batch::try_add_member(
  std::shared_ptr<deferred_device_batch> const& batch, slice const& request)
{
  auto registration = std::make_unique<member_registration>(batch);
  auto* member      = registration.get();
  auto future       = std::async(std::launch::deferred,
                           [registration = std::move(registration), bytes = request.size()] {
                             return registration->batch->await(bytes);
                           });
  std::scoped_lock lock(batch->_mutex);
  if (batch->_state != state::OPEN) { return std::nullopt; }
  batch->_slices.push_back(request);
  ++batch->_members;
  member->active = true;
  return std::move(future);
}

void deferred_device_batch::release_member() noexcept
{
  std::scoped_lock lock(_mutex);
  if (--_members == 0 && _state == state::OPEN) { _state = state::CANCELLED; }
}

bool deferred_device_batch::is_open() const
{
  std::scoped_lock lock(_mutex);
  return _state == state::OPEN;
}

std::size_t deferred_device_batch::await(std::size_t bytes)
{
  issue_if_open();
  if (auto registry = _registry.lock()) {
    for (auto const& weak : registry->take_batches(_stream)) {
      if (auto sibling = weak.lock(); sibling && sibling.get() != this) {
        sibling->issue_if_open();
      }
    }
  }
  std::shared_future<std::size_t> completion;
  {
    std::scoped_lock lock(_mutex);
    if (_state == state::FAILED) { std::rethrow_exception(_error); }
    if (_state == state::CANCELLED) {
      throw std::logic_error(
        "sirius_datasource destroyed before its deferred device reads were waited on");
    }
    completion = _completion;
  }
  std::ignore = completion.get();
  return bytes;
}

void deferred_device_batch::issue_if_open()
{
  std::scoped_lock lock(_mutex);
  if (_state == state::OPEN && _members != 0) { issue(); }
}

void deferred_device_batch::cancel() noexcept
{
  std::scoped_lock lock(_mutex);
  if (_state == state::OPEN) { _state = state::CANCELLED; }
}

void deferred_device_batch::issue() noexcept
{
  try {
    rmm::cuda_set_device_raii device_guard{rmm::cuda_device_id{_device}};
    if (_owner->uses_prefetching_cache()) {
      auto const flush_id = _owner->io_ctx()->cache()->note_deferred_flush();
      sirius::log::log_noexcept([&] {
        SIRIUS_LOG_DEBUG(
          "sirius_datasource: vectored flush #{} ranges={} bytes={} stream={:#x} path={}",
          flush_id,
          _slices.size(),
          std::accumulate(_slices.begin(),
                          _slices.end(),
                          std::size_t{0},
                          [](std::size_t acc, slice const& s) { return acc + s.size(); }),
          reinterpret_cast<std::uintptr_t>(_stream),
          _owner->get_io_object().object_path());
      });
    }
    _completion = _owner->device_read_ranges_async(_slices, ::cuda::stream_ref{_stream}).share();
    _state      = state::ISSUED;
  } catch (...) {
    _error = std::current_exception();
    _state = state::FAILED;
  }
}

//===----------------------------------------------------------------------===//
// deferred_batch_registry
//===----------------------------------------------------------------------===//
bool deferred_batch_registry::accepts(cudaStream_t stream) noexcept
{
  return stream != nullptr && stream != cudaStreamLegacy && stream != cudaStreamPerThread;
}

void deferred_batch_registry::add(std::shared_ptr<deferred_device_batch> const& batch)
{
  std::scoped_lock lock(_mutex);
  auto& open = _open[batch->stream()];
  std::erase_if(open,
                [](std::weak_ptr<deferred_device_batch> const& weak) { return weak.expired(); });
  open.push_back(batch);
}

std::vector<std::weak_ptr<deferred_device_batch>> deferred_batch_registry::take_batches(
  cudaStream_t stream)
{
  std::scoped_lock lock(_mutex);
  auto const it = _open.find(stream);
  if (it == _open.end()) { return {}; }
  auto batches = std::move(it->second);
  _open.erase(it);
  return batches;
}

}  // namespace sirius::io
