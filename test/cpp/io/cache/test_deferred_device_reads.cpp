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

// These tests are the contract of sirius_datasource::device_read_async with a prefetching cache:
// explicit-stream calls capture the CUDA device and queue ranges without cache lookup, copies or
// I/O, the first get() on any member issues the whole batch as one vectored read, and every member
// resolves from that one completion. The [cross_source] cases cover the first waiter on a stream
// also issuing the open batches other datasources queued on it. The controlled backend parks each
// issued request, so "nothing was issued" and "one request carried every range" are both directly
// observable.

#include "catch.hpp"
#include "io/cache/config.hpp"
#include "io/cache/controlled_backend.hpp"
#include "io/cache/prefetching_cache.hpp"
#include "io/deferred_device_batch.hpp"
#include "io/io_request.hpp"
#include "io/sirius_datasource.hpp"
#include "scan/test_utils.hpp"
#include "utils/host_allocation_fault.hpp"

#include <rmm/cuda_device.hpp>
#include <rmm/cuda_stream.hpp>
#include <rmm/cuda_stream_view.hpp>
#include <rmm/device_buffer.hpp>

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using namespace sirius::test::controlled;

constexpr std::size_t read_size = 4096;

/// A cache-armed controlled backend plus one datasource on it. Device destinations are real device
/// buffers so a cache hit can be checked byte for byte; the controlled reactor never touches them.
struct deferred_fixture {
  explicit deferred_fixture(bool deferred_device_reads = true)
    : memory(initialize_memory_manager(1)), context(std::make_shared<controlled_context>(1, [] {
        return std::make_unique<controlled_reactor>();
      }))
  {
    sirius::io::cache::config config;
    config.mode                  = sirius::io::cache::cache_mode::sirius;
    config.deferred_device_reads = deferred_device_reads;
    config.apply_mode();
    context->initialize_cache(*memory, config, single_gpu_topology());
    REQUIRE(context->cache() != nullptr);
    datasource = context->open_datasource("controlled://deferred-device-reads");
  }

  ~deferred_fixture()
  {
    datasource.reset();
    context->shutdown_cache();
  }

  [[nodiscard]] std::future<std::size_t> read(std::size_t offset,
                                              rmm::device_buffer& destination,
                                              rmm::cuda_stream_view stream)
  {
    return read_on(*datasource, offset, destination, stream);
  }

  [[nodiscard]] static std::future<std::size_t> read_on(sirius::io::sirius_datasource& source,
                                                        std::size_t offset,
                                                        rmm::device_buffer& destination,
                                                        rmm::cuda_stream_view stream)
  {
    return source.device_read_async(
      offset, destination.size(), static_cast<std::uint8_t*>(destination.data()), stream);
  }

  /// A second datasource on the same cache: a different file, as the files of one multi-file split.
  [[nodiscard]] std::unique_ptr<sirius::io::sirius_datasource> open_second()
  {
    return context->open_datasource("controlled://deferred-device-reads-second");
  }

  [[nodiscard]] std::uint64_t flushes() const { return context->cache()->deferred_flushes(); }

  /// Poll until at least @p count deferred batches have been issued through the cache; false when
  /// @p timeout passes first.
  [[nodiscard]] bool await_flushes(std::uint64_t count,
                                   std::chrono::milliseconds timeout = 2s) const
  {
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (flushes() < count) {
      if (std::chrono::steady_clock::now() >= deadline) { return false; }
      std::this_thread::sleep_for(1ms);
    }
    return true;
  }

  /// Advise and prepare @p size bytes at @p offset, then start their prefetch and return the parked
  /// backend request for the test to complete when it chooses.
  [[nodiscard]] std::unique_ptr<sirius::io::grouped_io_request> start_prefetch(std::size_t offset,
                                                                               std::size_t size)
  {
    return start_prefetch_on(*datasource, offset, size);
  }

  [[nodiscard]] std::unique_ptr<sirius::io::grouped_io_request> start_prefetch_on(
    sirius::io::sirius_datasource& source, std::size_t offset, std::size_t size)
  {
    std::array<cudf::io::text::byte_range_info, 1> ranges{cudf::io::text::byte_range_info{
      static_cast<std::int64_t>(offset), static_cast<std::int64_t>(size)}};
    source.fadvise(ranges, 0);
    REQUIRE(source.prepare_prefetch(false) == sirius::io::prepare_result::prepared);
    REQUIRE(source.prefetch_async([](bool) noexcept {}) == sirius::io::prefetch_refusal::issued);
    auto prefetch = context->reactor().take_next();
    REQUIRE(prefetch != nullptr);
    return prefetch;
  }

  std::unique_ptr<sirius::memory::sirius_memory_reservation_manager> memory;
  std::shared_ptr<controlled_context> context;
  std::unique_ptr<sirius::io::sirius_datasource> datasource;
};

/**
 * @brief Owns the threads a test starts and the backend requests it takes, so that no assertion can
 * leave the test while one of those threads is blocked on a parked request.
 *
 * On destruction every held request that is still unsettled is failed, and so is every request the
 * backend delivers meanwhile (failing a parked prefetch can release a waiter that then issues new
 * requests), until the threads have finished or 10 s have passed; then the threads are joined.
 * Declare a scope after the streams and device buffers its threads use, so it is destroyed first.
 */
class deferred_scope {
 public:
  explicit deferred_scope(deferred_fixture& fixture) noexcept : _fixture{fixture} {}

  deferred_scope(deferred_scope const&)            = delete;
  deferred_scope& operator=(deferred_scope const&) = delete;
  deferred_scope(deferred_scope&&)                 = delete;
  deferred_scope& operator=(deferred_scope&&)      = delete;

  ~deferred_scope()
  {
    auto const error = std::make_exception_ptr(std::runtime_error("deferred_scope teardown"));
    settle(error, 0ms);
    auto const deadline = std::chrono::steady_clock::now() + 10s;
    while (!threads_finished() && std::chrono::steady_clock::now() < deadline) {
      settle(error, 50ms);
    }
    // Destroying an std::async future joins its thread.
    _value_threads.clear();
    _void_threads.clear();
  }

  /// The next request the backend receives, kept by this scope; null when none arrives within @p
  /// timeout.
  [[nodiscard]] sirius::io::grouped_io_request* take_next(std::chrono::milliseconds timeout = 2s)
  {
    auto request = _fixture.context->reactor().take_next(timeout);
    return request == nullptr ? nullptr : adopt(std::move(request));
  }

  /// Keep @p request, taken before this scope existed (a parked prefetch).
  sirius::io::grouped_io_request* adopt(std::unique_ptr<sirius::io::grouped_io_request> request)
  {
    return _requests.emplace_back(std::move(request)).get();
  }

  /// Run @p work on another thread.
  template <std::invocable F>
  auto& run_async(F work)
  {
    auto thread = std::async(std::launch::async, std::move(work));
    if constexpr (std::is_void_v<std::invoke_result_t<F>>) {
      return _void_threads.emplace_back(std::move(thread));
    } else {
      return _value_threads.emplace_back(std::move(thread));
    }
  }

  /// Wait on @p read from another thread, since get() blocks until the test completes the parked
  /// request.
  std::future<std::size_t>& get_async(std::future<std::size_t> read)
  {
    return run_async([read = std::move(read)]() mutable { return read.get(); });
  }

 private:
  void settle(std::exception_ptr const& error, std::chrono::milliseconds wait)
  {
    for (auto const& request : _requests) {
      complete_failure(*request, error);
    }
    while (auto* request = take_next(wait)) {
      complete_failure(*request, error);
    }
  }

  [[nodiscard]] bool threads_finished() const
  {
    auto const finished = [](auto const& thread) {
      return !thread.valid() || thread.wait_for(0ms) == std::future_status::ready;
    };
    return std::ranges::all_of(_value_threads, finished) &&
           std::ranges::all_of(_void_threads, finished);
  }

  deferred_fixture& _fixture;
  std::deque<std::unique_ptr<sirius::io::grouped_io_request>> _requests;
  std::deque<std::future<std::size_t>> _value_threads;
  std::deque<std::future<void>> _void_threads;
};

std::vector<std::uint8_t> download(rmm::device_buffer const& buffer, rmm::cuda_stream const& stream)
{
  std::vector<std::uint8_t> host(buffer.size());
  REQUIRE(cudaMemcpyAsync(
            host.data(), buffer.data(), host.size(), cudaMemcpyDeviceToHost, stream.value()) ==
          cudaSuccess);
  stream.synchronize();
  return host;
}

}  // namespace

TEST_CASE("deferred device reads issue nothing until a member is waited on, then one request",
          "[cache][deferred_device_reads][gpu_execution]")
{
  deferred_fixture fixture;

  rmm::cuda_stream stream;
  std::array<rmm::device_buffer, 3> destinations{rmm::device_buffer{read_size, stream},
                                                 rmm::device_buffer{read_size, stream},
                                                 rmm::device_buffer{read_size, stream}};
  deferred_scope scope{fixture};
  std::vector<std::future<std::size_t>> reads;
  for (std::size_t i = 0; i < destinations.size(); ++i) {
    reads.push_back(fixture.read(i * chunk_size, destinations[i], stream));
  }

  // Queued only: no request reached the backend and no flush happened.
  CHECK(fixture.context->reactor().pending() == 0);
  CHECK(fixture.flushes() == 0);
  for (auto const& read : reads) {
    CHECK(read.wait_for(0ms) == std::future_status::deferred);
  }

  auto& waiting = scope.get_async(std::move(reads[1]));
  auto* request = scope.take_next();
  REQUIRE(request != nullptr);
  CHECK(fixture.context->reactor().pending() == 0);
  CHECK(fixture.flushes() == 1);

  REQUIRE(complete_success(*request) == destinations.size());
  REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
  CHECK(waiting.get() == read_size);
  CHECK(reads[0].get() == read_size);
  CHECK(reads[2].get() == read_size);
  CHECK(fixture.flushes() == 1);
}

TEST_CASE("deferred device reads on different streams are issued as separate batches",
          "[cache][deferred_device_reads][gpu_execution]")
{
  deferred_fixture fixture;

  rmm::cuda_stream first_stream;
  rmm::cuda_stream second_stream;
  rmm::device_buffer first_destination{read_size, first_stream};
  rmm::device_buffer second_destination{read_size, second_stream};
  deferred_scope scope{fixture};
  auto first  = fixture.read(0, first_destination, first_stream);
  auto second = fixture.read(chunk_size, second_destination, second_stream);
  CHECK(fixture.context->reactor().pending() == 0);

  auto& waiting_first = scope.get_async(std::move(first));
  auto* first_request = scope.take_next();
  REQUIRE(first_request != nullptr);
  CHECK(fixture.context->reactor().pending() == 0);
  CHECK(fixture.flushes() == 1);
  CHECK(complete_success(*first_request) == 1);
  REQUIRE(waiting_first.wait_for(2s) == std::future_status::ready);
  CHECK(waiting_first.get() == read_size);

  auto& waiting_second = scope.get_async(std::move(second));
  auto* second_request = scope.take_next();
  REQUIRE(second_request != nullptr);
  CHECK(fixture.flushes() == 2);
  CHECK(complete_success(*second_request) == 1);
  REQUIRE(waiting_second.wait_for(2s) == std::future_status::ready);
  CHECK(waiting_second.get() == read_size);
}

TEST_CASE("every member of a deferred batch resolves to its own clamped byte count",
          "[cache][deferred_device_reads][gpu_execution]")
{
  deferred_fixture fixture;
  auto const file_size = fixture.datasource->size();

  rmm::cuda_stream stream;
  rmm::device_buffer whole{read_size, stream};
  rmm::device_buffer tail{read_size, stream};
  deferred_scope scope{fixture};
  auto whole_read = fixture.read(0, whole, stream);
  auto tail_read  = fixture.read(file_size - 100, tail, stream);

  auto& waiting = scope.get_async(std::move(tail_read));
  auto* request = scope.take_next();
  REQUIRE(request != nullptr);
  REQUIRE(complete_success(*request) == 2);
  REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
  CHECK(waiting.get() == 100);
  CHECK(whole_read.get() == read_size);
}

TEST_CASE("a backend failure surfaces from every member of a deferred batch",
          "[cache][deferred_device_reads][gpu_execution]")
{
  deferred_fixture fixture;

  rmm::cuda_stream stream;
  rmm::device_buffer first_destination{read_size, stream};
  rmm::device_buffer second_destination{read_size, stream};
  deferred_scope scope{fixture};
  auto first  = fixture.read(0, first_destination, stream);
  auto second = fixture.read(chunk_size, second_destination, stream);

  auto& waiting_first = scope.get_async(std::move(first));
  auto* request       = scope.take_next();
  REQUIRE(request != nullptr);
  auto& waiting_second = scope.get_async(std::move(second));
  CHECK(fixture.context->reactor().pending() == 0);

  complete_failure(*request, std::make_exception_ptr(std::runtime_error("controlled failure")));
  REQUIRE(waiting_first.wait_for(2s) == std::future_status::ready);
  REQUIRE(waiting_second.wait_for(2s) == std::future_status::ready);
  CHECK_THROWS(waiting_first.get());
  CHECK_THROWS(waiting_second.get());
}

TEST_CASE("degenerate deferred device reads settle immediately and join no batch",
          "[cache][deferred_device_reads][gpu_execution]")
{
  deferred_fixture fixture;
  auto const file_size = fixture.datasource->size();

  rmm::cuda_stream stream;
  rmm::device_buffer destination{read_size, stream};
  deferred_scope scope{fixture};
  auto* dst = static_cast<std::uint8_t*>(destination.data());

  auto empty = fixture.datasource->device_read_async(0, 0, dst, stream);
  REQUIRE(empty.wait_for(0ms) == std::future_status::ready);
  CHECK(empty.get() == 0);

  auto past_end = fixture.datasource->device_read_async(file_size, read_size, dst, stream);
  REQUIRE(past_end.wait_for(0ms) == std::future_status::ready);
  CHECK(past_end.get() == 0);

  auto null_destination = fixture.datasource->device_read_async(0, read_size, nullptr, stream);
  REQUIRE(null_destination.wait_for(0ms) == std::future_status::ready);
  CHECK_THROWS_AS(null_destination.get(), std::invalid_argument);

  // The next real read opens a fresh batch that carries only itself.
  auto& waiting = scope.get_async(fixture.read(0, destination, stream));
  auto* request = scope.take_next();
  REQUIRE(request != nullptr);
  CHECK(complete_success(*request) == 1);
  REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
  CHECK(waiting.get() == read_size);
  CHECK(fixture.flushes() == 1);
}

TEST_CASE("destroying the datasource before a wait fails its unissued batch",
          "[cache][deferred_device_reads][gpu_execution]")
{
  deferred_fixture fixture;

  rmm::cuda_stream stream;
  rmm::device_buffer destination{read_size, stream};
  auto read = fixture.read(0, destination, stream);
  CHECK(read.wait_for(0ms) == std::future_status::deferred);

  fixture.datasource.reset();

  CHECK(fixture.context->reactor().pending() == 0);
  CHECK_THROWS_AS(read.get(), std::logic_error);
  CHECK(fixture.context->reactor().pending() == 0);
  CHECK(fixture.flushes() == 0);
}

TEST_CASE("a deferred cache hit is copied to the device when the batch is issued",
          "[cache][deferred_device_reads][gpu_execution]")
{
  deferred_fixture fixture;
  auto prefetch = fixture.start_prefetch(0, read_size);
  complete_success(*prefetch, 0x6c);

  rmm::cuda_stream stream;
  rmm::device_buffer destination{read_size, stream};
  deferred_scope scope{fixture};
  auto read = fixture.read(0, destination, stream);
  CHECK(read.wait_for(0ms) == std::future_status::deferred);

  // Entirely cache-resident: the issue is one memcpy batch and no backend request.
  auto& waiting = scope.get_async(std::move(read));
  REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
  CHECK(waiting.get() == read_size);
  CHECK(fixture.context->reactor().pending() == 0);
  auto const host = download(destination, stream);
  CHECK(std::ranges::all_of(host, [](auto byte) { return byte == 0x6c; }));
}

TEST_CASE("the synchronous device_read issues its own batch and returns the byte count",
          "[cache][deferred_device_reads][gpu_execution]")
{
  deferred_fixture fixture;

  rmm::cuda_stream stream;
  rmm::device_buffer destination{read_size, stream};
  deferred_scope scope{fixture};
  auto& call    = scope.run_async([&] {
    return fixture.datasource->device_read(
      0, read_size, static_cast<std::uint8_t*>(destination.data()), stream);
  });
  auto* request = scope.take_next();
  REQUIRE(request != nullptr);
  CHECK(complete_success(*request) == 1);
  REQUIRE(call.wait_for(2s) == std::future_status::ready);
  CHECK(call.get() == read_size);
}

TEST_CASE("with deferred_device_reads off every device read is issued as it is called",
          "[cache][deferred_device_reads][gpu_execution]")
{
  deferred_fixture fixture{/*deferred_device_reads=*/false};

  rmm::cuda_stream stream;
  rmm::device_buffer destination{read_size, stream};
  deferred_scope scope{fixture};
  auto read     = fixture.read(0, destination, stream);
  auto* request = scope.take_next();
  REQUIRE(request != nullptr);
  CHECK(read.wait_for(0ms) == std::future_status::timeout);

  CHECK(complete_success(*request) == 1);
  REQUIRE(read.wait_for(2s) == std::future_status::ready);
  CHECK(read.get() == read_size);
  CHECK(fixture.flushes() == 0);
}

TEST_CASE("concurrent waiters on one deferred batch share a single issue",
          "[cache][deferred_device_reads][gpu_execution]")
{
  deferred_fixture fixture;

  rmm::cuda_stream stream;
  std::array<rmm::device_buffer, 3> destinations{rmm::device_buffer{read_size, stream},
                                                 rmm::device_buffer{read_size, stream},
                                                 rmm::device_buffer{read_size, stream}};
  deferred_scope scope{fixture};
  std::vector<std::future<std::size_t>> reads;
  for (std::size_t i = 0; i < destinations.size(); ++i) {
    reads.push_back(fixture.read(i * chunk_size, destinations[i], stream));
  }

  std::vector<std::future<std::size_t>*> waiters;
  for (auto& read : reads) {
    waiters.push_back(&scope.get_async(std::move(read)));
  }

  auto* request = scope.take_next();
  REQUIRE(request != nullptr);
  CHECK(scope.take_next(100ms) == nullptr);
  CHECK(fixture.flushes() == 1);
  CHECK(complete_success(*request) == destinations.size());
  for (auto* waiter : waiters) {
    REQUIRE(waiter->wait_for(2s) == std::future_status::ready);
    CHECK(waiter->get() == read_size);
  }
  CHECK(fixture.flushes() == 1);
}

TEST_CASE("a same-stream read after the batch was issued opens a new batch",
          "[cache][deferred_device_reads][gpu_execution]")
{
  deferred_fixture fixture;

  rmm::cuda_stream stream;
  rmm::device_buffer first_destination{read_size, stream};
  rmm::device_buffer second_destination{read_size, stream};
  deferred_scope scope{fixture};
  auto& waiting_first = scope.get_async(fixture.read(0, first_destination, stream));
  auto* first_request = scope.take_next();
  REQUIRE(first_request != nullptr);
  CHECK(fixture.flushes() == 1);

  // The first batch is closed: this read must not join it (its dst would never be written).
  auto second = fixture.read(chunk_size, second_destination, stream);
  CHECK(second.wait_for(0ms) == std::future_status::deferred);
  CHECK(fixture.context->reactor().pending() == 0);

  auto& waiting_second = scope.get_async(std::move(second));
  auto* second_request = scope.take_next();
  REQUIRE(second_request != nullptr);
  CHECK(fixture.flushes() == 2);

  CHECK(complete_success(*first_request) == 1);
  CHECK(complete_success(*second_request) == 1);
  REQUIRE(waiting_first.wait_for(2s) == std::future_status::ready);
  REQUIRE(waiting_second.wait_for(2s) == std::future_status::ready);
  CHECK(waiting_first.get() == read_size);
  CHECK(waiting_second.get() == read_size);
}

TEST_CASE("a dropped member is still issued when a sibling of its batch is waited on",
          "[cache][deferred_device_reads][gpu_execution]")
{
  deferred_fixture fixture;

  rmm::cuda_stream stream;
  rmm::device_buffer kept_destination{read_size, stream};
  rmm::device_buffer dropped_destination{read_size, stream};
  deferred_scope scope{fixture};
  auto kept = fixture.read(0, kept_destination, stream);
  {
    auto dropped = fixture.read(chunk_size, dropped_destination, stream);
    CHECK(dropped.wait_for(0ms) == std::future_status::deferred);
  }

  auto& waiting = scope.get_async(std::move(kept));
  auto* request = scope.take_next();
  REQUIRE(request != nullptr);
  CHECK(complete_success(*request) == 2);
  REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
  CHECK(waiting.get() == read_size);
  CHECK(fixture.flushes() == 1);
}

TEST_CASE("a batch whose members were all dropped expires instead of being joined",
          "[cache][deferred_device_reads][gpu_execution]")
{
  deferred_fixture fixture;

  rmm::cuda_stream stream;
  rmm::device_buffer abandoned_destination{read_size, stream};
  rmm::device_buffer destination{read_size, stream};
  deferred_scope scope{fixture};
  {
    auto abandoned = fixture.read(0, abandoned_destination, stream);
    CHECK(abandoned.wait_for(0ms) == std::future_status::deferred);
  }

  auto& waiting = scope.get_async(fixture.read(chunk_size, destination, stream));
  auto* request = scope.take_next();
  REQUIRE(request != nullptr);
  CHECK(complete_success(*request) == 1);
  REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
  CHECK(waiting.get() == read_size);
  CHECK(fixture.flushes() == 1);
}

TEST_CASE("a mixed hit and miss deferred batch issues one request for the non-resident pieces",
          "[cache][deferred_device_reads][gpu_execution]")
{
  deferred_fixture fixture;
  auto prefetch = fixture.start_prefetch(0, chunk_size);
  complete_success(*prefetch, 0x6c);

  rmm::cuda_stream stream;
  rmm::device_buffer hit_destination{read_size, stream};
  rmm::device_buffer miss_destination{read_size, stream};
  rmm::device_buffer straddle_destination{read_size, stream};
  deferred_scope scope{fixture};
  auto hit      = fixture.read(0, hit_destination, stream);
  auto miss     = fixture.read(chunk_size, miss_destination, stream);
  auto straddle = fixture.read(chunk_size - read_size / 2, straddle_destination, stream);
  CHECK(fixture.context->reactor().pending() == 0);

  auto& waiting = scope.get_async(std::move(straddle));
  auto* request = scope.take_next();
  REQUIRE(request != nullptr);
  CHECK(fixture.context->reactor().pending() == 0);
  CHECK(fixture.flushes() == 1);
  CHECK(waiting.wait_for(100ms) == std::future_status::timeout);

  // The miss and the non-resident half of the straddle; the hit and the resident half are copies.
  REQUIRE(complete_success(*request) == 2);
  REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
  CHECK(waiting.get() == read_size);
  CHECK(hit.get() == read_size);
  CHECK(miss.get() == read_size);

  auto const hit_bytes = download(hit_destination, stream);
  CHECK(std::ranges::all_of(hit_bytes, [](auto byte) { return byte == 0x6c; }));
  auto const straddle_bytes = download(straddle_destination, stream);
  CHECK(std::all_of(straddle_bytes.begin(),
                    straddle_bytes.begin() + static_cast<std::ptrdiff_t>(read_size / 2),
                    [](auto byte) { return byte == 0x6c; }));
}

TEST_CASE(
  "a batch that pinned a hit and then failed on a miss fails every member and releases the pin",
  "[cache][deferred_device_reads][gpu_execution]")
{
  deferred_fixture fixture;
  auto prefetch = fixture.start_prefetch(0, read_size);
  complete_success(*prefetch, 0x6c);
  auto& cache = *fixture.context->cache();
  REQUIRE(cache.claimed_bytes() > 0);

  rmm::cuda_stream stream;
  rmm::device_buffer hit_destination{read_size, stream};
  rmm::device_buffer miss_destination{read_size, stream};
  deferred_scope scope{fixture};
  auto hit  = fixture.read(0, hit_destination, stream);
  auto miss = fixture.read(chunk_size, miss_destination, stream);

  auto& waiting_hit = scope.get_async(std::move(hit));
  auto* request     = scope.take_next();
  REQUIRE(request != nullptr);
  auto& waiting_miss = scope.get_async(std::move(miss));

  complete_failure(*request, std::make_exception_ptr(std::runtime_error("controlled failure")));
  REQUIRE(waiting_hit.wait_for(2s) == std::future_status::ready);
  REQUIRE(waiting_miss.wait_for(2s) == std::future_status::ready);
  CHECK_THROWS(waiting_hit.get());
  CHECK_THROWS(waiting_miss.get());

  // The hit's pin is released once the stream passes its copy, after which the chunk is idle and
  // evictable. Retirement settles on the completion poll thread, so give it a bounded moment.
  fixture.datasource.reset();
  auto const deadline = std::chrono::steady_clock::now() + 2s;
  while (cache.claimed_bytes() != 0 && std::chrono::steady_clock::now() < deadline) {
    cache.evict_sync(cache.claimed_bytes());
    std::this_thread::sleep_for(20ms);
  }
  CHECK(cache.claimed_bytes() == 0);
}

TEST_CASE("an issue blocked behind the prefetch holds the batch: siblings and the destructor wait",
          "[cache][deferred_device_reads][gpu_execution]")
{
  deferred_fixture fixture;
  // Parked, not completed: the chunk stays `loading` under an in-flight prefetch, so the first
  // waiter's issue blocks in the cache until it lands.
  auto parked_prefetch = fixture.start_prefetch(0, chunk_size);

  rmm::cuda_stream stream;
  rmm::device_buffer first_destination{read_size, stream};
  rmm::device_buffer second_destination{read_size, stream};
  deferred_scope scope{fixture};
  auto* prefetch = scope.adopt(std::move(parked_prefetch));
  auto first     = fixture.read(0, first_destination, stream);
  auto second    = fixture.read(read_size, second_destination, stream);

  auto& waiting_first  = scope.get_async(std::move(first));
  auto& waiting_second = scope.get_async(std::move(second));
  // A waiter is inside issue(), holding the batch mutex.
  REQUIRE(fixture.await_flushes(1));
  auto& destroying = scope.run_async([&fixture] { fixture.datasource.reset(); });

  CHECK(waiting_first.wait_for(100ms) == std::future_status::timeout);
  CHECK(waiting_second.wait_for(100ms) == std::future_status::timeout);
  CHECK(destroying.wait_for(100ms) == std::future_status::timeout);
  CHECK(fixture.context->reactor().pending() == 0);

  complete_success(*prefetch, 0x6c);
  REQUIRE(waiting_first.wait_for(2s) == std::future_status::ready);
  REQUIRE(waiting_second.wait_for(2s) == std::future_status::ready);
  REQUIRE(destroying.wait_for(2s) == std::future_status::ready);
  CHECK(waiting_first.get() == read_size);
  CHECK(waiting_second.get() == read_size);
  CHECK(fixture.context->reactor().pending() == 0);
  auto const bytes = download(first_destination, stream);
  CHECK(std::ranges::all_of(bytes, [](auto byte) { return byte == 0x6c; }));
}

TEST_CASE("the cache summary reports deferred flushes globally and per query cycle",
          "[cache][deferred_device_reads][gpu_execution]")
{
  deferred_fixture fixture;

  rmm::cuda_stream stream;
  rmm::device_buffer destination{read_size, stream};
  deferred_scope scope{fixture};
  auto& waiting = scope.get_async(fixture.read(0, destination, stream));
  auto* request = scope.take_next();
  REQUIRE(request != nullptr);
  CHECK(complete_success(*request) == 1);
  REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
  CHECK(waiting.get() == read_size);

  auto& cache             = *fixture.context->cache();
  auto const before_cycle = cache.summary();
  CHECK(std::regex_search(
    before_cycle,
    std::regex(R"(global\[[^\]]*deferred_flushes=1\] last_cycle\[[^\]]*deferred_flushes=1\])")));

  cache.prepare_for_query();
  auto const after_cycle = cache.summary();
  CHECK(std::regex_search(
    after_cycle,
    std::regex(R"(global\[[^\]]*deferred_flushes=1\] last_cycle\[[^\]]*deferred_flushes=0\])")));
}

TEST_CASE(
  "a waiter issues its later registered batch before siblings without waiting for completion",
  "[cache][deferred_device_reads][cross_source][gpu_execution]")
{
  deferred_fixture fixture;
  auto second = fixture.open_second();

  rmm::cuda_stream stream;
  rmm::device_buffer first_destination{read_size, stream};
  rmm::device_buffer second_destination{read_size, stream};
  deferred_scope scope{fixture};
  auto first_read  = fixture.read(0, first_destination, stream);
  auto second_read = deferred_fixture::read_on(*second, chunk_size, second_destination, stream);
  CHECK(fixture.context->reactor().pending() == 0);

  // Wait on the batch registered last, distinguishing requested-batch priority from registry order.
  auto& waiting        = scope.get_async(std::move(second_read));
  auto* first_request  = scope.take_next();
  auto* second_request = scope.take_next();
  REQUIRE(first_request != nullptr);
  REQUIRE(second_request != nullptr);
  CHECK(first_request->front().offset() == chunk_size);
  CHECK(second_request->front().offset() == 0);
  CHECK(fixture.context->reactor().pending() == 0);
  CHECK(fixture.flushes() == 2);

  CHECK(complete_success(*first_request) == 1);
  REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
  CHECK(waiting.get() == read_size);
  auto& sibling_waiting = scope.get_async(std::move(first_read));
  CHECK(sibling_waiting.wait_for(100ms) == std::future_status::timeout);
  CHECK(complete_success(*second_request) == 1);
  REQUIRE(sibling_waiting.wait_for(2s) == std::future_status::ready);
  CHECK(sibling_waiting.get() == read_size);
  CHECK(fixture.flushes() == 2);
}

TEST_CASE("a cancelled requested batch still issues its live sibling",
          "[cache][deferred_device_reads][cross_source][gpu_execution]")
{
  deferred_fixture fixture;
  auto second = fixture.open_second();
  rmm::cuda_stream stream;
  rmm::device_buffer first_destination{read_size, stream};
  rmm::device_buffer second_destination{read_size, stream};
  deferred_scope scope{fixture};
  auto first_read  = fixture.read(0, first_destination, stream);
  auto second_read = deferred_fixture::read_on(*second, chunk_size, second_destination, stream);
  fixture.datasource.reset();

  auto& waiting = scope.get_async(std::move(first_read));
  auto* request = scope.take_next();
  REQUIRE(request != nullptr);
  CHECK(request->front().offset() == chunk_size);
  REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
  CHECK_THROWS_AS(waiting.get(), std::logic_error);
  CHECK(complete_success(*request) == 1);
  CHECK(second_read.get() == read_size);
  CHECK(fixture.flushes() == 1);
  CHECK(fixture.context->reactor().pending() == 0);
}

TEST_CASE("sibling batches on a different stream are left to their own waiters",
          "[cache][deferred_device_reads][cross_source][gpu_execution]")
{
  deferred_fixture fixture;
  auto second = fixture.open_second();

  rmm::cuda_stream first_stream;
  rmm::cuda_stream second_stream;
  rmm::device_buffer first_destination{read_size, first_stream};
  rmm::device_buffer second_destination{read_size, second_stream};
  deferred_scope scope{fixture};
  auto first_read = fixture.read(0, first_destination, first_stream);
  auto second_read =
    deferred_fixture::read_on(*second, chunk_size, second_destination, second_stream);

  auto& waiting_first = scope.get_async(std::move(first_read));
  auto* first_request = scope.take_next();
  REQUIRE(first_request != nullptr);
  CHECK(scope.take_next(100ms) == nullptr);
  CHECK(fixture.flushes() == 1);
  CHECK(complete_success(*first_request) == 1);
  REQUIRE(waiting_first.wait_for(2s) == std::future_status::ready);
  CHECK(waiting_first.get() == read_size);

  auto& waiting_second = scope.get_async(std::move(second_read));
  auto* second_request = scope.take_next();
  REQUIRE(second_request != nullptr);
  CHECK(complete_success(*second_request) == 1);
  REQUIRE(waiting_second.wait_for(2s) == std::future_status::ready);
  CHECK(waiting_second.get() == read_size);
  CHECK(fixture.flushes() == 2);
}

TEST_CASE("shared default stream device reads issue eagerly on the submitting thread",
          "[cache][deferred_device_reads][cross_source][gpu_execution]")
{
  auto const shared =
    GENERATE(rmm::cuda_stream_default, rmm::cuda_stream_legacy, rmm::cuda_stream_per_thread);
  deferred_fixture fixture;
  auto second = fixture.open_second();

  rmm::device_buffer first_destination{read_size, shared};
  rmm::device_buffer second_destination{read_size, shared};
  deferred_scope scope{fixture};
  auto first_read  = fixture.read(0, first_destination, shared);
  auto second_read = deferred_fixture::read_on(*second, chunk_size, second_destination, shared);

  CHECK(first_read.wait_for(0ms) == std::future_status::timeout);
  CHECK(second_read.wait_for(0ms) == std::future_status::timeout);
  CHECK(fixture.context->reactor().pending() == 2);
  auto* first_request  = scope.take_next();
  auto* second_request = scope.take_next();
  REQUIRE(first_request != nullptr);
  REQUIRE(second_request != nullptr);
  CHECK(complete_success(*first_request) == 1);
  CHECK(complete_success(*second_request) == 1);
  auto& first_waiter  = scope.get_async(std::move(first_read));
  auto& second_waiter = scope.get_async(std::move(second_read));
  REQUIRE(first_waiter.wait_for(2s) == std::future_status::ready);
  REQUIRE(second_waiter.wait_for(2s) == std::future_status::ready);
  CHECK(first_waiter.get() == read_size);
  CHECK(second_waiter.get() == read_size);
  CHECK(fixture.flushes() == 0);
  shared.synchronize();
}

TEST_CASE("a per-thread default stream cache hit keeps its submitting stream",
          "[cache][deferred_device_reads][gpu_execution]")
{
  deferred_fixture fixture;
  auto prefetch = fixture.start_prefetch(0, read_size);
  complete_success(*prefetch, 0x7b);
  auto stream = rmm::cuda_stream_per_thread;
  rmm::device_buffer destination{read_size, stream};
  deferred_scope scope{fixture};
  auto read = fixture.read(0, destination, stream);
  CHECK(read.wait_for(0ms) != std::future_status::deferred);
  auto& waiting = scope.get_async(std::move(read));
  REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
  CHECK(waiting.get() == read_size);
  std::vector<std::uint8_t> bytes(read_size);
  REQUIRE(
    cudaMemcpyAsync(
      bytes.data(), destination.data(), bytes.size(), cudaMemcpyDeviceToHost, stream.value()) ==
    cudaSuccess);
  stream.synchronize();
  CHECK(std::ranges::all_of(bytes, [](auto byte) { return byte == 0x7b; }));
  CHECK(fixture.flushes() == 0);
}

TEST_CASE("migrated deferred reads preserve GPU one and restore the waiter device",
          "[cache][deferred_device_reads][multi_gpu][gpu_execution]")
{
  int devices = 0;
  REQUIRE(cudaGetDeviceCount(&devices) == cudaSuccess);
  if (devices < 2) {
    WARN("requires two visible GPUs");
    return;
  }
  deferred_fixture fixture;
  rmm::cuda_set_device_raii submitting_device{rmm::cuda_device_id{1}};
  rmm::cuda_stream stream;
  rmm::device_buffer destination{read_size, stream};
  deferred_scope scope{fixture};
  auto read = fixture.read(0, destination, stream);
  std::atomic<std::size_t> bytes_read{0};
  auto& waiter_device = scope.run_async([read = std::move(read), &bytes_read]() mutable {
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{0}};
    bytes_read.store(read.get());
    return static_cast<std::size_t>(rmm::get_current_cuda_device().value());
  });
  auto* request       = scope.take_next();
  REQUIRE(request != nullptr);
  auto const& slice = request->front();
  REQUIRE(slice.d_buffer.device_id == 1);
  REQUIRE(slice.d_buffer.stream.get() == stream.value());
  // Exercise a real transfer using the request's device and stream, just as a backend does.
  std::vector<std::uint8_t> payload(read_size, 0x5d);
  {
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{slice.d_buffer.device_id}};
    REQUIRE(cudaMemcpyAsync(slice.d_buffer.data,
                            payload.data(),
                            payload.size(),
                            cudaMemcpyHostToDevice,
                            slice.d_buffer.stream.get()) == cudaSuccess);
    REQUIRE(cudaStreamSynchronize(slice.d_buffer.stream.get()) == cudaSuccess);
  }
  CHECK(complete_success(*request) == 1);
  REQUIRE(waiter_device.wait_for(2s) == std::future_status::ready);
  CHECK(waiter_device.get() == 0);
  CHECK(bytes_read.load() == read_size);
  CHECK(download(destination, stream) == payload);
}

TEST_CASE("an expired sibling batch is skipped by the waiter",
          "[cache][deferred_device_reads][cross_source][gpu_execution]")
{
  deferred_fixture fixture;
  auto second = fixture.open_second();

  rmm::cuda_stream stream;
  rmm::device_buffer first_destination{read_size, stream};
  rmm::device_buffer dropped_destination{read_size, stream};
  deferred_scope scope{fixture};
  {
    auto dropped = deferred_fixture::read_on(*second, chunk_size, dropped_destination, stream);
    CHECK(dropped.wait_for(0ms) == std::future_status::deferred);
  }

  auto& waiting = scope.get_async(fixture.read(0, first_destination, stream));
  auto* request = scope.take_next();
  REQUIRE(request != nullptr);
  CHECK(scope.take_next(100ms) == nullptr);
  CHECK(fixture.flushes() == 1);
  CHECK(complete_success(*request) == 1);
  REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
  CHECK(waiting.get() == read_size);
}

TEST_CASE("a failure in a sibling's read does not fail the waiter's own members",
          "[cache][deferred_device_reads][cross_source][gpu_execution]")
{
  deferred_fixture fixture;
  auto second = fixture.open_second();

  rmm::cuda_stream stream;
  rmm::device_buffer first_destination{read_size, stream};
  rmm::device_buffer second_destination{read_size, stream};
  deferred_scope scope{fixture};
  auto first_read  = fixture.read(0, first_destination, stream);
  auto second_read = deferred_fixture::read_on(*second, chunk_size, second_destination, stream);

  auto& waiting = scope.get_async(std::move(first_read));
  auto* one     = scope.take_next();
  auto* two     = scope.take_next();
  REQUIRE(one != nullptr);
  REQUIRE(two != nullptr);
  if (one->front().offset() != 0) { std::swap(one, two); }
  REQUIRE(one->front().offset() == 0);
  REQUIRE(two->front().offset() == chunk_size);

  complete_failure(*two, std::make_exception_ptr(std::runtime_error("sibling failure")));
  CHECK(complete_success(*one) == 1);
  REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
  CHECK(waiting.get() == read_size);
  CHECK_THROWS(second_read.get());
}

TEST_CASE("destroying a sibling's datasource while the waiter is issuing it waits for the issue",
          "[cache][deferred_device_reads][cross_source][gpu_execution]")
{
  deferred_fixture fixture;
  auto second = fixture.open_second();
  // Parked prefetch on the second file: issuing its batch blocks in the cache until it lands.
  auto parked_prefetch = fixture.start_prefetch_on(*second, 0, chunk_size);

  rmm::cuda_stream stream;
  rmm::device_buffer second_destination{read_size, stream};
  rmm::device_buffer first_destination{read_size, stream};
  deferred_scope scope{fixture};
  auto* prefetch   = scope.adopt(std::move(parked_prefetch));
  auto second_read = deferred_fixture::read_on(*second, 0, second_destination, stream);
  auto first_read  = fixture.read(chunk_size, first_destination, stream);

  // The requested batch reaches the backend before the waiter enters the sibling's blocked issue.
  auto& waiting = scope.get_async(std::move(first_read));
  auto* request = scope.take_next();
  REQUIRE(request != nullptr);
  REQUIRE(request->front().offset() == chunk_size);
  CHECK(complete_success(*request) == 1);
  // The waiter is inside the second file's issue(), holding that batch's mutex.
  REQUIRE(fixture.await_flushes(2));
  auto& destroying = scope.run_async([&second] { second.reset(); });
  CHECK(waiting.wait_for(100ms) == std::future_status::timeout);
  CHECK(destroying.wait_for(100ms) == std::future_status::timeout);
  CHECK(fixture.context->reactor().pending() == 0);

  complete_success(*prefetch, 0x6c);
  REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
  REQUIRE(destroying.wait_for(2s) == std::future_status::ready);
  CHECK(waiting.get() == read_size);
  CHECK(second_read.get() == read_size);
  CHECK(fixture.flushes() == 2);
  auto const bytes = download(second_destination, stream);
  CHECK(std::ranges::all_of(bytes, [](auto byte) { return byte == 0x6c; }));
}

TEST_CASE(
  "after the cache is shut down an open batch is issued to the backend and siblings are left alone",
  "[cache][deferred_device_reads][cross_source][gpu_execution]")
{
  deferred_fixture fixture;
  auto second = fixture.open_second();

  rmm::cuda_stream stream;
  rmm::device_buffer first_destination{read_size, stream};
  rmm::device_buffer second_destination{read_size, stream};
  deferred_scope scope{fixture};
  auto first_read  = fixture.read(0, first_destination, stream);
  auto second_read = deferred_fixture::read_on(*second, chunk_size, second_destination, stream);

  // The registry expired with the cache, so the waiter issues only its own batch, and the batch
  // goes straight to the backend. The flush counter went with the cache, so it is not checked.
  fixture.context->shutdown_cache();
  REQUIRE(fixture.context->cache() == nullptr);

  auto& waiting_first = scope.get_async(std::move(first_read));
  auto* first_request = scope.take_next();
  REQUIRE(first_request != nullptr);
  CHECK(scope.take_next(100ms) == nullptr);
  CHECK(complete_success(*first_request) == 1);
  REQUIRE(waiting_first.wait_for(2s) == std::future_status::ready);
  CHECK(waiting_first.get() == read_size);

  auto& waiting_second = scope.get_async(std::move(second_read));
  auto* second_request = scope.take_next();
  REQUIRE(second_request != nullptr);
  CHECK(complete_success(*second_request) == 1);
  REQUIRE(waiting_second.wait_for(2s) == std::future_status::ready);
  CHECK(waiting_second.get() == read_size);
}

TEST_CASE("failed deferred submissions never publish their destination",
          "[.][cache][deferred_host_allocation_fault][host_allocation_fault][gpu_execution]")
{
  using fault_guard = sirius::test::scoped_host_allocation_fault;
  REQUIRE(fault_guard::available());
  auto const new_batch    = GENERATE(false, true);
  std::size_t allocations = 0;
  for (std::size_t ordinal = 0; ordinal <= allocations; ++ordinal) {
    CAPTURE(new_batch, ordinal);
    deferred_fixture fixture;
    auto second_source = fixture.open_second();
    rmm::cuda_stream stream;
    rmm::device_buffer retained_destination{read_size, stream};
    rmm::device_buffer candidate_destination{read_size, stream};
    deferred_scope scope{fixture};
    auto retained = fixture.read(0, retained_destination, stream);
    std::future<std::size_t> candidate;
    bool rejected        = false;
    bool fired           = false;
    std::size_t observed = 0;
    {
      fault_guard fault{fault_guard::every_allocation_scope, ordinal};
      try {
        auto& source = new_batch ? *second_source : *fixture.datasource;
        candidate    = deferred_fixture::read_on(source, chunk_size, candidate_destination, stream);
      } catch (std::bad_alloc const&) {
        rejected = true;
      }
      observed = fault.stop();
      fired    = fault.fired();
    }
    if (ordinal == 0) {
      allocations = observed;
      REQUIRE(allocations > 0);
    }
    CHECK(fired == (ordinal != 0));
    CHECK(rejected == fired);
    auto& waiting             = scope.get_async(std::move(retained));
    std::size_t issued_slices = 0;
    auto* request             = scope.take_next();
    REQUIRE(request != nullptr);
    issued_slices += complete_success(*request);
    if (new_batch && !rejected) {
      request = scope.take_next();
      REQUIRE(request != nullptr);
      issued_slices += complete_success(*request);
    }
    REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
    CHECK(waiting.get() == read_size);
    if (candidate.valid()) { CHECK(candidate.get() == read_size); }
    CHECK(issued_slices == (rejected ? 1 : 2));
    CHECK(fixture.context->reactor().pending() == 0);
  }
}

TEST_CASE("datasource cancellation requires no allocation under sustained host OOM",
          "[.][cache][deferred_host_allocation_fault][host_allocation_fault][gpu_execution]")
{
  using fault_guard = sirius::test::scoped_host_allocation_fault;
  REQUIRE(fault_guard::available());
  deferred_fixture fixture;
  rmm::cuda_stream stream;
  rmm::device_buffer first_destination{read_size, stream};
  rmm::device_buffer second_destination{read_size, stream};
  auto first              = fixture.read(0, first_destination, stream);
  auto second             = fixture.read(chunk_size, second_destination, stream);
  std::size_t allocations = 0;
  {
    fault_guard fault{fault_guard::sustained_allocation_scope, 1};
    fixture.datasource.reset();
    allocations = fault.stop();
  }
  CHECK(allocations == 0);
  CHECK_THROWS_AS(first.get(), std::logic_error);
  CHECK_THROWS_AS(second.get(), std::logic_error);
  CHECK(fixture.flushes() == 0);
  CHECK(fixture.context->reactor().pending() == 0);
}

TEST_CASE("issue failure under sustained host OOM is terminal for every member",
          "[.][cache][deferred_host_allocation_fault][host_allocation_fault][gpu_execution]")
{
  using fault_guard = sirius::test::scoped_host_allocation_fault;
  REQUIRE(fault_guard::available());
  deferred_fixture fixture;
  rmm::cuda_stream stream;
  rmm::device_buffer first_destination{read_size, stream};
  rmm::device_buffer second_destination{read_size, stream};
  auto first  = fixture.read(0, first_destination, stream);
  auto second = fixture.read(chunk_size, second_destination, stream);
  std::array<bool, 2> failed{};
  std::size_t first_allocations  = 0;
  std::size_t second_allocations = 0;
  {
    fault_guard fault{fault_guard::sustained_allocation_scope, 1};
    try {
      (void)first.get();
    } catch (std::bad_alloc const&) {
      failed[0] = true;
    }
    first_allocations = fault.stop();
  }
  auto const first_flushes = fixture.flushes();
  {
    fault_guard fault{fault_guard::sustained_allocation_scope, 1};
    try {
      (void)second.get();
    } catch (std::bad_alloc const&) {
      failed[1] = true;
    }
    second_allocations = fault.stop();
  }
  CHECK(failed[0]);
  CHECK(failed[1]);
  CHECK(first_allocations >= 1);
  CHECK(second_allocations == 0);
  CHECK(fixture.flushes() == first_flushes);
  CHECK(fixture.context->reactor().pending() == 0);
}

TEST_CASE(
  "a requested batch with a terminal issue failure still issues its live sibling",
  "[.][cache][deferred_host_allocation_fault][host_allocation_fault][cross_source][gpu_execution]")
{
  using fault_guard = sirius::test::scoped_host_allocation_fault;
  REQUIRE(fault_guard::available());
  deferred_fixture fixture;
  auto second = fixture.open_second();
  rmm::cuda_stream stream;
  rmm::device_buffer first_destination{read_size, stream};
  rmm::device_buffer second_destination{read_size, stream};
  deferred_scope scope{fixture};
  auto const registry = fixture.context->cache()->deferred_batches();
  auto first          = std::make_shared<sirius::io::deferred_device_batch>(
    *fixture.datasource, stream.value(), rmm::get_current_cuda_device().value(), registry);
  REQUIRE(first->try_add(
    sirius::io::slice{0, read_size, static_cast<std::uint8_t*>(first_destination.data())}));
  registry->add(first);
  std::size_t allocations = 0;
  {
    fault_guard fault{fault_guard::sustained_allocation_scope, 1};
    first->issue_if_open();
    allocations = fault.stop();
  }
  REQUIRE(allocations >= 1);
  REQUIRE_FALSE(first->is_open());
  REQUIRE(fixture.context->reactor().pending() == 0);
  auto const first_flushes = fixture.flushes();
  auto second_read = deferred_fixture::read_on(*second, chunk_size, second_destination, stream);

  auto& waiting = scope.run_async([first] { return first->await(read_size); });
  auto* request = scope.take_next();
  REQUIRE(request != nullptr);
  CHECK(request->front().offset() == chunk_size);
  REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
  CHECK_THROWS_AS(waiting.get(), std::bad_alloc);
  CHECK(complete_success(*request) == 1);
  CHECK(second_read.get() == read_size);
  CHECK(fixture.flushes() == first_flushes + 1);
  CHECK(fixture.context->reactor().pending() == 0);
}
