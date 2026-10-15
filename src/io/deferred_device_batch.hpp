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

#pragma once

#include "io/types.hpp"

#include <cuda_runtime_api.h>

#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace sirius::io {

class sirius_datasource;
class deferred_batch_registry;

/**
 * @brief Reads collected by one `sirius_datasource` for one explicit CUDA stream and device.
 *
 * Member futures own the batch; destinations remain borrowed until its completion. The mutex
 * protects collection, issuance and cancellation. `sirius_datasource` cancels its batches before
 * destruction under that same mutex, waiting out any issuance that dereferences the owner. Issuance
 * never takes another batch's mutex or the registry mutex. Every member observes the same
 * completion or terminal error, even when its future is moved to another thread.
 */
class deferred_device_batch {
 public:
  deferred_device_batch(sirius_datasource& owner,
                        cudaStream_t stream,
                        int device,
                        std::weak_ptr<deferred_batch_registry> registry) noexcept;

  [[nodiscard]] cudaStream_t stream() const noexcept { return _stream; }
  [[nodiscard]] int device() const noexcept { return _device; }

  /**
   * @brief Append a request if still open; allocation failure leaves the batch unchanged.
   */
  [[nodiscard]] bool try_add(slice const& request);

  [[nodiscard]] bool is_open() const;

  /**
   * @brief Issue this batch before registered siblings on this stream, then wait for this batch and
   * return the member's byte count.
   *
   * Issue calls can block on prefetch, so sibling issuance can delay the completion wait even after
   * this batch finishes. A failed or cancelled batch still dispatches siblings before reporting its
   * terminal error. Concurrent member waits share one issuance; this order does not impose a global
   * order on other waiters.
   */
  std::size_t await(std::size_t bytes);

  /**
   * @brief Issue an open batch without waiting for backend completion.
   */
  void issue_if_open();

  /**
   * @brief Invalidate an open batch without allocating, waiting out any current owner access.
   */
  void cancel() noexcept;

 private:
  enum class state { OPEN, ISSUED, FAILED, CANCELLED };

  /**
   * @brief Issue once under `_mutex`, retaining errors without allocating a replacement future.
   */
  void issue() noexcept;

  sirius_datasource* const _owner;
  cudaStream_t const _stream;
  int const _device;
  /// Maps this batch to other batches on the same stream for sibling issuance.
  std::weak_ptr<deferred_batch_registry> const _registry;
  mutable std::mutex _mutex;
  std::vector<slice> _slices;
  state _state{state::OPEN};
  std::shared_future<std::size_t> _completion;
  std::exception_ptr _error;
};

/**
 * @brief Weakly tracks open batches across datasources on one prefetching cache.
 *
 * A first waiter can issue all files' collected reads on its explicit stream, allowing their
 * backend work to overlap. Expired entries do not keep abandoned batches alive. Registration may
 * run under a datasource's pending mutex. Registry operations take no pending or batch mutex and
 * release their mutex before any issuance, so the lock order is acyclic.
 */
class deferred_batch_registry {
 public:
  /**
   * @brief Whether a stream has an explicit identity that can be preserved on another thread.
   *
   * Null, legacy-default and per-thread-default handles use the eager read path.
   */
  [[nodiscard]] static bool accepts(cudaStream_t stream) noexcept;

  void add(std::shared_ptr<deferred_device_batch> const& batch);

  /**
   * @brief Remove and return a stream's entries without allocating; the caller skips itself and
   * expired entries.
   */
  [[nodiscard]] std::vector<std::weak_ptr<deferred_device_batch>> take_batches(cudaStream_t stream);

 private:
  std::mutex _mutex;
  std::unordered_map<cudaStream_t, std::vector<std::weak_ptr<deferred_device_batch>>> _open;
};

}  // namespace sirius::io
