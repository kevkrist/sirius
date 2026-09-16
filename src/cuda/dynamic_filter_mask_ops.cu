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

#include <cudf/utilities/bit.hpp>

#include <cub/block/block_reduce.cuh>
#include <cuda_runtime_api.h>

#include <cucascade/error.hpp>
#include <op/dynamic_filter/dynamic_filter_mask_ops.hpp>

#include <algorithm>
#include <stdexcept>

namespace sirius::op {

namespace {

constexpr int kBlockThreads = 256;
constexpr int kMaxBlocks    = 4096;

__device__ __forceinline__ bool valid_at(cudf::bitmask_type const* bitmask,
                                         cudf::size_type index) noexcept
{
  return bitmask == nullptr || cudf::bit_is_set(bitmask, index);
}

/// acc[i] = acc[i] && valid(acc, i) && (mask ? mask[i] && valid(mask, i) : true); one atomic per
/// block adds the block's survivor count.
__global__ void and_mask_count_kernel(bool* __restrict__ acc,
                                      cudf::bitmask_type const* acc_valid,
                                      cudf::size_type acc_offset,
                                      bool const* __restrict__ mask,
                                      cudf::bitmask_type const* mask_valid,
                                      cudf::size_type mask_offset,
                                      cudf::size_type n,
                                      cudf::size_type* __restrict__ survivor_count)
{
  using block_reduce = cub::BlockReduce<cudf::size_type, kBlockThreads>;
  __shared__ typename block_reduce::TempStorage temp;

  cudf::size_type local = 0;
  auto const stride     = static_cast<cudf::size_type>(gridDim.x) * blockDim.x;
  for (auto i = static_cast<cudf::size_type>(blockIdx.x * blockDim.x + threadIdx.x); i < n;
       i += stride) {
    bool keep = acc[i] && valid_at(acc_valid, acc_offset + i);
    if (mask != nullptr) { keep = keep && mask[i] && valid_at(mask_valid, mask_offset + i); }
    acc[i] = keep;
    local += keep ? 1 : 0;
  }
  auto const block_total = block_reduce(temp).Sum(local);
  if (threadIdx.x == 0 && block_total != 0) { atomicAdd(survivor_count, block_total); }
}

}  // namespace

void and_mask_count(cudf::column& accumulator,
                    std::optional<cudf::column_view> mask,
                    cudf::size_type* survivor_count,
                    rmm::cuda_stream_view stream)
{
  if (accumulator.type().id() != cudf::type_id::BOOL8) {
    throw std::invalid_argument("[and_mask_count] the accumulator must be a BOOL8 column");
  }
  auto const n = accumulator.size();
  if (mask) {
    if (mask->type().id() != cudf::type_id::BOOL8 || mask->size() != n) {
      throw std::invalid_argument(
        "[and_mask_count] the mask must be a BOOL8 column of the accumulator's size");
    }
  }
  if (n == 0) { return; }

  auto acc_view                        = accumulator.mutable_view();
  bool const* mask_data                = mask ? mask->data<bool>() : nullptr;
  cudf::bitmask_type const* mask_valid = mask && mask->nullable() ? mask->null_mask() : nullptr;
  cudf::size_type const mask_offset    = mask ? mask->offset() : 0;

  auto const blocks = static_cast<int>(
    std::min<cudf::size_type>(kMaxBlocks, (n + kBlockThreads - 1) / kBlockThreads));
  and_mask_count_kernel<<<blocks, kBlockThreads, 0, stream.value()>>>(
    acc_view.data<bool>(),
    acc_view.nullable() ? acc_view.null_mask() : nullptr,
    acc_view.offset(),
    mask_data,
    mask_valid,
    mask_offset,
    n,
    survivor_count);
  CUCASCADE_CUDA_TRY(cudaPeekAtLastError());
}

}  // namespace sirius::op
