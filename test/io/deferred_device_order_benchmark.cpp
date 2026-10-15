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

#include "copy_symbols.hpp"
#include "io/cache/controlled_backend.hpp"
#include "io/sirius_datasource.hpp"
#include "memory/sirius_memory_reservation_manager.hpp"

#include <cudf/detail/utilities/cuda_memcpy.hpp>

#include <rmm/cuda_stream.hpp>
#include <rmm/device_buffer.hpp>

#include <cucascade/memory/reservation_manager_configurator.hpp>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using clock_type = std::chrono::steady_clock;
using namespace std::chrono_literals;
using namespace sirius::test::controlled;
constexpr std::size_t sources           = 3;
constexpr std::size_t slices_per_source = 4;

void check_cuda(cudaError_t status)
{
  if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
}

std::int64_t now_ns() noexcept
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch())
    .count();
}

struct observation {
  struct completion_token {
    observation* owner{};
    std::size_t source{};
    std::size_t bytes{};
  };
  std::array<std::uintptr_t, sources> destinations{};
  std::size_t bytes{};
  std::array<std::atomic<std::size_t>, sources> copied{};
  std::array<std::atomic<std::int64_t>, sources> ready{};
  std::array<completion_token, 256> tokens{};
  std::atomic<std::size_t> next_token{};
};
observation* active_observation{};

void copy_complete(void* opaque) noexcept
{
  auto const& token = *static_cast<observation::completion_token*>(opaque);
  auto& state       = *token.owner;
  if (state.copied[token.source].fetch_add(token.bytes) + token.bytes == state.bytes) {
    state.ready[token.source].store(now_ns());
  }
}

struct trace_event {
  std::string event;
  std::string source;
  bool prefetch;
  std::int64_t timestamp;
};

/**
 * @brief Finite request queue with real pread and H2D service on a fixed worker pool
 *
 * The controlled queue holds at most eight requests in this three-source workload; overflow fails
 * the process. The optional prefetch gate and service delay are injected latency, not storage
 * performance measurements.
 */
class benchmark_reactor : public controlled_reactor {
 public:
  benchmark_reactor(std::size_t workers,
                    std::chrono::microseconds service,
                    std::chrono::microseconds gate)
    : _worker_count(workers), _service(service), _gate(gate)
  {
  }
  ~benchmark_reactor() { shutdown(); }

  void enqueue(std::unique_ptr<sirius::io::grouped_io_request> request) noexcept
  {
    if (pending() >= 8) std::abort();
    record("enqueue", *request);
    controlled_reactor::enqueue(std::move(request));
  }
  void start()
  {
    for (std::size_t i = 0; i < _worker_count; ++i) {
      _workers.emplace_back([this](std::stop_token stop) { work(stop); });
    }
    while (_started.load() != _worker_count)
      std::this_thread::yield();
  }
  void shutdown()
  {
    for (auto& worker : _workers)
      worker.request_stop();
    _armed.store(true);
    _workers.clear();
  }
  void arm(std::int64_t origin)
  {
    _origin = origin;
    _armed.store(true);
  }
  std::vector<trace_event> events() const
  {
    std::lock_guard lock(_trace_mutex);
    return _trace;
  }

 private:
  void record(std::string name, sirius::io::grouped_io_request const& request) noexcept
  {
    try {
      std::lock_guard lock(_trace_mutex);
      _trace.push_back({std::move(name),
                        std::filesystem::path(request.obj->object_path()).filename().string(),
                        !request.slices.front().has_device_request(),
                        now_ns()});
    } catch (...) {
      std::abort();
    }
  }
  void work(std::stop_token stop) noexcept
  {
    std::uint8_t* staging{};
    if (cudaSetDevice(0) != cudaSuccess || cudaMallocHost(&staging, 4 * chunk_size) != cudaSuccess)
      std::abort();
    _started.fetch_add(1);
    while (!stop.stop_requested()) {
      auto request = take_next(1ms);
      if (!request) continue;
      while (!_armed.load() && !stop.stop_requested())
        std::this_thread::yield();
      record("service_start", *request);
      if (!request->front().has_device_request()) {
        std::this_thread::sleep_until(clock_type::time_point{std::chrono::nanoseconds{_origin}} +
                                      _gate);
      }
      std::this_thread::sleep_for(_service);
      auto const fd = ::open(request->obj->object_path().c_str(), O_RDONLY);
      try {
        if (fd < 0) throw std::runtime_error("fixture open failed");
        for (auto const& slice : request->slices) {
          std::size_t done = 0;
          while (done < slice.size()) {
            auto const n = ::pread(fd, staging + done, slice.size() - done, slice.offset() + done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("fixture pread failed or short file");
            done += static_cast<std::size_t>(n);
          }
          if (slice.h_buffer.is_contiguous()) {
            std::memcpy(std::get<std::uint8_t*>(slice.h_buffer.buffer), staging, slice.size());
          } else if (slice.h_buffer.is_fragmented()) {
            for (auto* chunk : slice.h_buffer.fragments()) {
              auto const begin = std::max(slice.offset(), chunk->offset);
              auto const end = std::min(slice.offset() + slice.size(), chunk->offset + chunk_size);
              if (begin < end)
                std::memcpy(chunk->data + begin - chunk->offset,
                            staging + begin - slice.offset(),
                            end - begin);
            }
          }
          if (slice.has_device_request()) {
            void* destination  = slice.d_buffer.data;
            void const* source = staging;
            auto const length  = slice.size();
            check_cuda(
              cudf::detail::memcpy_batch_async(&destination,
                                               &source,
                                               &length,
                                               1,
                                               rmm::cuda_stream_view{slice.d_buffer.stream.get()}));
            check_cuda(cudaStreamSynchronize(slice.d_buffer.stream.get()));
          }
        }
        record("service_complete", *request);
        while (!request->empty()) {
          auto slice = request->take_front();
          if (slice.on_complete) (*slice.on_complete)(slice.h_buffer.fragments(), true);
          request->coordinator->on_complete();
        }
      } catch (...) {
        request->cancel_remaining(std::current_exception());
      }
      if (fd >= 0) ::close(fd);
    }
    cudaFreeHost(staging);
  }
  std::size_t _worker_count;
  std::chrono::microseconds _service;
  std::chrono::microseconds _gate;
  std::atomic<std::size_t> _started{};
  std::atomic<bool> _armed{};
  std::int64_t _origin{};
  mutable std::mutex _trace_mutex;
  std::vector<trace_event> _trace;
  std::vector<std::jthread> _workers;
};

class benchmark_context final : public sirius::io::templated_ioctx<benchmark_reactor> {
 public:
  using templated_ioctx::templated_ioctx;
  sirius::io::io_context_type type() const noexcept override
  {
    return sirius::io::io_context_type::uring;
  }
  benchmark_reactor& reactor() noexcept { return *_reactors.front(); }
};

std::uint8_t pattern(std::size_t source, std::size_t offset)
{
  return static_cast<std::uint8_t>((source * 71 + offset * 13 + offset / 257) % 251);
}

void run(std::string const& directory,
         std::string const& scenario,
         std::size_t workers,
         std::size_t bytes,
         int iterations,
         int warmups,
         int gate_us,
         int service_us)
{
  std::filesystem::create_directories(directory);
  std::array<std::string, sources> paths;
  for (std::size_t source = 0; source < sources; ++source) {
    paths[source] = directory + "/" + std::string(1, static_cast<char>('A' + source));
    std::vector<std::uint8_t> contents(4 * chunk_size);
    for (std::size_t i = 0; i < contents.size(); ++i)
      contents[i] = pattern(source, i);
    std::ofstream file(paths[source], std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<char const*>(contents.data()), contents.size());
    if (!file) throw std::runtime_error("cannot write fixture");
  }
  cucascade::memory::reservation_manager_configurator builder;
  builder.set_number_of_gpus(1)
    .set_gpu_usage_limit(1ULL << 30)
    .set_reservation_fraction_per_gpu(0.75)
    .set_per_numa_region_capacity(1ULL << 30)
    .use_gpu_id_as_host_id()
    .set_reservation_fraction_per_numa_region(0.75);
  sirius::memory::sirius_memory_reservation_manager memory(builder.build());
  std::ofstream results(directory + "/samples.csv");
  std::ofstream trace(directory + "/trace.csv");
  results << "iteration,a_ready_us,a_get_us,all_get_us,b_ready_us,c_ready_us\n";
  trace << "iteration,event,source,prefetch,time_us\n";
  for (int iteration = -warmups; iteration < iterations; ++iteration) {
    ::alarm(20);
    auto context = std::make_shared<benchmark_context>(1, [&] {
      return std::make_unique<benchmark_reactor>(
        workers, std::chrono::microseconds{service_us}, std::chrono::microseconds{gate_us});
    });
    sirius::io::cache::config config;
    config.mode = sirius::io::cache::cache_mode::sirius;
    config.apply_mode();
    context->initialize_cache(memory, config, single_gpu_topology());
    context->start();
    std::array<std::unique_ptr<sirius::io::sirius_datasource>, sources> datasource;
    rmm::cuda_stream stream;
    std::array<rmm::device_buffer, sources> destinations;
    observation observed;
    observed.bytes = bytes;
    for (std::size_t source = 0; source < sources; ++source) {
      datasource[source]            = context->open_datasource(paths[source]);
      destinations[source]          = rmm::device_buffer(bytes, stream);
      observed.destinations[source] = reinterpret_cast<std::uintptr_t>(destinations[source].data());
    }
    auto const blocked = scenario == "blocked_a" ? 0 : scenario == "blocked_b" ? 1 : -1;
    if (blocked >= 0) {
      std::array<cudf::io::text::byte_range_info, 1> ranges{
        {{0, static_cast<std::int64_t>(bytes)}}};
      auto& source = *datasource[blocked];
      source.fadvise(ranges, 0);
      if (source.prepare_prefetch(false) != sirius::io::prepare_result::prepared ||
          source.prefetch_async([](bool) noexcept {}) != sirius::io::prefetch_refusal::issued) {
        throw std::runtime_error("prefetch was not issued");
      }
    }
    std::array<std::array<std::future<std::size_t>, slices_per_source>, sources> futures;
    for (std::size_t source = 0; source < sources; ++source) {
      for (std::size_t slice = 0; slice < slices_per_source; ++slice) {
        auto const size        = bytes / slices_per_source;
        futures[source][slice] = datasource[source]->device_read_async(
          slice * size,
          size,
          static_cast<std::uint8_t*>(destinations[source].data()) + slice * size,
          stream);
      }
    }
    stream.synchronize();
    active_observation = &observed;
    auto const origin  = now_ns();
    context->reactor().arm(origin);
    auto const first_bytes  = futures[0][0].get();
    auto const first_return = now_ns();
    if (first_bytes != bytes / slices_per_source)
      throw std::runtime_error("wrong first byte count");
    for (std::size_t source = 0; source < sources; ++source) {
      for (std::size_t slice = 0; slice < slices_per_source; ++slice) {
        if (source == 0 && slice == 0) continue;
        if (futures[source][slice].get() != bytes / slices_per_source) {
          throw std::runtime_error("wrong byte count");
        }
      }
    }
    auto const all_return = now_ns();
    stream.synchronize();
    active_observation = nullptr;
    for (std::size_t source = 0; source < sources; ++source) {
      if (observed.copied[source].load() != bytes || observed.ready[source].load() == 0) {
        throw std::runtime_error("copy observation missing or duplicated");
      }
      std::vector<std::uint8_t> contents(bytes);
      check_cuda(
        cudaMemcpy(contents.data(), destinations[source].data(), bytes, cudaMemcpyDeviceToHost));
      for (std::size_t i = 0; i < bytes; ++i) {
        if (contents[i] != pattern(source, i)) throw std::runtime_error("device data mismatch");
      }
    }
    auto elapsed = [origin](std::int64_t value) {
      return static_cast<double>(value - origin) / 1000.;
    };
    if (iteration >= 0) {
      results << iteration << ',' << elapsed(observed.ready[0]) << ',' << elapsed(first_return)
              << ',' << elapsed(all_return) << ',' << elapsed(observed.ready[1]) << ','
              << elapsed(observed.ready[2]) << '\n';
      for (auto const& event : context->reactor().events()) {
        trace << iteration << ',' << event.event << ',' << event.source << ',' << event.prefetch
              << ',' << elapsed(event.timestamp) << '\n';
      }
    }
    for (auto& source : datasource)
      source.reset();
    context->shutdown_cache();
    context->shutdown();
    ::alarm(0);
  }
  if (!results || !trace) throw std::runtime_error("benchmark output failed");
}
}  // namespace

extern "C" cudaError_t real_copy(void* const*,
                                 void const* const*,
                                 std::size_t const*,
                                 std::size_t,
                                 rmm::cuda_stream_view) asm(SIRIUS_BENCH_REAL_COPY);
extern "C" cudaError_t wrapped_copy(void* const*,
                                    void const* const*,
                                    std::size_t const*,
                                    std::size_t,
                                    rmm::cuda_stream_view) asm(SIRIUS_BENCH_WRAPPED_COPY);
extern "C" cudaError_t wrapped_copy(void* const* destinations,
                                    void const* const* sources_ptr,
                                    std::size_t const* sizes,
                                    std::size_t count,
                                    rmm::cuda_stream_view stream)
{
  auto const status = real_copy(destinations, sources_ptr, sizes, count, stream);
  if (status != cudaSuccess || active_observation == nullptr) return status;
  auto& observed = *active_observation;
  std::array<std::size_t, sources> bytes{};
  for (std::size_t i = 0; i < count; ++i) {
    auto const address = reinterpret_cast<std::uintptr_t>(destinations[i]);
    for (std::size_t source = 0; source < sources; ++source) {
      if (address >= observed.destinations[source] &&
          address - observed.destinations[source] < observed.bytes) {
        bytes[source] += sizes[i];
      }
    }
  }
  for (std::size_t source = 0; source < sources; ++source) {
    if (bytes[source] == 0) continue;
    auto const slot = observed.next_token.fetch_add(1);
    if (slot >= observed.tokens.size()) std::abort();
    auto& token                = observed.tokens[slot];
    token                      = {&observed, source, bytes[source]};
    auto const callback_status = cudaLaunchHostFunc(stream.value(), copy_complete, &token);
    if (callback_status != cudaSuccess) std::abort();
  }
  return status;
}

int main(int argc, char** argv)
{
  try {
    if (argc != 9)
      throw std::invalid_argument(
        "usage: benchmark DIRECTORY SCENARIO WORKERS BYTES ITERATIONS WARMUPS GATE_US SERVICE_US");
    std::string scenario  = argv[2];
    auto const workers    = std::stoul(argv[3]);
    auto const bytes      = std::stoul(argv[4]);
    auto const iterations = std::stoi(argv[5]);
    auto const warmups    = std::stoi(argv[6]);
    auto const gate       = std::stoi(argv[7]);
    auto const service    = std::stoi(argv[8]);
    if ((scenario != "none" && scenario != "blocked_a" && scenario != "blocked_b") || workers < 1 ||
        workers > 2 || bytes < 4 || bytes > 4 * chunk_size || bytes % 4 != 0 || iterations < 1 ||
        warmups < 0 || gate < 0 || service < 0) {
      throw std::invalid_argument("invalid benchmark arguments");
    }
    run(argv[1], scenario, workers, bytes, iterations, warmups, gate, service);
    std::cout << "Verified all device buffers; wrote " << argv[1] << "/samples.csv and trace.csv\n";
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
