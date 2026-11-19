/*
 * PROTOTYPE (eager probe): optional throttle of scan tasks while an eagerly overlapped build
 * CONCAT is still moving the build across GPUs. Header-only, process-global, measurement only.
 *
 * SIRIUS_EAGER_SCAN_THROTTLE = K (> 0): while at least one eagerly activated join still waits for
 * its build CONCAT pipeline, at most K tasks of scan-sourced pipelines run their Preparing phase
 * (host-to-GPU copy / decode) per GPU. 0 or unset = off.
 */
#pragma once

#include <array>
#include <condition_variable>
#include <cstdlib>
#include <mutex>

namespace sirius::op::eager_throttle {

struct state_t {
  std::mutex mutex;
  std::condition_variable cv;
  int pending_build_moves = 0;
  std::array<int, 16> running{};
};

inline state_t& state()
{
  static state_t instance;
  return instance;
}

inline int limit()
{
  static int const value = [] {
    auto const* text = std::getenv("SIRIUS_EAGER_SCAN_THROTTLE");
    return text ? std::atoi(text) : 0;
  }();
  return value;
}

//! Per-join move state, guarded by state().mutex: 0 = none, 1 = counted, 2 = ended.
inline void build_move_begin(int& join_state)
{
  if (limit() <= 0) { return; }
  auto& s = state();
  std::lock_guard<std::mutex> guard(s.mutex);
  if (join_state == 0) {
    join_state = 1;
    ++s.pending_build_moves;
  }
}

inline void build_move_end(int& join_state)
{
  if (limit() <= 0) { return; }
  auto& s = state();
  {
    std::lock_guard<std::mutex> guard(s.mutex);
    if (join_state == 1) { --s.pending_build_moves; }
    join_state = 2;
  }
  s.cv.notify_all();
}

//! Blocks while the device already runs `limit()` throttled scan tasks and a build move is
//! pending. Returns true when a slot was taken (release it with scan_release).
inline bool scan_acquire(int device)
{
  if (limit() <= 0 || device < 0 || device >= 16) { return false; }
  auto& s = state();
  std::unique_lock<std::mutex> guard(s.mutex);
  if (s.pending_build_moves <= 0) { return false; }
  s.cv.wait(guard, [&] {
    return s.pending_build_moves <= 0 || s.running[static_cast<std::size_t>(device)] < limit();
  });
  if (s.pending_build_moves <= 0) { return false; }
  ++s.running[static_cast<std::size_t>(device)];
  return true;
}

inline void scan_release(int device)
{
  auto& s = state();
  {
    std::lock_guard<std::mutex> guard(s.mutex);
    --s.running[static_cast<std::size_t>(device)];
  }
  s.cv.notify_all();
}

//! Releases a taken slot at scope exit or on reset().
struct scan_slot {
  int device = -1;
  scan_slot() = default;
  explicit scan_slot(int dev) : device(scan_acquire(dev) ? dev : -1) {}
  scan_slot(scan_slot const&)            = delete;
  scan_slot& operator=(scan_slot const&) = delete;
  void reset() noexcept
  {
    if (device >= 0) {
      scan_release(device);
      device = -1;
    }
  }
  ~scan_slot() { reset(); }
};

}  // namespace sirius::op::eager_throttle
