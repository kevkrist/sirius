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

/**
 * @file host_or.hpp
 * @brief Bitwise OR of host byte ranges, and the worker threads that split one OR over several
 * cores.
 *
 * `sirius::op::detail::unite` combines the chunks that several GPUs staged in pinned host memory
 * with `host_or`. It splits each chunk into parts and hands them to a `host_or_executor`: in the
 * engine a `host_or_workers` shared by every publication, in tests an `inline_host_or_executor`.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

namespace sirius::memory {

/**
 * @brief Writes the bitwise OR of @p inputs to @p out.
 *
 * Uses AVX-512 with non-temporal stores when the CPU has it, so the output does not pass through
 * the cache on its way to the next device copy; otherwise ORs 8 bytes at a time. Never allocates.
 *
 * @pre @p inputs is not empty, every input holds at least `out.size()` bytes, and @p out overlaps
 * no input
 * @param inputs The ranges to combine; only the first `out.size()` bytes of each are read
 * @param out The range that receives the union
 */
void host_or(std::span<std::span<std::byte const> const> inputs, std::span<std::byte> out) noexcept;

/**
 * @brief Something that runs the parts of one split job, possibly on several threads.
 *
 * `run(parts, body)` calls `body(i)` exactly once for each `i` in `[0, parts)` and returns after
 * every call returned. `max_parts()` is the largest split worth asking for.
 */
template <class Executor>
concept host_or_executor = requires(Executor& executor, void (*body)(unsigned) noexcept) {
  { executor.max_parts() } noexcept -> std::convertible_to<unsigned>;
  { executor.run(2U, body) } noexcept;
};

/**
 * @brief Runs every part on the calling thread.
 */
class inline_host_or_executor {
 public:
  [[nodiscard]] unsigned max_parts() const noexcept { return 1; }

  template <std::invocable<unsigned> Body>
  void run(unsigned parts, Body&& body) noexcept
  {
    for (unsigned part = 0; part < parts; ++part) {
      body(part);
    }
  }
};

/**
 * @brief A fixed set of helper threads that run the parts of split jobs together with the threads
 * that submit them.
 *
 * The submitting thread always works on its own job and takes every part no helper has started, so
 * a job never waits for a helper: with all helpers busy on other jobs it simply runs on its
 * submitter. Jobs from different threads share the helpers part by part.
 *
 * A helper sleeps while there are no jobs. After finishing a part it keeps polling for the next job
 * for `spin_window` before it sleeps again, because the jobs of one publication follow each other
 * faster than a sleeping thread wakes up.
 */
class host_or_workers {
 public:
  /// How long a helper polls for another job after its last one
  static constexpr std::chrono::microseconds spin_window{500};

  /**
   * @brief Threads one job may use, the submitter included, on a machine with
   * @p hardware_threads: a quarter of them, at least 1 and at most 8.
   */
  [[nodiscard]] static unsigned thread_budget(unsigned hardware_threads) noexcept;

  /**
   * @brief The process-wide workers, sized by `thread_budget` of this machine.
   *
   * They live while any returned pointer does: `sirius_memory_reservation_manager` keeps one for
   * the engine's lifetime, so the threads are started once. A caller without a live manager gets
   * workers of its own that stop when it releases them.
   */
  [[nodiscard]] static std::shared_ptr<host_or_workers> shared();

  /**
   * @brief Starts `threads - 1` helpers.
   *
   * @param threads Threads one job may use, the submitter included; at least 1
   */
  explicit host_or_workers(unsigned threads);

  /// Stops and joins the helpers. No job may be running.
  ~host_or_workers();

  host_or_workers(host_or_workers const&)            = delete;
  host_or_workers& operator=(host_or_workers const&) = delete;

  [[nodiscard]] unsigned max_parts() const noexcept
  {
    return static_cast<unsigned>(_helpers.size()) + 1;
  }

  /**
   * @brief Calls `body(i)` once for each `i` in `[0, parts)`, on this thread and on idle helpers,
   * and returns when all calls returned. Thread-safe.
   *
   * @param parts Number of parts; more than `max_parts()` is allowed and uses no extra thread
   * @param body Called concurrently with distinct arguments; must not throw
   */
  template <std::invocable<unsigned> Body>
  void run(unsigned parts, Body&& body) noexcept
  {
    run_job(
      parts,
      [](void* context, unsigned part) noexcept {
        (*static_cast<std::remove_reference_t<Body>*>(context))(part);
      },
      std::addressof(body));
  }

 private:
  using part_function = void (*)(void* context, unsigned part) noexcept;

  /// One submitted job; lives on its submitter's stack until no helper is inside it.
  struct job {
    part_function invoke;
    void* context;
    unsigned parts;
    unsigned wanted;                  ///< Helpers still to join; guarded by `_mutex`
    std::atomic<unsigned> next{0};    ///< The next part nobody has started
    std::atomic<unsigned> inside{0};  ///< Helpers that joined and have not left
  };

  void run_job(unsigned parts, part_function invoke, void* context) noexcept;
  void help();
  static void work(job& current) noexcept;

  std::mutex _mutex;
  std::condition_variable _wake;
  std::vector<job*> _open;            ///< Jobs that still want helpers; guarded by `_mutex`
  std::atomic<unsigned> _open_count;  ///< `_open.size()`, readable without the mutex
  bool _stop = false;                 ///< Guarded by `_mutex`
  std::vector<std::thread> _helpers;
};

}  // namespace sirius::memory
