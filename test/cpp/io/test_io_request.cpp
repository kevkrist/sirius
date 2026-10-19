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

#include <rmm/cuda_stream.hpp>
#include <rmm/device_buffer.hpp>

// One logical read may expand into many physical operations across reactors.
// grouped_coordinator exists to make that fan-out look like one future while
// stopping new dispatch on the first error and still draining published work.
// The tests below exercise those ordering and ownership requirements directly.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
  std::future<cudaError_t> result;
  bool released{false};
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
    if (released) { return; }
    released = true;
    release_copy.release();
    release_drain.release();
  }

  static cudaError_t record(cudaEvent_t, cudaStream_t) { return cudaErrorInvalidResourceHandle; }

  static cudaError_t synchronize(cudaStream_t stream)
  {
    current->drain_entered.release();
    current->release_drain.acquire();
    return cudaStreamSynchronize(stream);
  }

  void start()
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
    result = std::async(std::launch::async, [this, device] {
      current = this;
      sirius::io::device_cpy_request request{
        sirius::io::range{0, 64},
        sirius::io::device_buffer{static_cast<std::uint8_t*>(destination.data()), stream, device},
        device};
      iovec input{source, 64};
      // The injected event boundary fails only after the real copy is accepted.
      return request.copy_async(request.req_rng,
                                std::span{&input, 1},
                                reinterpret_cast<cudaEvent_t>(this),
                                {record, synchronize});
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
    sirius::io::cache::config config;
    config.mode = sirius::io::cache::cache_mode::sirius;
    config.apply_mode();
    sirius::exec::detail::completion_cuda_api api;
    api.add_callback = [](cudaStream_t, cudaStreamCallback_t, void*, unsigned) {
      return cudaErrorNotSupported;
    };
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
  REQUIRE(source->prefetch_async([](bool) noexcept {}) == sirius::io::prefetch_refusal::issued);
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
