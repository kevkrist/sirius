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
#include <cuda/dynamic_filter_membership_probe.cuh>
#include <cuda_runtime_api.h>

#include <cucascade/error.hpp>
#include <op/dynamic_filter/dynamic_filter_mask_ops.hpp>

#include <algorithm>
#include <cstddef>
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

/// The steps of one fused round, passed by value as the kernel argument (hence the fixed bound).
struct fused_membership_steps {
  membership_probe probe[k_fused_membership_max_steps];
};

/**
 * out[i] = residual(i) && step_0(i) && ... && step_{K-1}(i) with early-out; `residual` may alias
 * `out`. local[0] counts rows passing the residual, local[s + 1] rows surviving step s; one atomic
 * per block per counter. Grid-stride over the split: the pass is bound by the residual/key/mask
 * streams and the membership structures' L2 sectors, so one byte per row per stream is read once.
 */
template <int K>
__global__ void __launch_bounds__(kBlockThreads)
  fused_membership_mask_kernel(bool const* residual,
                               cudf::bitmask_type const* residual_valid,
                               cudf::size_type residual_offset,
                               fused_membership_steps const steps,
                               bool* out,
                               cudf::size_type n,
                               cudf::size_type* residual_count,
                               cudf::size_type* __restrict__ step_counts)
{
  static_assert(K >= 1 && K <= static_cast<int>(k_fused_membership_max_steps));
  using block_reduce = cub::BlockReduce<cudf::size_type, kBlockThreads>;
  __shared__ typename block_reduce::TempStorage temp;

  cudf::size_type local[K + 1] = {};
  auto const stride            = static_cast<cudf::size_type>(gridDim.x) * blockDim.x;
  for (auto i = static_cast<cudf::size_type>(blockIdx.x * blockDim.x + threadIdx.x); i < n;
       i += stride) {
    bool keep =
      residual == nullptr || (residual[i] && valid_at(residual_valid, residual_offset + i));
    local[0] += keep ? 1 : 0;
#pragma unroll
    for (int s = 0; s < K; ++s) {
      if (keep) { keep = membership_probe_keeps(steps.probe[s], i); }
      local[s + 1] += keep ? 1 : 0;
    }
    out[i] = keep;
  }

#pragma unroll
  for (int s = 0; s <= K; ++s) {
    if (s > 0) { __syncthreads(); }  // the reductions share `temp`
    auto const block_total = block_reduce(temp).Sum(local[s]);
    if (threadIdx.x == 0 && block_total != 0) {
      auto* const target = s == 0 ? residual_count : step_counts + (s - 1);
      if (target != nullptr) { atomicAdd(target, block_total); }
    }
  }
}

template <int K>
void launch_fused_membership_mask(bool const* residual,
                                  cudf::bitmask_type const* residual_valid,
                                  cudf::size_type residual_offset,
                                  fused_membership_steps const& steps,
                                  bool* out,
                                  cudf::size_type n,
                                  cudf::size_type* residual_count,
                                  cudf::size_type* step_counts,
                                  rmm::cuda_stream_view stream)
{
  auto const blocks = static_cast<int>(
    std::min<cudf::size_type>(kMaxBlocks, (n + kBlockThreads - 1) / kBlockThreads));
  fused_membership_mask_kernel<K><<<blocks, kBlockThreads, 0, stream.value()>>>(
    residual, residual_valid, residual_offset, steps, out, n, residual_count, step_counts);
}

}  // namespace

void fused_membership_mask(bool const* residual,
                           cudf::bitmask_type const* residual_valid,
                           cudf::size_type residual_offset,
                           std::span<membership_probe const> steps,
                           bool* out,
                           cudf::size_type n,
                           cudf::size_type* residual_count,
                           cudf::size_type* step_counts,
                           rmm::cuda_stream_view stream)
{
  if (steps.empty() || steps.size() > k_fused_membership_max_steps) {
    throw std::invalid_argument(
      "[fused_membership_mask] a round takes 1..k_fused_membership_max_steps membership steps");
  }
  if (n == 0) { return; }

  fused_membership_steps args{};
  std::copy(steps.begin(), steps.end(), args.probe);
  switch (steps.size()) {
    case 1:
      launch_fused_membership_mask<1>(residual,
                                      residual_valid,
                                      residual_offset,
                                      args,
                                      out,
                                      n,
                                      residual_count,
                                      step_counts,
                                      stream);
      break;
    case 2:
      launch_fused_membership_mask<2>(residual,
                                      residual_valid,
                                      residual_offset,
                                      args,
                                      out,
                                      n,
                                      residual_count,
                                      step_counts,
                                      stream);
      break;
    case 3:
      launch_fused_membership_mask<3>(residual,
                                      residual_valid,
                                      residual_offset,
                                      args,
                                      out,
                                      n,
                                      residual_count,
                                      step_counts,
                                      stream);
      break;
    default:
      static_assert(k_fused_membership_max_steps == 4, "add a case per supported round size");
      launch_fused_membership_mask<4>(residual,
                                      residual_valid,
                                      residual_offset,
                                      args,
                                      out,
                                      n,
                                      residual_count,
                                      step_counts,
                                      stream);
      break;
  }
  CUCASCADE_CUDA_TRY(cudaPeekAtLastError());
}

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
