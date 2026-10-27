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
#include "io/cache/controlled_backend.hpp"
#include "io/io_request.hpp"
#include "io/sirius_datasource.hpp"
#include "scan/test_utils.hpp"
#include "utils/child_process_environment.hpp"

#include <rmm/cuda_stream.hpp>
#include <rmm/device_buffer.hpp>

#include <cuda/stream>
#include <cuda_runtime.h>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

// One logical read may expand into many physical operations across reactors.
// grouped_coordinator exists to make that fan-out look like one future while
// stopping new dispatch on the first error and still draining published work.
// The tests below exercise those ordering and ownership requirements directly.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <memory>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
// All barriers are released before joining, including on assertion failure.
struct failed_copy_publication {
  rmm::cuda_stream stream;
  rmm::device_buffer destination{64, stream};
  std::uint8_t* source{};
  std::binary_semaphore copy_held{0}, release_copy{0}, drain_entered{0}, release_drain{0};
  std::binary_semaphore callback_installed{0}, callback_entered{0}, release_callback{0};
  std::future<cudaError_t> result;
  cudaStreamCallback_t completion{};
  void* completion_data{};
  cudaError_t callback_status{cudaSuccess};
  bool fail_sync{false};
  bool copy_released{false}, drain_released{false}, callback_released{false};
  bool gate_submitted{false};
  inline static thread_local failed_copy_publication* current{};

  failed_copy_publication()
  {
    auto const status = cudaMallocHost(&source, 64);
    if (status != cudaSuccess) { throw std::runtime_error(cudaGetErrorString(status)); }
  }

  ~failed_copy_publication()
  {
    release();
    if (result.valid()) { result.wait(); }
    if (gate_submitted) { stream.synchronize_no_throw(); }
    cudaFreeHost(source);
  }

  void release() noexcept
  {
    allow_copy();
    allow_drain();
    allow_callback();
  }

  void allow_copy() noexcept
  {
    if (!copy_released) {
      copy_released = true;
      release_copy.release();
    }
  }

  void allow_drain() noexcept
  {
    if (!drain_released) {
      drain_released = true;
      release_drain.release();
    }
  }

  void allow_callback() noexcept
  {
    if (!callback_released) {
      callback_released = true;
      release_callback.release();
    }
  }

  static cudaError_t record(cudaEvent_t, cudaStream_t) { return cudaErrorInvalidResourceHandle; }

  static cudaError_t synchronize(cudaStream_t stream)
  {
    current->drain_entered.release();
    current->release_drain.acquire();
    if (current->fail_sync) { return cudaErrorLaunchFailure; }
    return cudaStreamSynchronize(stream);
  }

  static cudaError_t add_callback(cudaStream_t stream,
                                  cudaStreamCallback_t callback,
                                  void* data,
                                  unsigned flags)
  {
    auto& self           = *current;
    self.completion      = callback;
    self.completion_data = data;
    auto const status    = cudaStreamAddCallback(
      stream,
      [](cudaStream_t callback_stream, cudaError_t, void* opaque) {
        auto& operation = *static_cast<failed_copy_publication*>(opaque);
        operation.callback_entered.release();
        operation.release_callback.acquire();
        operation.completion(callback_stream, operation.callback_status, operation.completion_data);
      },
      &self,
      flags);
    self.callback_installed.release();
    return status;
  }

  void start(bool publication_failure = true)
  {
    std::fill_n(source, 64, std::uint8_t{0x5a});
    auto const status = cudaLaunchHostFunc(
      stream.value(),
      [](void* opaque) {
        auto& self = *static_cast<failed_copy_publication*>(opaque);
        self.copy_held.release();
        self.release_copy.acquire();
      },
      this);
    if (status != cudaSuccess) { throw std::runtime_error(cudaGetErrorString(status)); }
    gate_submitted = true;
    int device{};
    if (cudaGetDevice(&device) != cudaSuccess) { throw std::runtime_error("missing CUDA device"); }
    result = std::async(std::launch::async, [this, device, publication_failure] {
      current = this;
      sirius::io::device_cpy_request request{
        sirius::io::range{0, 64},
        sirius::io::device_buffer{static_cast<std::uint8_t*>(destination.data()), stream, device},
        device};
      iovec input{source, 64};
      auto const status =
        request.copy_async(request.req_rng,
                           std::span{&input, 1},
                           publication_failure ? reinterpret_cast<cudaEvent_t>(this) : nullptr,
                           {record, synchronize, add_callback});
      if (publication_failure || status != cudaSuccess) { return status; }
      return sirius::cuda::drain_copy_stream_or_terminate(
        stream, device, {record, synchronize, add_callback});
    });
  }
};
}  // namespace

TEST_CASE("copy publication failure cannot return before its accepted writes drain",
          "[io][copy_completion][gpu_execution]")
{
  using namespace std::chrono_literals;
  failed_copy_publication operation;
  operation.start();
  REQUIRE(operation.copy_held.try_acquire_for(5s));
  REQUIRE(operation.drain_entered.try_acquire_for(5s));
  CHECK(operation.result.wait_for(0ms) == std::future_status::timeout);
  operation.release();
  REQUIRE(operation.result.wait_for(5s) == std::future_status::ready);
  CHECK(operation.result.get() == cudaErrorInvalidResourceHandle);
  std::array<std::uint8_t, 64> output{};
  REQUIRE(cudaMemcpy(
            output.data(), operation.destination.data(), output.size(), cudaMemcpyDeviceToHost) ==
          cudaSuccess);
  CHECK(std::ranges::all_of(output, [](auto byte) { return byte == 0x5a; }));
}

TEST_CASE("failed copy synchronization retains buffers until its accepted callback",
          "[io][copy_completion][callback_recovery][gpu_execution]")
{
  using namespace std::chrono_literals;
  for (auto const callback_status : {cudaSuccess, cudaErrorLaunchFailure}) {
    for (bool const publication_failure : {true, false}) {
      DYNAMIC_SECTION("callback=" << callback_status << ", publication=" << publication_failure)
      {
        failed_copy_publication operation;
        operation.fail_sync       = true;
        operation.callback_status = callback_status;
        operation.start(publication_failure);
        REQUIRE(operation.copy_held.try_acquire_for(5s));
        REQUIRE(operation.drain_entered.try_acquire_for(5s));
        operation.allow_drain();
        REQUIRE(operation.callback_installed.try_acquire_for(5s));
        CHECK(operation.result.wait_for(50ms) == std::future_status::timeout);
        operation.allow_copy();
        REQUIRE(operation.callback_entered.try_acquire_for(5s));
        CHECK(operation.result.wait_for(50ms) == std::future_status::timeout);
        operation.allow_callback();
        REQUIRE(operation.result.wait_for(5s) == std::future_status::ready);
        CHECK(operation.result.get() ==
              (publication_failure ? cudaErrorInvalidResourceHandle : cudaErrorLaunchFailure));
        std::array<std::uint8_t, 64> output{};
        REQUIRE(cudaMemcpy(output.data(),
                           operation.destination.data(),
                           output.size(),
                           cudaMemcpyDeviceToHost) == cudaSuccess);
        CHECK(std::ranges::all_of(output, [](auto byte) { return byte == 0x5a; }));
      }
    }
  }
}

TEST_CASE("copy recovery without a completion boundary child",
          "[.][copy_completion_terminal_child]")
{
  rmm::cuda_stream stream;
  int device{};
  REQUIRE(cudaGetDevice(&device) == cudaSuccess);
  static unsigned failed_proofs{};
  sirius::cuda::detail::copy_completion_api api;
  api.synchronize = [](cudaStream_t) {
    failed_proofs |= 1;
    return cudaErrorLaunchFailure;
  };
  api.add_callback = [](cudaStream_t, cudaStreamCallback_t, void*, unsigned) {
    failed_proofs |= 2;
    return cudaErrorNotSupported;
  };
  std::set_terminate([] { std::_Exit(failed_proofs == 3 ? 86 : 85); });
  static_cast<void>(sirius::cuda::drain_copy_stream_or_terminate(stream, device, api));
  std::_Exit(87);
}

TEST_CASE("copy recovery cannot return when synchronization and callback both fail",
          "[io][copy_completion][callback_recovery][gpu_execution]")
{
  using namespace std::chrono_literals;
  struct child_process {
    pid_t pid{-1};
    ~child_process()
    {
      if (pid <= 0) { return; }
      static_cast<void>(::kill(pid, SIGKILL));
      int status{};
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
  } child;
  char executable[] = "sirius_unittest";
  char filter[]     = "copy recovery without a completion boundary child";
  std::array<char*, 3> arguments{executable, filter, nullptr};
  sirius::test::child_process_environment environment{{}};
  REQUIRE(::posix_spawn(
            &child.pid, "/proc/self/exe", nullptr, nullptr, arguments.data(), environment.data()) ==
          0);
  auto const deadline = std::chrono::steady_clock::now() + 30s;
  while (std::chrono::steady_clock::now() < deadline) {
    int status{};
    auto const waited = ::waitpid(child.pid, &status, WNOHANG);
    if (waited == child.pid) {
      child.pid = -1;
      REQUIRE(WIFEXITED(status));
      CHECK(WEXITSTATUS(status) == 86);
      return;
    }
    REQUIRE((waited == 0 || (waited < 0 && errno == EINTR)));
    std::this_thread::sleep_for(20ms);
  }
  FAIL("copy recovery child exceeded its watchdog");
}

namespace {
class failed_cache_completion_context final
  : public sirius::io::templated_ioctx<sirius::test::controlled::controlled_reactor> {
 public:
  failed_cache_completion_context()
    : templated_ioctx{
        1, [] { return std::make_unique<sirius::test::controlled::controlled_reactor>(); }}
  {
  }

  ~failed_cache_completion_context() override { pre_destroy(); }

  sirius::io::io_context_type type() const noexcept override
  {
    return sirius::io::io_context_type::uring;
  }

  void initialize_failed_cache(cucascade::memory::memory_reservation_manager& memory)
  {
    sirius::exec::detail::completion_cuda_api api;
    api.add_callback = [](cudaStream_t, cudaStreamCallback_t, void*, unsigned) {
      return cudaErrorNotSupported;
    };
    initialize_cache_with_api(memory, api);
  }

  void initialize_cache_with_api(cucascade::memory::memory_reservation_manager& memory,
                                 sirius::exec::detail::completion_cuda_api api)
  {
    sirius::io::cache::config config;
    config.mode = sirius::io::cache::cache_mode::sirius;
    config.apply_mode();
    _cache = std::make_unique<sirius::io::cache::prefetching_cache>(
      memory, this, config, sirius::test::controlled::single_gpu_topology(), api);
  }

  auto& reactor() { return *_reactors.front(); }
};

struct parked_read {
  std::unique_ptr<sirius::io::grouped_io_request> request;
  std::exception_ptr teardown_error{
    std::make_exception_ptr(std::runtime_error("parked read teardown"))};
  ~parked_read()
  {
    if (request) { sirius::test::controlled::complete_failure(*request, teardown_error); }
  }
};

struct cache_completion_fault {
  cudaStream_t stream{};
  std::atomic<bool> reject_callback{false}, fail_query{false}, fail_sync{false};
  std::atomic<int> callback_attempts{0}, failed_synchronizations{0};
  std::binary_semaphore callback_entered{0}, synchronize_entered{0}, release_callback{0};
  cudaStreamCallback_t callback{};
  void* callback_data{};
  bool hold_callback{false}, released{false};
  inline static cache_completion_fault* current{};

  cache_completion_fault() { current = this; }
  ~cache_completion_fault() { current = nullptr; }

  void release() noexcept
  {
    fail_sync.store(false);
    fail_query.store(false);
    if (!released) {
      released = true;
      release_callback.release();
    }
  }

  static cudaError_t add(cudaStream_t stream,
                         cudaStreamCallback_t callback,
                         void* data,
                         unsigned flags)
  {
    auto& self = *current;
    if (stream != self.stream) { return cudaStreamAddCallback(stream, callback, data, flags); }
    ++self.callback_attempts;
    if (self.reject_callback.exchange(false)) { return cudaErrorNotSupported; }
    if (!self.hold_callback) { return cudaStreamAddCallback(stream, callback, data, flags); }
    self.callback      = callback;
    self.callback_data = data;
    return cudaStreamAddCallback(
      stream,
      [](cudaStream_t callback_stream, cudaError_t status, void* opaque) {
        auto& fault = *static_cast<cache_completion_fault*>(opaque);
        fault.callback_entered.release();
        fault.release_callback.acquire();
        fault.callback(callback_stream, status, fault.callback_data);
      },
      &self,
      flags);
  }

  static cudaError_t query(cudaStream_t stream)
  {
    auto& self = *current;
    if (stream == self.stream && self.fail_query.exchange(false)) {
      return cudaErrorInvalidResourceHandle;
    }
    return cudaStreamQuery(stream);
  }

  static cudaError_t synchronize(cudaStream_t stream)
  {
    auto& self = *current;
    if (stream == self.stream && self.fail_sync.load()) {
      if (self.failed_synchronizations.fetch_add(1) == 0) { self.synchronize_entered.release(); }
      return cudaErrorLaunchFailure;
    }
    return cudaStreamSynchronize(stream);
  }
};

struct release_cache_callback {
  cache_completion_fault& fault;
  ::cuda::stream_ref stream, other_stream;
  ~release_cache_callback()
  {
    fault.release();
    static_cast<void>(cudaStreamSynchronize(stream.get()));
    static_cast<void>(cudaStreamSynchronize(other_stream.get()));
  }
};
}  // namespace

TEST_CASE("cached-copy failure cannot settle a file with a pending backend writer",
          "[io][copy_completion][read-arbitration][gpu_execution]")
{
  using namespace std::chrono_literals;
  using namespace sirius::test::controlled;
  auto memory  = initialize_memory_manager(1);
  auto context = std::make_shared<failed_cache_completion_context>();
  context->initialize_failed_cache(*memory);
  auto source = context->open_datasource("controlled://failed-cache-copy");
  std::array<cudf::io::text::byte_range_info, 1> ranges{{{0, 64}}};
  source->fadvise(ranges, 0);
  REQUIRE(source->prepare_prefetch(false) == sirius::io::prepare_result::prepared);
  REQUIRE(source->prefetch_async([](sirius::io::cache::prefetch_result) noexcept {}) ==
          sirius::io::prefetch_refusal::issued);
  parked_read prefetch{context->reactor().take_next()};
  REQUIRE(prefetch.request);
  complete_success(*prefetch.request);

  rmm::cuda_stream stream;
  rmm::device_buffer cached{64, stream}, backend{64, stream};
  std::array<sirius::io::slice, 2> slices{
    sirius::io::slice{0, 64, static_cast<std::uint8_t*>(cached.data())},
    sirius::io::slice{2 * chunk_size, 64, static_cast<std::uint8_t*>(backend.data())}};
  auto read = source->device_read_ranges_async(slices, stream);
  parked_read held{context->reactor().take_next()};
  REQUIRE(held.request);
  // The callback installation has failed and its stream drained. The backend
  // credit still owns a destination, so even the failed file must stay pending.
  CHECK(read.wait_for(100ms) == std::future_status::timeout);
  complete_success(*held.request);
  REQUIRE(read.wait_for(5s) == std::future_status::ready);
  try {
    static_cast<void>(read.get());
    FAIL("cached-copy failure was lost");
  } catch (std::runtime_error const& error) {
    CHECK(std::string_view{error.what()}.find("operation not supported") != std::string_view::npos);
  }
}

TEST_CASE("a cached read recovers on the same stream after its completion lane closes",
          "[io][copy_completion][callback_recovery][read-arbitration][gpu_execution]")
{
  using namespace std::chrono_literals;
  using namespace sirius::test::controlled;
  for (bool const publication_failure : {true, false}) {
    DYNAMIC_SECTION("publication failure=" << publication_failure)
    {
      auto memory = initialize_memory_manager(1);
      rmm::cuda_stream stream, other_stream;
      cache_completion_fault fault;
      fault.stream          = stream.value();
      fault.reject_callback = publication_failure;
      fault.fail_query      = !publication_failure;
      fault.fail_sync       = !publication_failure;
      fault.hold_callback   = !publication_failure;
      auto context          = std::make_shared<failed_cache_completion_context>();
      context->initialize_cache_with_api(*memory,
                                         {cache_completion_fault::add,
                                          cache_completion_fault::query,
                                          cache_completion_fault::synchronize});
      auto source = context->open_datasource("controlled://recovered-cache-copy");
      std::array<cudf::io::text::byte_range_info, 1> ranges{{{0, 64}}};
      source->fadvise(ranges, 0);
      REQUIRE(source->prepare_prefetch(false) == sirius::io::prepare_result::prepared);
      REQUIRE(source->prefetch_async([](sirius::io::cache::prefetch_result) noexcept {}) ==
              sirius::io::prefetch_refusal::issued);
      parked_read prefetch{context->reactor().take_next()};
      REQUIRE(prefetch.request);
      REQUIRE_FALSE(prefetch.request->slices.empty());
      auto const chunks = prefetch.request->slices.front().h_buffer.fragments();
      REQUIRE(chunks.size() == 1);
      auto* const chunk = chunks.front();
      complete_success(*prefetch.request, 0x6c);

      rmm::device_buffer first_destination{64, stream}, next_destination{64, stream};
      rmm::device_buffer other_destination{64, other_stream};
      release_cache_callback cleanup{fault, stream, other_stream};
      auto first = source->device_read_async(
        0, 64, static_cast<std::uint8_t*>(first_destination.data()), stream);
      if (!publication_failure) {
        REQUIRE(fault.callback_entered.try_acquire_for(5s));
        REQUIRE(fault.synchronize_entered.try_acquire_for(5s));
        CHECK(first.wait_for(50ms) == std::future_status::timeout);
        CHECK(chunk->state.get_pin_count() == 1);
        fault.release();
      }
      REQUIRE(first.wait_for(5s) == std::future_status::ready);
      try {
        static_cast<void>(first.get());
        FAIL("cached-copy failure was lost");
      } catch (std::runtime_error const& error) {
        auto const expected =
          publication_failure ? cudaErrorNotSupported : cudaErrorInvalidResourceHandle;
        CHECK(std::string_view{error.what()}.find(cudaGetErrorString(expected)) !=
              std::string_view::npos);
      }
      CHECK(chunk->state.get_pin_count() == 0);
      CHECK(fault.callback_attempts.load() == 1);
      CHECK(fault.failed_synchronizations.load() == (publication_failure ? 0 : 1));

      auto next = source->device_read_async(
        0, 64, static_cast<std::uint8_t*>(next_destination.data()), stream);
      REQUIRE(next.wait_for(5s) == std::future_status::ready);
      CHECK(next.get() == 64);
      CHECK(fault.callback_attempts.load() == 1);
      CHECK(chunk->state.get_pin_count() == 0);

      auto other = source->device_read_async(
        0, 64, static_cast<std::uint8_t*>(other_destination.data()), other_stream);
      REQUIRE(other.wait_for(5s) == std::future_status::ready);
      CHECK(other.get() == 64);
      CHECK(chunk->state.get_pin_count() == 0);
      CHECK(context->reactor().pending() == 0);
      for (auto const* destination : {&first_destination, &next_destination, &other_destination}) {
        std::array<std::uint8_t, 64> output{};
        REQUIRE(
          cudaMemcpy(output.data(), destination->data(), output.size(), cudaMemcpyDeviceToHost) ==
          cudaSuccess);
        CHECK(std::ranges::all_of(output, [](auto byte) { return byte == 0x6c; }));
      }
    }
  }
}

using sirius::io::grouped_coordinator;
using sirius::io::io_op_request;
using sirius::io::prepared_io_completion;

TEST_CASE("an empty grouped coordinator is ready with zero bytes", "[io][coordinator]")
{
  grouped_coordinator coordinator{0, 0};
  auto future = coordinator.get_future();

  CHECK(future.is_ready());
  CHECK(std::move(future).get() == 0);
}

TEST_CASE("slice expansion adds credits before children complete", "[io][coordinator]")
{
  grouped_coordinator coordinator{4096, 1};
  auto future = coordinator.get_future();

  coordinator.add_tasks(3);
  CHECK(coordinator.tasks_remaining() == 4);

  std::vector<std::thread> completions;
  completions.reserve(4);
  for (int i = 0; i < 4; ++i) {
    completions.emplace_back([&] { coordinator.on_complete(); });
  }
  for (auto& completion : completions) {
    completion.join();
  }

  CHECK(future.is_ready());
  CHECK(std::move(future).get() == 4096);
}

TEST_CASE("the first error stops dispatch but fulfillment waits for drain", "[io][coordinator]")
{
  grouped_coordinator coordinator{128, 2};
  auto future = coordinator.get_future();
  auto error  = std::make_exception_ptr(std::runtime_error("first physical read failed"));

  coordinator.report_error(error);

  CHECK_FALSE(coordinator.should_continue());
  CHECK(coordinator.has_error());
  CHECK(coordinator.tasks_remaining() == 1);
  CHECK_FALSE(future.is_ready());

  coordinator.on_complete();

  CHECK(future.is_ready());
  CHECK_THROWS_WITH(std::move(future).get(), "first physical read failed");
}

TEST_CASE("physical completion publishes cache state before the future", "[io][coordinator]")
{
  auto coordinator = std::make_shared<grouped_coordinator>(64, 1);
  auto future      = coordinator->get_future();
  std::atomic<bool> callback_ran{false};

  io_op_request operation;
  operation.coordinator = coordinator;
  operation.on_complete = std::make_shared<prepared_io_completion>(
    [&callback_ran](std::span<sirius::io::cache::cached_chunk* const>, bool success) noexcept {
      callback_ran.store(success, std::memory_order_release);
    });

  operation.finish_success();

  CHECK(callback_ran.load(std::memory_order_acquire));
  CHECK(future.is_ready());
  CHECK(std::move(future).get() == 64);
  CHECK(operation.terminal());
}

TEST_CASE("host-valid data is publishable even when its device copy fails", "[io][coordinator]")
{
  auto coordinator = std::make_shared<grouped_coordinator>(64, 1);
  auto future      = coordinator->get_future();
  std::atomic<bool> host_data_valid{false};

  io_op_request operation;
  operation.coordinator = coordinator;
  operation.on_complete = std::make_shared<prepared_io_completion>(
    [&host_data_valid](std::span<sirius::io::cache::cached_chunk* const>, bool success) noexcept {
      host_data_valid.store(success, std::memory_order_release);
    });

  operation.finish_error(std::make_exception_ptr(std::runtime_error("H2D event failed")), true);

  CHECK(host_data_valid.load(std::memory_order_acquire));
  CHECK(future.is_ready());
  CHECK_THROWS_WITH(std::move(future).get(), "H2D event failed");
}
