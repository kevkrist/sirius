// SPDX-License-Identifier: Apache-2.0
#ifndef SIMPATICO_UTIL_STREAM_POOL_HPP
#define SIMPATICO_UTIL_STREAM_POOL_HPP

#include <cuda_runtime.h>

#include <cstddef>
#include <utility>
#include <vector>

namespace simpatico {

/// A fixed set of CUDA streams for cross-column parallelism (one column per
/// worker thread, each on its own stream). Per-column compress/decompress is
/// single-stream and does not use this.
struct stream_pool {
  std::vector<cudaStream_t> streams;

  stream_pool() = default;
  ~stream_pool() noexcept { shutdown(); }

  // Not copyable: copying CUDA stream handles would alias them and cause
  // double-destroy on destruction.
  stream_pool(const stream_pool&)            = delete;
  stream_pool& operator=(const stream_pool&) = delete;

  stream_pool(stream_pool&& other) noexcept : streams(std::move(other.streams)) {}
  stream_pool& operator=(stream_pool&& other) noexcept
  {
    if (this != &other) {
      shutdown();
      streams = std::move(other.streams);
    }
    return *this;
  }

  /// Initialize with n streams. Returns false on error.
  bool init(size_t n);
  /// Best-effort synchronization followed by destruction of all streams.
  /// Errors are deliberately suppressed so this is safe during unwinding.
  void shutdown() noexcept;
  /// Synchronize all streams, throwing when CUDA reports a failure.
  void sync_all();
};

}  // namespace simpatico

#endif  // SIMPATICO_UTIL_STREAM_POOL_HPP
