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
#include "memory/host_or.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace {
using sirius::memory::host_or;
using sirius::memory::host_or_workers;
using sirius::memory::inline_host_or_executor;

static_assert(sirius::memory::host_or_executor<host_or_workers>);
static_assert(sirius::memory::host_or_executor<inline_host_or_executor>);

std::vector<std::byte> random_bytes(std::size_t size, std::uint32_t seed)
{
  std::mt19937 engine{seed};
  std::vector<std::byte> bytes(size);
  for (auto& value : bytes) {
    value = static_cast<std::byte>(engine() & 0xFF);
  }
  return bytes;
}
}  // namespace

TEST_CASE("host_or equals a byte-wise OR for every size, alignment and input count", "[host_or]")
{
  constexpr std::size_t guard = 96;
  auto const size =
    GENERATE(std::size_t{0}, 1, 7, 8, 63, 64, 65, 96, 127, 128, 1000, 4096, 4097, 262147);
  auto const input_count = GENERATE(std::size_t{1}, 2, 3, 5);
  auto const shift       = GENERATE(std::size_t{0}, 1, 31, 63);

  std::vector<std::vector<std::byte>> sources;
  std::vector<std::span<std::byte const>> inputs;
  for (std::size_t index = 0; index < input_count; ++index) {
    // Inputs start at their own odd offsets and are longer than the output.
    sources.push_back(
      random_bytes(size + index + 3, static_cast<std::uint32_t>(17 * index + size)));
    inputs.emplace_back(sources.back().data() + index, size + 3);
  }
  std::vector<std::byte> expected(size);
  for (std::size_t index = 0; index < size; ++index) {
    for (auto const& input : inputs) {
      expected[index] |= input[index];
    }
  }
  // Bytes around the output must stay untouched.
  std::vector<std::byte> storage(guard + shift + size + guard, std::byte{0xA5});
  std::span<std::byte> const out{storage.data() + guard + shift, size};
  host_or(inputs, out);
  CHECK(std::ranges::equal(out, expected));
  CHECK(std::ranges::all_of(std::span{storage}.first(guard + shift),
                            [](std::byte value) { return value == std::byte{0xA5}; }));
  CHECK(std::ranges::all_of(std::span{storage}.last(guard),
                            [](std::byte value) { return value == std::byte{0xA5}; }));
}

TEST_CASE("host_or_workers budgets a quarter of the hardware threads, between 1 and 8", "[host_or]")
{
  CHECK(host_or_workers::thread_budget(0) == 1);
  CHECK(host_or_workers::thread_budget(2) == 1);
  CHECK(host_or_workers::thread_budget(8) == 2);
  CHECK(host_or_workers::thread_budget(16) == 4);
  CHECK(host_or_workers::thread_budget(32) == 8);
  CHECK(host_or_workers::thread_budget(96) == 8);
}

TEST_CASE("host_or_workers runs every part exactly once", "[host_or]")
{
  auto const threads = GENERATE(1U, 2U, 8U);
  host_or_workers workers{threads};
  CHECK(workers.max_parts() == threads);
  for (unsigned parts : {0U, 1U, 2U, 5U, 8U, 33U}) {
    std::vector<std::atomic<int>> calls(parts);
    for (int round = 0; round < 200; ++round) {
      workers.run(parts, [&](unsigned part) noexcept { calls[part].fetch_add(1); });
    }
    CHECK(std::ranges::all_of(calls, [](auto const& count) { return count.load() == 200; }));
  }
}

TEST_CASE("host_or_workers shares its helpers between concurrent submitters", "[host_or]")
{
  constexpr unsigned submitters = 12;
  constexpr unsigned parts      = 6;
  constexpr int rounds          = 300;
  host_or_workers workers{4};
  std::vector<std::atomic<int>> calls(submitters * parts);
  {
    std::vector<std::jthread> threads;
    for (unsigned submitter = 0; submitter < submitters; ++submitter) {
      threads.emplace_back([&, submitter] {
        for (int round = 0; round < rounds; ++round) {
          workers.run(
            parts, [&](unsigned part) noexcept { calls[submitter * parts + part].fetch_add(1); });
        }
      });
    }
  }
  CHECK(std::ranges::all_of(calls, [](auto const& count) { return count.load() == rounds; }));
}

TEST_CASE("host_or_workers::shared hands out one instance while it is held", "[host_or]")
{
  auto first        = host_or_workers::shared();
  auto const second = host_or_workers::shared();
  CHECK(first == second);
  CHECK(first->max_parts() == host_or_workers::thread_budget(std::thread::hardware_concurrency()));
  std::atomic<int> calls{0};
  first->run(first->max_parts(), [&](unsigned) noexcept { calls.fetch_add(1); });
  CHECK(calls.load() == static_cast<int>(first->max_parts()));
}

TEST_CASE("inline_host_or_executor runs every part on the calling thread", "[host_or]")
{
  inline_host_or_executor executor;
  CHECK(executor.max_parts() == 1);
  std::vector<unsigned> order;
  auto const caller = std::this_thread::get_id();
  executor.run(3, [&](unsigned part) noexcept {
    CHECK(std::this_thread::get_id() == caller);
    order.push_back(part);
  });
  CHECK(order == std::vector<unsigned>{0, 1, 2});
}

#if defined(__linux__)
TEST_CASE("host_or_workers helpers are not confined to the CPUs of the thread that starts them",
          "[host_or]")
{
  cpu_set_t process;
  CPU_ZERO(&process);
  REQUIRE(sched_getaffinity(getpid(), sizeof(process), &process) == 0);
  if (CPU_COUNT(&process) < 2) { return; }
  std::unique_ptr<host_or_workers> workers;
  // The engine starts its workers from a thread bound to one CPU.
  std::jthread{[&] {
    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(sched_getcpu(), &one);
    REQUIRE(pthread_setaffinity_np(pthread_self(), sizeof(one), &one) == 0);
    workers = std::make_unique<host_or_workers>(3);
  }}.join();
  auto const caller = std::this_thread::get_id();
  std::atomic<int> helper_cpus{0};
  std::atomic<int> helped{0};
  // Parts that outlast a helper's wake-up, so that helpers take some.
  for (int round = 0; round < 50 && helped.load() == 0; ++round) {
    workers->run(3, [&](unsigned) noexcept {
      std::this_thread::sleep_for(std::chrono::milliseconds{2});
      if (std::this_thread::get_id() == caller) { return; }
      cpu_set_t mine;
      CPU_ZERO(&mine);
      (void)pthread_getaffinity_np(pthread_self(), sizeof(mine), &mine);
      helper_cpus.store(CPU_COUNT(&mine));
      helped.fetch_add(1);
    });
  }
  REQUIRE(helped.load() > 0);
  CHECK(helper_cpus.load() == CPU_COUNT(&process));
}
#endif
