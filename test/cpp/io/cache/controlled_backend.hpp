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

// A backend for prefetching_cache tests that performs no I/O: every grouped request the ioctx
// dispatches is parked on the reactor until the test takes it and completes it by hand, so a test
// observes exactly when a read reaches the backend and decides when, and how, it finishes.

#include "io/cache/prefetching_cache.hpp"
#include "io/io_request.hpp"
#include "io/templated_ioctx.hpp"
#include "memory/topology_index.hpp"

#include <cudf/io/text/byte_range_info.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace sirius::test::controlled {

constexpr std::size_t chunk_size = 1U << 20;

struct controlled_config {
  [[nodiscard]] std::size_t min_alignment_requirement() const noexcept { return 1; }
  [[nodiscard]] std::size_t merge_gap_size() const noexcept { return 0; }

  std::size_t n_max_concurrent_scans{1};
};

/// A four-chunk object whose bytes are whatever the test writes when it completes a request.
class controlled_object final : public sirius::io::io_object {
 public:
  explicit controlled_object(std::string path) : _path(std::move(path)) {}

  [[nodiscard]] std::string const& raw_file_cache_id() const noexcept override { return _path; }
  [[nodiscard]] std::string const& object_path() const noexcept override { return _path; }
  [[nodiscard]] std::size_t size() const noexcept override { return 4 * chunk_size; }

 private:
  std::string _path;
};

class controlled_reactor {
 public:
  using io_object_type                  = controlled_object;
  using reactor_config_type             = controlled_config;
  static constexpr bool prefers_bulk_io = false;

  /// @p staging_block_size mirrors what a real reactor takes from its host resource; 0 means "no
  /// staging", which opts the reactor out of the chunk-size check in ioctx::initialize_cache.
  explicit controlled_reactor(std::size_t staging_block_size = 0)
    : _staging_block_size(staging_block_size)
  {
  }

  [[nodiscard]] controlled_config const& get_config() const noexcept { return _config; }

  [[nodiscard]] std::size_t staging_block_size() const noexcept { return _staging_block_size; }

  void enqueue(std::unique_ptr<sirius::io::grouped_io_request> request) noexcept
  {
    {
      std::lock_guard lock(_mutex);
      _requests.push_back(std::move(request));
    }
    _ready.notify_one();
  }

  /// The oldest parked request, or null when none arrives within @p timeout.
  [[nodiscard]] std::unique_ptr<sirius::io::grouped_io_request> take_next(
    std::chrono::milliseconds timeout = std::chrono::seconds{2})
  {
    std::unique_lock lock(_mutex);
    if (!_ready.wait_for(lock, timeout, [this] { return !_requests.empty(); })) { return nullptr; }
    auto request = std::move(_requests.front());
    _requests.pop_front();
    return request;
  }

  [[nodiscard]] std::size_t pending() const
  {
    std::lock_guard lock(_mutex);
    return _requests.size();
  }

  [[nodiscard]] std::size_t queued_bytes() const noexcept { return 0; }

  std::size_t host_read(controlled_object const&,
                        std::size_t,
                        std::size_t size,
                        std::uint8_t*) const
  {
    return size;
  }

  void start() {}
  void shutdown() {}
  void interrupt() {}

  [[nodiscard]] static std::unique_ptr<controlled_object> create_io_object(std::string path)
  {
    return std::make_unique<controlled_object>(std::move(path));
  }

  [[nodiscard]] static bool supports(std::string_view) { return true; }

  [[nodiscard]] static std::vector<cudf::io::text::byte_range_info> align_and_coalesce(
    std::span<cudf::io::text::byte_range_info const> ranges, std::optional<std::size_t>)
  {
    return {ranges.begin(), ranges.end()};
  }

 private:
  controlled_config _config;
  std::size_t _staging_block_size{0};
  mutable std::mutex _mutex;
  std::condition_variable _ready;
  std::deque<std::unique_ptr<sirius::io::grouped_io_request>> _requests;
};

class controlled_context final : public sirius::io::templated_ioctx<controlled_reactor> {
 public:
  using templated_ioctx::templated_ioctx;

  [[nodiscard]] sirius::io::io_context_type type() const noexcept override
  {
    return sirius::io::io_context_type::uring;
  }

  [[nodiscard]] controlled_reactor& reactor() noexcept { return *_reactors.front(); }
};

inline std::shared_ptr<const sirius::memory::topology_index> single_gpu_topology()
{
  cucascade::memory::system_topology_info topology;
  topology.num_gpus = 1;
  cucascade::memory::gpu_topology_info gpu;
  gpu.id        = 0;
  gpu.numa_node = 0;
  topology.gpus.push_back(std::move(gpu));
  return std::make_shared<sirius::memory::topology_index>(topology, std::vector<int>{0});
}

/// Complete every slice of @p request successfully, filling any host destination with @p value, and
/// return how many slices it carried.
inline std::size_t complete_success(sirius::io::grouped_io_request& request,
                                    std::uint8_t value = 0x5a)
{
  std::size_t slices = 0;
  while (!request.empty()) {
    auto slice = request.take_front();
    ++slices;
    if (slice.h_buffer.is_contiguous()) {
      auto* destination = std::get<std::uint8_t*>(slice.h_buffer.buffer);
      std::fill_n(destination, slice.size(), value);
    } else if (slice.h_buffer.is_fragmented()) {
      for (auto* chunk : slice.h_buffer.fragments()) {
        auto const within_chunk = slice.offset() - chunk->offset;
        std::fill_n(chunk->data + within_chunk, slice.size(), value);
      }
    }
    if (slice.on_complete != nullptr) { (*slice.on_complete)(slice.h_buffer.fragments(), true); }
    request.coordinator->on_complete();
  }
  return slices;
}

/// Fail every slice of @p request with @p error. The coordinator records the first error and still
/// needs one settlement per slice before it resolves, and slices that claimed cache chunks are
/// released as failed loads so the cache does not wait on them.
inline void complete_failure(sirius::io::grouped_io_request& request, std::exception_ptr error)
{
  while (!request.empty()) {
    auto slice = request.take_front();
    if (slice.on_complete != nullptr) { (*slice.on_complete)(slice.h_buffer.fragments(), false); }
    request.coordinator->report_error(error);
  }
}

}  // namespace sirius::test::controlled
