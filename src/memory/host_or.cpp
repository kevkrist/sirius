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

#include "memory/host_or.hpp"

#include <cucascade/exec/thread_util.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

#if defined(__x86_64__)
#include <immintrin.h>
#endif
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace sirius::memory {

namespace {

void or_bytes(std::span<std::span<std::byte const> const> inputs,
              std::byte* out,
              std::size_t first,
              std::size_t last) noexcept
{
  for (auto index = first; index < last; ++index) {
    auto value = inputs.front()[index];
    for (auto const& input : inputs.subspan(1)) {
      value |= input[index];
    }
    out[index] = value;
  }
}

void or_words(std::span<std::span<std::byte const> const> inputs, std::span<std::byte> out) noexcept
{
  auto const whole = out.size() - out.size() % sizeof(std::uint64_t);
  for (std::size_t index = 0; index < whole; index += sizeof(std::uint64_t)) {
    std::uint64_t value = 0;
    for (auto const& input : inputs) {
      std::uint64_t word;
      std::memcpy(&word, input.data() + index, sizeof(word));
      value |= word;
    }
    std::memcpy(out.data() + index, &value, sizeof(value));
  }
  or_bytes(inputs, out.data(), whole, out.size());
}

#if defined(__x86_64__)
constexpr std::size_t vector_bytes = 64;

__attribute__((target("avx512f"))) void or_avx512(
  std::span<std::span<std::byte const> const> inputs, std::span<std::byte> out) noexcept
{
  // Non-temporal stores need an aligned destination.
  auto const address = reinterpret_cast<std::uintptr_t>(out.data());
  auto const head    = std::min(out.size(), (vector_bytes - address % vector_bytes) % vector_bytes);
  auto const whole   = head + (out.size() - head) / vector_bytes * vector_bytes;
  or_bytes(inputs, out.data(), 0, head);
  for (auto index = head; index < whole; index += vector_bytes) {
    auto value = _mm512_loadu_si512(inputs.front().data() + index);
    for (auto const& input : inputs.subspan(1)) {
      value = _mm512_or_si512(value, _mm512_loadu_si512(input.data() + index));
    }
    _mm512_stream_si512(reinterpret_cast<__m512i*>(out.data() + index), value);
  }
  // Order the non-temporal stores before whatever reads the output next.
  _mm_sfence();
  or_bytes(inputs, out.data(), whole, out.size());
}
#endif

/**
 * @brief Lets the calling thread run on every CPU the process's main thread may use.
 *
 * A new thread inherits the CPU set of the thread that starts it, and the engine starts the helpers
 * from a thread it has bound to a few CPUs; helpers confined there would run one after the other.
 */
void use_process_cpus() noexcept
{
#if defined(__linux__)
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  if (::sched_getaffinity(::getpid(), sizeof(cpus), &cpus) == 0) {
    (void)::pthread_setaffinity_np(::pthread_self(), sizeof(cpus), &cpus);
  }
#endif
}

void pause() noexcept
{
#if defined(__x86_64__)
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}

}  // namespace

void host_or(std::span<std::span<std::byte const> const> inputs, std::span<std::byte> out) noexcept
{
#if defined(__x86_64__)
  static bool const avx512 = __builtin_cpu_supports("avx512f");
  if (avx512) { return or_avx512(inputs, out); }
#endif
  or_words(inputs, out);
}

unsigned host_or_workers::thread_budget(unsigned hardware_threads) noexcept
{
  return std::clamp(hardware_threads / 4, 1U, 8U);
}

std::shared_ptr<host_or_workers> host_or_workers::shared()
{
  static std::mutex mutex;
  static std::weak_ptr<host_or_workers> live;
  std::lock_guard lock{mutex};
  auto workers = live.lock();
  if (!workers) {
    workers = std::make_shared<host_or_workers>(thread_budget(std::thread::hardware_concurrency()));
    live    = workers;
  }
  return workers;
}

host_or_workers::host_or_workers(unsigned threads) : _open_count{0}
{
  _helpers.reserve(std::max(threads, 1U) - 1);
  try {
    for (unsigned helper = 1; helper < threads; ++helper) {
      _helpers.emplace_back([this] { help(); });
    }
  } catch (...) {
    // A thread that cannot start leaves fewer helpers, not a failed engine.
  }
}

host_or_workers::~host_or_workers()
{
  {
    std::lock_guard lock{_mutex};
    _stop = true;
  }
  _wake.notify_all();
  for (auto& helper : _helpers) {
    helper.join();
  }
}

void host_or_workers::work(job& current) noexcept
{
  for (auto part = current.next.fetch_add(1, std::memory_order_relaxed); part < current.parts;
       part      = current.next.fetch_add(1, std::memory_order_relaxed)) {
    current.invoke(current.context, part);
  }
}

void host_or_workers::run_job(unsigned parts, part_function invoke, void* context) noexcept
{
  job current{invoke, context, parts, 0};
  auto const helpers = std::min<std::size_t>(parts == 0 ? 0 : parts - 1, _helpers.size());
  if (helpers != 0) {
    current.wanted = static_cast<unsigned>(helpers);
    {
      std::lock_guard lock{_mutex};
      _open.push_back(&current);
      _open_count.store(static_cast<unsigned>(_open.size()), std::memory_order_release);
    }
    if (helpers == 1) {
      _wake.notify_one();
    } else {
      _wake.notify_all();
    }
  }
  work(current);
  if (helpers == 0) { return; }
  {
    // After this no helper can join; the ones that did are counted in `inside`.
    std::lock_guard lock{_mutex};
    std::erase(_open, &current);
    _open_count.store(static_cast<unsigned>(_open.size()), std::memory_order_release);
  }
  // Helpers finish their last part about when this thread finishes its own.
  for (unsigned spins = 0; current.inside.load(std::memory_order_acquire) != 0; ++spins) {
    if (spins < 4096) {
      pause();
    } else {
      std::this_thread::yield();
    }
  }
}

void host_or_workers::help()
{
  using clock = std::chrono::steady_clock;
  use_process_cpus();
  (void)cucascade::exec::thread_util::set_current_thread_name("host-or");
  std::unique_lock lock{_mutex};
  for (;;) {
    _wake.wait(lock, [this] { return _stop || !_open.empty(); });
    if (_stop) { return; }
    for (;;) {
      // Holding the mutex: join the oldest open job.
      auto* const current = _open.front();
      current->inside.fetch_add(1, std::memory_order_relaxed);
      if (--current->wanted == 0) {
        _open.erase(_open.begin());
        _open_count.store(static_cast<unsigned>(_open.size()), std::memory_order_release);
      }
      lock.unlock();
      work(*current);
      // The submitter may destroy the job as soon as this count drops: do not touch it afterwards.
      current->inside.fetch_sub(1, std::memory_order_release);
      auto const deadline = clock::now() + spin_window;
      while (_open_count.load(std::memory_order_acquire) == 0 && clock::now() < deadline) {
        pause();
      }
      lock.lock();
      if (_stop || _open.empty()) { break; }
    }
    if (_stop) { return; }
  }
}

}  // namespace sirius::memory
