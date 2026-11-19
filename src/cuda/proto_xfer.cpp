/*
 * Copyright 2025, Sirius Contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License. You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied. See the License for the specific language governing permissions and limitations under
 * the License.
 */

// PROTOTYPE [xfer-PR] (proto/xfer-host). See memory/proto_xfer.hpp.

#include "memory/proto_xfer.hpp"

#include <immintrin.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>

namespace sirius::memory::proto_xfer {
namespace {

[[nodiscard]] std::string_view env(char const* name) noexcept
{
  auto const* text = std::getenv(name);
  return text == nullptr ? std::string_view{} : std::string_view{text};
}

[[nodiscard]] std::size_t env_number(char const* name, std::size_t fallback) noexcept
{
  auto const* text = std::getenv(name);
  if (text == nullptr || *text == '\0') { return fallback; }
  return static_cast<std::size_t>(std::strtoull(text, nullptr, 10));
}

void or_range_generic(std::byte* out,
                      std::byte const* const* in,
                      std::size_t count,
                      std::size_t n,
                      std::size_t i = 0)
{
  for (; i + 8 <= n; i += 8) {
    std::uint64_t value;
    std::memcpy(&value, in[0] + i, 8);
    for (std::size_t k = 1; k < count; ++k) {
      std::uint64_t other;
      std::memcpy(&other, in[k] + i, 8);
      value |= other;
    }
    std::memcpy(out + i, &value, 8);
  }
  for (; i < n; ++i) {
    auto value = in[0][i];
    for (std::size_t k = 1; k < count; ++k) {
      value |= in[k][i];
    }
    out[i] = value;
  }
}

__attribute__((target("avx512f"))) void or_range_avx512(std::byte* out,
                                                        std::byte const* const* in,
                                                        std::size_t count,
                                                        std::size_t n)
{
  // Non-temporal stores: the result is read next by the upload's DMA, never by this core.
  bool const aligned = (reinterpret_cast<std::uintptr_t>(out) % 64) == 0;
  std::size_t i      = 0;
  for (; i + 64 <= n; i += 64) {
    __m512i value = _mm512_loadu_si512(in[0] + i);
    for (std::size_t k = 1; k < count; ++k) {
      value = _mm512_or_si512(value, _mm512_loadu_si512(in[k] + i));
    }
    if (aligned) {
      _mm512_stream_si512(reinterpret_cast<__m512i*>(out + i), value);
    } else {
      _mm512_storeu_si512(out + i, value);
    }
  }
  _mm_sfence();
  if (i < n) { or_range_generic(out, in, count, n, i); }
}

struct or_job {
  std::span<std::byte* const> out;
  std::span<std::span<std::byte* const> const> inputs;
  std::size_t block_bytes;
  std::size_t bytes;
  std::size_t granule;
  std::size_t items_per_block;
  std::size_t items;
  bool avx512;
  std::atomic<std::size_t> next{0};
};

void work(or_job& job)
{
  constexpr std::size_t max_inputs = 64;
  std::byte const* in[max_inputs];
  auto const count = std::min(job.inputs.size(), max_inputs);
  for (;;) {
    auto const item = job.next.fetch_add(1, std::memory_order_relaxed);
    if (item >= job.items) { return; }
    auto const block       = item / job.items_per_block;
    auto const block_start = block * job.block_bytes;
    if (block_start >= job.bytes) { continue; }
    auto const block_valid = std::min(job.block_bytes, job.bytes - block_start);
    auto const lo          = (item % job.items_per_block) * job.granule;
    if (lo >= block_valid) { continue; }
    auto const n = std::min(job.granule, block_valid - lo);
    for (std::size_t k = 0; k < count; ++k) {
      in[k] = job.inputs[k][block] + lo;
    }
    if (job.avx512) {
      or_range_avx512(job.out[block] + lo, in, count, n);
    } else {
      or_range_generic(job.out[block] + lo, in, count, n);
    }
  }
}

/**
 * @brief Persistent helpers of the host OR. One job at a time; helpers spin briefly between jobs,
 * because the chunks of one publication arrive a fraction of a millisecond apart.
 */
class or_pool {
 public:
  static or_pool& instance()
  {
    static or_pool pool;
    return pool;
  }

  void run(or_job& job, unsigned threads)
  {
    std::lock_guard serial{_serial};
    auto const helpers = threads == 0 ? 0u : threads - 1;
    ensure(helpers);
    _job = &job;
    _helpers.store(helpers);
    auto const generation = _generation.load() + 1;
    _open.store(generation);
    {
      std::lock_guard lock{_mutex};
      _generation.store(generation);
    }
    _wake.notify_all();
    work(job);
    _open.store(0);
    while (_active.load() != 0) {
      _mm_pause();
    }
    _job = nullptr;
  }

 private:
  or_pool() = default;

  ~or_pool()
  {
    {
      std::lock_guard lock{_mutex};
      _stop.store(true);
      _generation.fetch_add(1);
    }
    _wake.notify_all();
    for (auto& worker : _workers) {
      worker.join();
    }
  }

  void ensure(unsigned helpers)
  {
    while (_workers.size() < helpers) {
      auto const id = static_cast<unsigned>(_workers.size());
      _workers.emplace_back([this, id] { loop(id); });
    }
  }

  void loop(unsigned id)
  {
    using clock            = std::chrono::steady_clock;
    std::uint64_t seen     = _generation.load();
    auto spin_until        = clock::now();
    constexpr auto spin_for = std::chrono::microseconds{500};
    for (;;) {
      auto current = _generation.load();
      if (current == seen) {
        if (clock::now() < spin_until) {
          _mm_pause();
          continue;
        }
        std::unique_lock lock{_mutex};
        _wake.wait(lock, [&] { return _generation.load() != seen; });
        current = _generation.load();
      }
      if (_stop.load()) { return; }
      seen = current;
      if (id < _helpers.load()) {
        _active.fetch_add(1);
        if (_open.load() == current) { work(*_job); }
        _active.fetch_sub(1);
      }
      spin_until = clock::now() + spin_for;
    }
  }

  std::mutex _serial;
  std::mutex _mutex;
  std::condition_variable _wake;
  std::vector<std::thread> _workers;
  std::atomic<std::uint64_t> _generation{0};
  std::atomic<std::uint64_t> _open{0};
  std::atomic<unsigned> _helpers{0};
  std::atomic<int> _active{0};
  std::atomic<bool> _stop{false};
  or_job* _job{nullptr};
};

std::mutex g_last_mutex;
finish_observation g_last;

}  // namespace

bloom_mode bloom_host_mode() noexcept
{
  auto const text = env("SIRIUS_PROTO_XFER_HOST");
  if (text == "gpuor") { return bloom_mode::gpuor; }
  if (text == "hostor") { return bloom_mode::hostor; }
  if (text == "auto") { return bloom_mode::automatic; }
  return bloom_mode::off;
}

std::size_t window_slots() noexcept
{
  return std::max<std::size_t>(1, env_number("SIRIUS_PROTO_XFER_WINDOW", 4));
}

unsigned or_threads() noexcept
{
  return static_cast<unsigned>(
    std::clamp<std::size_t>(env_number("SIRIUS_PROTO_XFER_OR_THREADS", 16), 1, 128));
}

bool replica_enabled() noexcept
{
  auto const text = env("SIRIUS_PROTO_XFER_REPLICA");
  if (text.empty()) { return bloom_host_mode() != bloom_mode::off; }
  return text != "0";
}

bool fail_host_allocation() noexcept { return env("SIRIUS_PROTO_XFER_FAIL_HOST") == "1"; }

bool checksum_enabled() noexcept { return env("SIRIUS_PROTO_XFER_CHECKSUM") == "1"; }

void host_or(std::span<std::byte* const> out,
             std::span<std::span<std::byte* const> const> inputs,
             std::size_t block_bytes,
             std::size_t bytes,
             unsigned threads)
{
  if (bytes == 0 || inputs.empty() || block_bytes == 0) { return; }
  or_job job{};
  job.out             = out;
  job.inputs          = inputs;
  job.block_bytes     = block_bytes;
  job.bytes           = bytes;
  job.granule         = std::min<std::size_t>(block_bytes, std::size_t{128} << 10);
  job.items_per_block = (block_bytes + job.granule - 1) / job.granule;
  job.items           = ((bytes + block_bytes - 1) / block_bytes) * job.items_per_block;
  job.avx512          = __builtin_cpu_supports("avx512f") != 0;
  // A granule is 128 KiB or one block: do small chunks inline instead of waking helpers.
  if (threads <= 1 || job.items <= 1) {
    work(job);
    return;
  }
  or_pool::instance().run(job, threads);
}

void set_last_finish(finish_observation observation)
{
  std::lock_guard lock{g_last_mutex};
  g_last = std::move(observation);
}

finish_observation last_finish()
{
  std::lock_guard lock{g_last_mutex};
  return g_last;
}

counters& stats() noexcept
{
  static counters instance;
  return instance;
}

}  // namespace sirius::memory::proto_xfer
