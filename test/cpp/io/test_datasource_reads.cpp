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

#include "catch.hpp"
#include "io/cache/config.hpp"
#include "io/cache/controlled_backend.hpp"
#include "io/cache/prefetching_cache.hpp"
#include "io/io_request.hpp"
#include "io/sirius_datasource.hpp"
#include "scan/test_utils.hpp"
#include "utils/host_allocation_fault.hpp"

#include <rmm/cuda_device.hpp>
#include <rmm/cuda_stream.hpp>
#include <rmm/device_buffer.hpp>

#include <cuda/stream>
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
struct datasource_fixture {
  datasource_fixture()
    : memory(initialize_memory_manager(1)), context(std::make_shared<controlled_context>(1, [] {
        return std::make_unique<controlled_reactor>();
      }))
  {
    sirius::io::cache::config config;
    config.mode = sirius::io::cache::cache_mode::sirius;
    config.apply_mode();
    context->initialize_cache(*memory, config, single_gpu_topology());
    REQUIRE(context->cache() != nullptr);
    datasource = context->open_datasource("controlled://datasource-reads");
  }

  ~datasource_fixture()
  {
    datasource.reset();
    context->shutdown_cache();
  }

  [[nodiscard]] std::future<std::size_t> read(std::size_t offset,
                                              rmm::device_buffer& destination,
                                              ::cuda::stream_ref stream)
  {
    return read_on(*datasource, offset, destination, stream);
  }

  [[nodiscard]] static std::future<std::size_t> read_on(sirius::io::sirius_datasource& source,
                                                        std::size_t offset,
                                                        rmm::device_buffer& destination,
                                                        ::cuda::stream_ref stream)
  {
    return source.device_read_async(
      offset, destination.size(), static_cast<std::uint8_t*>(destination.data()), stream);
  }

  /// A second datasource on the same cache: a different file, as the files of one multi-file split.
  [[nodiscard]] std::unique_ptr<sirius::io::sirius_datasource> open_second()
  {
    return context->open_datasource("controlled://datasource-reads-second");
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
    REQUIRE(source.prefetch_async([](sirius::io::cache::prefetch_result) noexcept {}) ==
            sirius::io::prefetch_refusal::issued);
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
class datasource_scope {
 public:
  explicit datasource_scope(datasource_fixture& fixture) noexcept : _fixture{fixture} {}

  datasource_scope(datasource_scope const&)            = delete;
  datasource_scope& operator=(datasource_scope const&) = delete;
  datasource_scope(datasource_scope&&)                 = delete;
  datasource_scope& operator=(datasource_scope&&)      = delete;

  ~datasource_scope()
  {
    auto const error = std::make_exception_ptr(std::runtime_error("datasource_scope teardown"));
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

  datasource_fixture& _fixture;
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

TEST_CASE("degenerate datasource device reads settle immediately without I/O",
          "[cache][datasource_reads][gpu_execution]")
{
  datasource_fixture fixture;
  auto const file_size = fixture.datasource->size();

  rmm::cuda_stream stream;
  rmm::device_buffer destination{read_size, stream};
  datasource_scope scope{fixture};
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

  // A subsequent valid read still reaches the backend.
  auto& waiting = scope.get_async(fixture.read(0, destination, stream));
  auto* request = scope.take_next();
  REQUIRE(request != nullptr);
  CHECK(complete_success(*request) == 1);
  REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
  CHECK(waiting.get() == read_size);
}

TEST_CASE("a datasource cache hit copies the requested device bytes",
          "[cache][datasource_reads][gpu_execution]")
{
  datasource_fixture fixture;
  auto prefetch = fixture.start_prefetch(0, read_size);
  complete_success(*prefetch, 0x6c);

  rmm::cuda_stream stream;
  rmm::device_buffer destination{read_size, stream};
  datasource_scope scope{fixture};
  auto read = fixture.read(0, destination, stream);
  CHECK(read.wait_for(0ms) != std::future_status::deferred);

  // A resident read requires no backend request.
  auto& waiting = scope.get_async(std::move(read));
  REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
  CHECK(waiting.get() == read_size);
  CHECK(fixture.context->reactor().pending() == 0);
  auto const host = download(destination, stream);
  CHECK(std::ranges::all_of(host, [](auto byte) { return byte == 0x6c; }));
}

TEST_CASE("synchronous datasource device_read submits and returns the byte count",
          "[cache][datasource_reads][gpu_execution]")
{
  datasource_fixture fixture;

  rmm::cuda_stream stream;
  rmm::device_buffer destination{read_size, stream};
  datasource_scope scope{fixture};
  auto const device = rmm::get_current_cuda_device();
  auto& call        = scope.run_async([&, device] {
    rmm::cuda_set_device_raii guard{device};
    return fixture.datasource->device_read(
      0, read_size, static_cast<std::uint8_t*>(destination.data()), stream);
  });
  auto* request     = scope.take_next();
  REQUIRE(request != nullptr);
  CHECK(complete_success(*request) == 1);
  REQUIRE(call.wait_for(2s) == std::future_status::ready);
  CHECK(call.get() == read_size);
}

TEST_CASE("datasource device reads issue before timed waits",
          "[cache][datasource_reads][gpu_execution]")
{
  datasource_fixture fixture;

  rmm::cuda_stream stream;
  rmm::device_buffer destination{read_size, stream};
  datasource_scope scope{fixture};
  auto read     = fixture.read(0, destination, stream);
  auto* request = scope.take_next();
  REQUIRE(request != nullptr);
  CHECK(read.wait_for(0ms) == std::future_status::timeout);
  CHECK(read.wait_until(std::chrono::steady_clock::now()) == std::future_status::timeout);

  CHECK(complete_success(*request) == 1);
  REQUIRE(read.wait_for(2s) == std::future_status::ready);
  CHECK(read.wait_until(std::chrono::steady_clock::now()) == std::future_status::ready);
  CHECK(read.get() == read_size);
}

TEST_CASE("shared default stream device reads issue eagerly on the submitting thread",
          "[cache][datasource_reads][gpu_execution]")
{
  auto const shared = GENERATE(::cuda::stream_ref{cudaStream_t{}},
                               ::cuda::stream_ref{cudaStreamLegacy},
                               ::cuda::stream_ref{cudaStreamPerThread});
  datasource_fixture fixture;
  auto second = fixture.open_second();

  rmm::device_buffer first_destination{read_size, shared};
  rmm::device_buffer second_destination{read_size, shared};
  datasource_scope scope{fixture};
  auto first_read  = fixture.read(0, first_destination, shared);
  auto second_read = datasource_fixture::read_on(*second, chunk_size, second_destination, shared);

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
  shared.sync();
}

TEST_CASE("a per-thread default stream cache hit keeps its submitting stream",
          "[cache][datasource_reads][gpu_execution]")
{
  datasource_fixture fixture;
  auto prefetch = fixture.start_prefetch(0, read_size);
  complete_success(*prefetch, 0x7b);
  ::cuda::stream_ref const stream{cudaStreamPerThread};
  rmm::device_buffer destination{read_size, stream};
  datasource_scope scope{fixture};
  auto read = fixture.read(0, destination, stream);
  CHECK(read.wait_for(0ms) != std::future_status::deferred);
  auto& waiting = scope.get_async(std::move(read));
  REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
  CHECK(waiting.get() == read_size);
  std::vector<std::uint8_t> bytes(read_size);
  REQUIRE(cudaMemcpyAsync(
            bytes.data(), destination.data(), bytes.size(), cudaMemcpyDeviceToHost, stream.get()) ==
          cudaSuccess);
  stream.sync();
  CHECK(std::ranges::all_of(bytes, [](auto byte) { return byte == 0x7b; }));
}

TEST_CASE("datasource reads preserve the submitting GPU and stream across future migration",
          "[cache][datasource_reads][multi_gpu][gpu_execution]")
{
  int devices = 0;
  REQUIRE(cudaGetDeviceCount(&devices) == cudaSuccess);
  if (devices < 2) {
    WARN("requires two visible GPUs");
    return;
  }
  datasource_fixture fixture;
  rmm::cuda_set_device_raii submitting_device{rmm::cuda_device_id{1}};
  rmm::cuda_stream stream;
  rmm::device_buffer destination{read_size, stream};
  datasource_scope scope{fixture};
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

TEST_CASE("datasource device reads clamp each independent range at EOF",
          "[cache][datasource_reads][gpu_execution]")
{
  datasource_fixture fixture;
  rmm::cuda_stream stream;
  rmm::device_buffer whole{read_size, stream};
  rmm::device_buffer tail{read_size, stream};
  datasource_scope scope{fixture};
  auto whole_read = fixture.read(0, whole, stream);
  auto tail_read  = fixture.read(fixture.datasource->size() - 100, tail, stream);
  auto* first     = scope.take_next();
  auto* second    = scope.take_next();
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  CHECK(first->front().size() == read_size);
  CHECK(second->front().size() == 100);
  CHECK(complete_success(*second) == 1);
  CHECK(tail_read.get() == 100);
  CHECK(whole_read.wait_for(0ms) == std::future_status::timeout);
  CHECK(complete_success(*first) == 1);
  CHECK(whole_read.get() == read_size);
}

TEST_CASE("a datasource backend failure leaves an independent read intact",
          "[cache][datasource_reads][gpu_execution]")
{
  datasource_fixture fixture;
  rmm::cuda_stream first_stream;
  rmm::cuda_stream second_stream;
  rmm::device_buffer first_destination{read_size, first_stream};
  rmm::device_buffer second_destination{read_size, second_stream};
  datasource_scope scope{fixture};
  auto first_read  = fixture.read(0, first_destination, first_stream);
  auto second_read = fixture.read(chunk_size, second_destination, second_stream);
  auto* first      = scope.take_next();
  auto* second     = scope.take_next();
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  CHECK(first->front().d_buffer.stream.get() == first_stream.value());
  CHECK(second->front().d_buffer.stream.get() == second_stream.value());
  complete_failure(*first, std::make_exception_ptr(std::runtime_error("first failed")));
  CHECK_THROWS_WITH(first_read.get(), "first failed");
  CHECK(second_read.wait_until(std::chrono::steady_clock::now()) == std::future_status::timeout);
  CHECK(complete_success(*second) == 1);
  CHECK(second_read.get() == read_size);
}

TEST_CASE("datasource reads remain eager after cache shutdown",
          "[cache][datasource_reads][gpu_execution]")
{
  datasource_fixture fixture;
  fixture.context->shutdown_cache();
  REQUIRE_FALSE(fixture.datasource->uses_prefetching_cache());
  rmm::cuda_stream stream;
  rmm::device_buffer destination{read_size, stream};
  datasource_scope scope{fixture};
  auto read     = fixture.read(0, destination, stream);
  auto* request = scope.take_next();
  REQUIRE(request != nullptr);
  CHECK(read.wait_for(0ms) == std::future_status::timeout);
  CHECK(complete_success(*request) == 1);
  CHECK(read.get() == read_size);
}

TEST_CASE("destroying an eager datasource future does not cancel its submitted read",
          "[cache][datasource_reads][gpu_execution]")
{
  datasource_fixture fixture;
  rmm::cuda_stream stream;
  rmm::device_buffer destination{read_size, stream};
  datasource_scope scope{fixture};
  auto read     = fixture.read(0, destination, stream);
  auto* request = scope.take_next();
  REQUIRE(request != nullptr);
  read = {};
  CHECK(complete_success(*request) == 1);
  CHECK(fixture.context->reactor().pending() == 0);
}

namespace {

/**
 * Publishes a borrowed destination without allocating. The prebuilt completion isolates the
 * datasource's promise/callback preparation from allocations inside a backend fixture.
 */
class publication_context final : public sirius::io::templated_ioctx<controlled_reactor> {
 public:
  publication_context() : templated_ioctx{std::vector<std::unique_ptr<controlled_reactor>>{}} {}

  sirius::io::io_context_type type() const noexcept override
  {
    return sirius::io::io_context_type::uring;
  }

  sirius::exec::semi_future<std::size_t> host_read_async_io(
    sirius::io::io_object const&,
    std::size_t,
    std::size_t,
    std::uint8_t* destination) noexcept override
  {
    return publish(destination);
  }

  sirius::exec::semi_future<std::size_t> device_read_async_io(sirius::io::io_object const&,
                                                              std::size_t,
                                                              std::size_t,
                                                              std::uint8_t* destination,
                                                              ::cuda::stream_ref) noexcept override
  {
    return publish(destination);
  }

  sirius::exec::semi_future<std::size_t> host_readv_async_io(
    sirius::io::io_object const&, std::span<sirius::io::slice const> slices) noexcept override
  {
    return publish(slices.front().dst);
  }

  sirius::exec::semi_future<std::size_t> device_readv_async_io(
    sirius::io::io_object const&,
    std::span<sirius::io::slice const> slices,
    ::cuda::stream_ref) noexcept override
  {
    return publish(slices.front().dst);
  }

  void finish() { _promise.set_value(read_size); }
  std::uint8_t* destination{nullptr};

 private:
  sirius::exec::semi_future<std::size_t> publish(std::uint8_t* value) noexcept
  {
    destination = value;
    return std::move(_completion);
  }

  sirius::exec::promise<std::size_t> _promise;
  sirius::exec::semi_future<std::size_t> _completion{_promise.get_semi_future()};
};

}  // namespace

TEST_CASE("datasource future allocation failures never publish a borrowed destination",
          "[.][datasource_reads][datasource_host_allocation_fault][host_allocation_fault]")
{
  using fault_guard = sirius::test::scoped_host_allocation_fault;
  REQUIRE(fault_guard::available());
  auto const api          = GENERATE(0, 1, 2, 3);
  std::size_t allocations = 0;
  for (std::size_t ordinal = 0; ordinal <= allocations; ++ordinal) {
    CAPTURE(api, ordinal);
    auto context = std::make_shared<publication_context>();
    auto source  = context->open_datasource("controlled://publication");
    std::array<std::uint8_t, read_size> destination{};
    std::array<sirius::io::slice, 1> slices{sirius::io::slice{0, read_size, destination.data()}};
    std::future<std::size_t> read;
    bool rejected        = false;
    bool fired           = false;
    std::size_t observed = 0;
    {
      fault_guard fault{fault_guard::every_allocation_scope, ordinal};
      try {
        switch (api) {
          case 0: read = source->host_read_async(0, read_size, destination.data()); break;
          case 1:
            read = source->device_read_async(
              0, read_size, destination.data(), ::cuda::stream_ref{cudaStream_t{}});
            break;
          case 2: read = source->host_read_ranges_async(slices); break;
          case 3:
            read = source->device_read_ranges_async(slices, ::cuda::stream_ref{cudaStream_t{}});
            break;
        }
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
    CHECK((context->destination != nullptr) == !rejected);
    CHECK(read.valid() == !rejected);
    if (read.valid()) {
      CHECK(context->destination == destination.data());
      CHECK(read.wait_for(0ms) == std::future_status::timeout);
      context->finish();
      CHECK(read.get() == read_size);
    }
  }
}
