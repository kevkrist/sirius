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

#include <cuda_runtime_api.h>

#include <catch.hpp>

#include <chrono>
#include <cstddef>

namespace sirius::test {
// One-shot CUDA readiness failure on the calling thread, armed only after test
// setup. These wrappers are linked into the test executable, never production.
class scoped_cuda_status_fault {
 public:
  enum operation { wait_event = 1, synchronize_device = 2 };
  scoped_cuda_status_fault(operation op, cudaError_t status);
  ~scoped_cuda_status_fault();
  scoped_cuda_status_fault(scoped_cuda_status_fault const&)            = delete;
  scoped_cuda_status_fault& operator=(scoped_cuda_status_fault const&) = delete;
  bool consumed();
};

// One-shot cudaMemPoolGetAccess failure for one pool and accessor, from any thread. The first
// @p skip matching queries succeed.
class scoped_pool_access_fault {
 public:
  scoped_pool_access_fault(cudaMemPool_t pool, int accessor, std::size_t skip, cudaError_t status);
  ~scoped_pool_access_fault();
  scoped_pool_access_fault(scoped_pool_access_fault const&)            = delete;
  scoped_pool_access_fault& operator=(scoped_pool_access_fault const&) = delete;
  bool consumed() const;
};

/**
 * @brief Injects one publication API result and observes submitted streams until cleanup.
 *
 * Only the calling thread is armed. An optional delay precedes one matching call; owners and
 * observation storage remain test-owned. This helper is linked only into the isolated CUDA fault
 * executable.
 */
class scoped_publication_fault {
 public:
  enum operation { peer_copy, batch_copy, record_event, synchronize_stream, create_event };
  scoped_publication_fault(operation op,
                           std::size_t skip,
                           cudaError_t status,
                           std::chrono::milliseconds delay = {});
  ~scoped_publication_fault();
  scoped_publication_fault(scoped_publication_fault const&)            = delete;
  scoped_publication_fault& operator=(scoped_publication_fault const&) = delete;
  bool consumed() const noexcept;
  bool all_submitted_streams_joined() const noexcept;
  bool invalid_join() const noexcept;
};
}  // namespace sirius::test
