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
 * @file accumulation_cuda.cuh
 * @brief CUDA status and event helpers shared by the build-filter accumulation sources:
 * `sirius_dynamic_bloom_filter.cu` and `union_transfer.cu`.
 */

#pragma once

#include <rmm/cuda_device.hpp>

#include <cuda_runtime_api.h>

#include <cucascade/cuda/event.hpp>
#include <op/dynamic_filter/detail/accumulated_bloom_builder.hpp>

namespace sirius::op::detail {

using cucascade::cuda::cuda_event;

/**
 * @brief Throws `accumulation_cuda_error` for a failed CUDA call after clearing the error.
 *
 * @param status The result of the CUDA call
 * @param call The name of the CUDA call, used in the error message
 * @param kernel_launch Whether `status` comes from a kernel launch
 */
inline void check_cuda(cudaError_t status, char const* call, bool kernel_launch = false)
{
  if (status == cudaSuccess) { return; }
  (void)cudaGetLastError();
  throw accumulation_cuda_error{status, kernel_launch, call};
}

/**
 * @brief Creates a timing-free event on `device`, restoring the current device afterwards.
 */
[[nodiscard]] inline cuda_event make_event_on(int device)
{
  rmm::cuda_set_device_raii guard{rmm::cuda_device_id{device}};
  return cuda_event{};
}

}  // namespace sirius::op::detail
