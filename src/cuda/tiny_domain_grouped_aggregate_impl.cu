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

// clang-format off
#include <op/aggregate/tiny_domain_grouped_aggregate_impl.hpp>

#include <cudf/column/column.hpp>
#include <cudf/column/column_factories.hpp>
#include <cudf/copying.hpp>
#include <cudf/null_mask.hpp>
#include <cudf/strings/strings_column_view.hpp>
#include <cudf/utilities/bit.hpp>
#include <cudf/utilities/error.hpp>
#include <cudf/utilities/traits.hpp>

#include <rmm/device_uvector.hpp>

#include <cuda_runtime_api.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>
// clang-format on

namespace sirius::op {
namespace {

constexpr int k_block_size                         = 256;
constexpr int k_warps_per_block                    = k_block_size / 32;
constexpr int k_max_groups                         = 64;
constexpr int k_key_radix                          = 257;  // every byte plus one NULL token
constexpr int k_null_key_token                     = 256;
constexpr int k_max_key_domain                     = k_key_radix * k_key_radix;
constexpr int k_status_bad_key                     = 1;
constexpr int k_status_overflow                    = 2;
constexpr int k_status_bad_state                   = 4;
constexpr int k_status_bounded_register_overflow   = 8;
constexpr int k_status_late_key                    = 16;
constexpr int k_max_bounded_register_groups        = 32;
constexpr int k_max_bounded_register_states        = 8;
constexpr int k_max_bounded_register_entries       = 32;
constexpr int k_min_bounded_register_blocks_per_sm = 2;
// Direct unmasked 64-bit specializations at or below this live-state count target
// three resident blocks; larger bins rely on the runtime two-block admission floor.
constexpr int k_three_block_codegen_entry_limit = 24;
constexpr int64_t k_preflight_sample_rows       = int64_t{1} << 16;
constexpr std::size_t k_max_dynamic_smem        = 48 * 1024;
static_assert(k_block_size % 32 == 0);

enum class key_kind : int32_t { STRING = 0, INT8 = 1, UINT8 = 2 };

struct key_spec {
  key_kind kind;
  void const* data;
  cudf::size_type const* offsets;
  char const* chars;
  cudf::bitmask_type const* null_mask;
  cudf::size_type null_mask_offset;
  cudf::size_type row_offset;
};

struct aggregate_spec {
  int32_t kind;
  int32_t input_type;
  int32_t output_type;
  int32_t is_unsigned;
  void const* input_data;
  cudf::size_type input_offset;
  cudf::bitmask_type const* input_null_mask;
  cudf::size_type input_null_mask_offset;
  void* output_data;
  cudf::bitmask_type* output_null_mask;
};

struct bounded_register_state_spec {
  int32_t kind;
  int32_t input_type;
  void const* input_data;
  cudf::size_type input_offset;
  cudf::bitmask_type const* input_null_mask;
  cudf::size_type input_null_mask_offset;
};

struct bounded_register_plan {
  std::vector<bounded_register_state_spec> states;
  std::vector<int8_t> logical_to_physical;
  bool track_sum_validity           = false;
  bool direct_unmasked_64bit_states = true;
};

struct alignas(16) wide_value {
  uint64_t lo;
  uint64_t hi;
};

struct alignas(16) partial_state {
  wide_value value;
  uint32_t valid;
  uint32_t padding;
};

static_assert(sizeof(wide_value) == 16);

template <cudaDeviceAttr Attr>
[[nodiscard]] int device_attribute()
{
  int device = 0;
  CUDF_CUDA_TRY(cudaGetDevice(&device));
  thread_local int cached_device = -1;
  thread_local int cached_value  = 0;
  if (cached_device != device) {
    int value = 0;
    CUDF_CUDA_TRY(cudaDeviceGetAttribute(&value, Attr, device));
    cached_device = device;
    cached_value  = value;
  }
  return cached_value;
}

template <typename Kernel>
[[nodiscard]] unsigned resident_grid_for(int64_t rows, Kernel kernel, std::size_t dynamic_smem = 0)
{
  auto const row_blocks =
    static_cast<unsigned>(std::max<int64_t>((rows + k_block_size - 1) / k_block_size, int64_t{1}));
  auto const sms = device_attribute<cudaDevAttrMultiProcessorCount>();
  int blocks_per_sm{0};
  CUDF_CUDA_TRY(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
    &blocks_per_sm, kernel, k_block_size, dynamic_smem));
  CUDF_EXPECTS(sms > 0 && blocks_per_sm > 0,
               "CUDA reported no resident blocks for the tiny-domain aggregate kernel");
  return std::min(row_blocks, static_cast<unsigned>(sms * blocks_per_sm));
}

__device__ __forceinline__ bool row_is_valid(cudf::bitmask_type const* mask,
                                             cudf::size_type offset,
                                             int64_t row)
{
  return mask == nullptr || cudf::bit_is_set(mask, offset + static_cast<cudf::size_type>(row));
}

__device__ __forceinline__ int key_token(key_spec const& spec, int64_t row, int32_t* status)
{
  if (!row_is_valid(spec.null_mask, spec.null_mask_offset, row)) { return k_null_key_token; }
  switch (spec.kind) {
    case key_kind::STRING: {
      auto const i     = spec.row_offset + static_cast<cudf::size_type>(row);
      auto const begin = spec.offsets[i];
      auto const end   = spec.offsets[i + 1];
      if (end - begin != 1) {
        atomicOr(status, k_status_bad_key);
        return -1;
      }
      return static_cast<unsigned char>(spec.chars[begin]);
    }
    case key_kind::INT8:
      return static_cast<unsigned char>(static_cast<int8_t const*>(spec.data)[row]);
    case key_kind::UINT8: return static_cast<uint8_t const*>(spec.data)[row];
  }
  atomicOr(status, k_status_bad_key);
  return -1;
}

__device__ __forceinline__ int packed_key(key_spec const* specs,
                                          int num_keys,
                                          int64_t row,
                                          int32_t* status)
{
  auto const first = key_token(specs[0], row, status);
  if (first < 0) { return -1; }
  if (num_keys == 1) { return first; }
  auto const second = key_token(specs[1], row, status);
  return second < 0 ? -1 : first * k_key_radix + second;
}

__global__ void preflight_keys_kernel(
  key_spec const* specs, int num_keys, int64_t rows, int32_t* representatives, int32_t* status)
{
  auto const stride = static_cast<int64_t>(gridDim.x) * blockDim.x;
  for (int64_t row = static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x; row < rows;
       row += stride) {
    auto const packed = packed_key(specs, num_keys, row, status);
    if (packed >= 0) { atomicCAS(representatives + packed, -1, static_cast<int32_t>(row)); }
  }
}

__device__ __forceinline__ wide_value from_signed(int64_t value)
{
  return wide_value{static_cast<uint64_t>(value), value < 0 ? ~uint64_t{0} : uint64_t{0}};
}

__device__ __forceinline__ wide_value from_unsigned(uint64_t value) { return wide_value{value, 0}; }

__device__ __forceinline__ bool add_wide(wide_value lhs,
                                         wide_value rhs,
                                         bool is_unsigned,
                                         wide_value& out)
{
  out.lo                = lhs.lo + rhs.lo;
  auto const carry      = out.lo < lhs.lo ? uint64_t{1} : uint64_t{0};
  auto const high_sum   = lhs.hi + rhs.hi;
  auto const high_wrap  = high_sum < lhs.hi;
  out.hi                = high_sum + carry;
  auto const carry_wrap = out.hi < high_sum;
  if (is_unsigned) { return high_wrap || carry_wrap; }
  auto const lhs_negative = (lhs.hi >> 63) != 0;
  auto const rhs_negative = (rhs.hi >> 63) != 0;
  auto const out_negative = (out.hi >> 63) != 0;
  return lhs_negative == rhs_negative && lhs_negative != out_negative;
}

__device__ __forceinline__ bool less_wide(wide_value lhs, wide_value rhs, bool is_unsigned)
{
  if (lhs.hi != rhs.hi) {
    return is_unsigned ? lhs.hi < rhs.hi
                       : static_cast<int64_t>(lhs.hi) < static_cast<int64_t>(rhs.hi);
  }
  return lhs.lo < rhs.lo;
}

__device__ __forceinline__ wide_value load_value(aggregate_spec const& spec, int64_t row)
{
  auto const i = spec.input_offset + static_cast<cudf::size_type>(row);
  switch (static_cast<cudf::type_id>(spec.input_type)) {
    case cudf::type_id::INT8: return from_signed(static_cast<int8_t const*>(spec.input_data)[i]);
    case cudf::type_id::INT16: return from_signed(static_cast<int16_t const*>(spec.input_data)[i]);
    case cudf::type_id::INT32:
    case cudf::type_id::DECIMAL32:
      return from_signed(static_cast<int32_t const*>(spec.input_data)[i]);
    case cudf::type_id::INT64:
    case cudf::type_id::DECIMAL64:
      return from_signed(static_cast<int64_t const*>(spec.input_data)[i]);
    case cudf::type_id::UINT8:
      return from_unsigned(static_cast<uint8_t const*>(spec.input_data)[i]);
    case cudf::type_id::UINT16:
      return from_unsigned(static_cast<uint16_t const*>(spec.input_data)[i]);
    case cudf::type_id::UINT32:
      return from_unsigned(static_cast<uint32_t const*>(spec.input_data)[i]);
    case cudf::type_id::UINT64:
      return from_unsigned(static_cast<uint64_t const*>(spec.input_data)[i]);
    case cudf::type_id::DECIMAL128: return static_cast<wide_value const*>(spec.input_data)[i];
    default: return wide_value{};
  }
}

__device__ __forceinline__ bool combine_value(int32_t kind,
                                              bool is_unsigned,
                                              wide_value incoming,
                                              wide_value& aggregate)
{
  switch (static_cast<cudf::aggregation::Kind>(kind)) {
    case cudf::aggregation::Kind::SUM:
    case cudf::aggregation::Kind::COUNT_ALL:
    case cudf::aggregation::Kind::COUNT_VALID: {
      wide_value result{};
      auto const overflow = add_wide(aggregate, incoming, is_unsigned, result);
      aggregate           = result;
      return overflow;
    }
    case cudf::aggregation::Kind::MIN:
      if (less_wide(incoming, aggregate, is_unsigned)) { aggregate = incoming; }
      return false;
    case cudf::aggregation::Kind::MAX:
      if (less_wide(aggregate, incoming, is_unsigned)) { aggregate = incoming; }
      return false;
    default: return true;
  }
}

__device__ __forceinline__ bool fits_signed(wide_value value, int bits)
{
  if (bits == 128) { return true; }
  auto const sign     = (value.lo >> (bits - 1)) & uint64_t{1};
  auto const low_mask = bits == 64 ? ~uint64_t{0} : ((uint64_t{1} << bits) - 1);
  if (sign == 0) { return value.hi == 0 && (value.lo & ~low_mask) == 0; }
  return value.hi == ~uint64_t{0} && (value.lo & ~low_mask) == ~low_mask;
}

__device__ __forceinline__ bool fits_unsigned(wide_value value, int bits)
{
  if (value.hi != 0) { return false; }
  return bits == 64 || (value.lo >> bits) == 0;
}

__device__ __forceinline__ bool output_value_fits(aggregate_spec const& spec, wide_value value)
{
  switch (static_cast<cudf::type_id>(spec.output_type)) {
    case cudf::type_id::INT8: return fits_signed(value, 8);
    case cudf::type_id::INT16: return fits_signed(value, 16);
    case cudf::type_id::INT32:
    case cudf::type_id::DECIMAL32: return fits_signed(value, 32);
    case cudf::type_id::INT64:
    case cudf::type_id::DECIMAL64: return fits_signed(value, 64);
    case cudf::type_id::DECIMAL128: return true;
    case cudf::type_id::UINT8: return fits_unsigned(value, 8);
    case cudf::type_id::UINT16: return fits_unsigned(value, 16);
    case cudf::type_id::UINT32: return fits_unsigned(value, 32);
    case cudf::type_id::UINT64: return fits_unsigned(value, 64);
    default: return false;
  }
}

__device__ __forceinline__ void write_value(aggregate_spec const& spec, int group, wide_value value)
{
  switch (static_cast<cudf::type_id>(spec.output_type)) {
    case cudf::type_id::INT8:
      static_cast<int8_t*>(spec.output_data)[group] = static_cast<int8_t>(value.lo);
      break;
    case cudf::type_id::INT16:
      static_cast<int16_t*>(spec.output_data)[group] = static_cast<int16_t>(value.lo);
      break;
    case cudf::type_id::INT32:
    case cudf::type_id::DECIMAL32:
      static_cast<int32_t*>(spec.output_data)[group] = static_cast<int32_t>(value.lo);
      break;
    case cudf::type_id::INT64:
    case cudf::type_id::DECIMAL64:
      static_cast<int64_t*>(spec.output_data)[group] = static_cast<int64_t>(value.lo);
      break;
    case cudf::type_id::UINT8:
      static_cast<uint8_t*>(spec.output_data)[group] = static_cast<uint8_t>(value.lo);
      break;
    case cudf::type_id::UINT16:
      static_cast<uint16_t*>(spec.output_data)[group] = static_cast<uint16_t>(value.lo);
      break;
    case cudf::type_id::UINT32:
      static_cast<uint32_t*>(spec.output_data)[group] = static_cast<uint32_t>(value.lo);
      break;
    case cudf::type_id::UINT64: static_cast<uint64_t*>(spec.output_data)[group] = value.lo; break;
    case cudf::type_id::DECIMAL128:
      static_cast<wide_value*>(spec.output_data)[group] = value;
      break;
    default: break;
  }
}

__device__ __forceinline__ void clear_validity(cudf::bitmask_type* mask, int row)
{
  constexpr auto bits_per_word = sizeof(cudf::bitmask_type) * 8;
  auto const word              = static_cast<unsigned>(row) / bits_per_word;
  auto const bit               = static_cast<unsigned>(row) % bits_per_word;
  atomicAnd(mask + word, ~(cudf::bitmask_type{1} << bit));
}

__device__ __forceinline__ bool checked_add_int64(int64_t& target, int64_t incoming)
{
  if (incoming > 0 && target > std::numeric_limits<int64_t>::max() - incoming) { return false; }
  if (incoming < 0 && target < std::numeric_limits<int64_t>::min() - incoming) { return false; }
  target += incoming;
  return true;
}

__device__ __forceinline__ bool load_bounded_register_value(bounded_register_state_spec const& spec,
                                                            int64_t row,
                                                            int64_t& value)
{
  auto const i = spec.input_offset + static_cast<cudf::size_type>(row);
  switch (static_cast<cudf::type_id>(spec.input_type)) {
    case cudf::type_id::INT8: value = static_cast<int8_t const*>(spec.input_data)[i]; return true;
    case cudf::type_id::INT16: value = static_cast<int16_t const*>(spec.input_data)[i]; return true;
    case cudf::type_id::INT32:
    case cudf::type_id::DECIMAL32:
      value = static_cast<int32_t const*>(spec.input_data)[i];
      return true;
    case cudf::type_id::INT64:
    case cudf::type_id::DECIMAL64:
      value = static_cast<int64_t const*>(spec.input_data)[i];
      return true;
    case cudf::type_id::UINT8: value = static_cast<uint8_t const*>(spec.input_data)[i]; return true;
    case cudf::type_id::UINT16:
      value = static_cast<uint16_t const*>(spec.input_data)[i];
      return true;
    case cudf::type_id::UINT32:
      value = static_cast<uint32_t const*>(spec.input_data)[i];
      return true;
    default: return false;
  }
}

template <int GroupCapacity, int NumStates, bool TrackSumValidity, bool Direct64BitSums>
__global__ __launch_bounds__(
  k_block_size,
  Direct64BitSums && GroupCapacity * NumStates <= k_three_block_codegen_entry_limit
    ? 3
    : 1) void accumulate_bounded_register_kernel(key_spec const* key_specs,
                                                 int num_keys,
                                                 int64_t rows,
                                                 int16_t const* dense_map,
                                                 bounded_register_state_spec const* state_specs,
                                                 int num_groups,
                                                 int64_t* thread_partials,
                                                 uint32_t* thread_validity,
                                                 int32_t* status,
                                                 bool prefix_preflight)
{
  static_assert(GroupCapacity * NumStates <= k_max_bounded_register_entries);
  __shared__ bounded_register_state_spec shared_specs[NumStates];
  if (threadIdx.x < NumStates) { shared_specs[threadIdx.x] = state_specs[threadIdx.x]; }
  __syncthreads();

  int64_t accumulators[GroupCapacity * NumStates]{};
  uint32_t valid_bits      = 0;
  bool register_overflow   = false;
  bool bad_state           = false;
  bool late_key            = false;
  auto const thread        = static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  auto const total_threads = static_cast<int64_t>(gridDim.x) * blockDim.x;
  for (int64_t row = thread; row < rows; row += total_threads) {
    auto const packed = packed_key(key_specs, num_keys, row, status);
    if (packed < 0) { continue; }
    auto const group = static_cast<int>(dense_map[packed]);
    if (group < 0) {
      late_key |= prefix_preflight;
      bad_state |= !prefix_preflight;
      continue;
    }
    if (group >= num_groups || group >= GroupCapacity) {
      bad_state = true;
      continue;
    }

#pragma unroll
    for (int candidate_group = 0; candidate_group < GroupCapacity; ++candidate_group) {
      if (group != candidate_group) { continue; }
#pragma unroll
      for (int state = 0; state < NumStates; ++state) {
        auto const& spec = shared_specs[state];
        auto const entry = state * GroupCapacity + candidate_group;
        if constexpr (Direct64BitSums && !TrackSumValidity) {
          auto const i        = spec.input_offset + static_cast<cudf::size_type>(row);
          auto const incoming = spec.input_data == nullptr
                                  ? int64_t{1}
                                  : static_cast<int64_t const*>(spec.input_data)[i];
          register_overflow |= !checked_add_int64(accumulators[entry], incoming);
        } else {
          auto const kind  = static_cast<cudf::aggregation::Kind>(spec.kind);
          bool value_valid = true;
          bool accumulate  = true;
          int64_t incoming = 0;
          if (kind == cudf::aggregation::Kind::SUM) {
            if constexpr (TrackSumValidity) {
              value_valid = row_is_valid(spec.input_null_mask, spec.input_null_mask_offset, row);
            }
            accumulate = value_valid;
            if (value_valid) {
              if constexpr (Direct64BitSums) {
                auto const i = spec.input_offset + static_cast<cudf::size_type>(row);
                incoming     = static_cast<int64_t const*>(spec.input_data)[i];
              } else if (!load_bounded_register_value(spec, row, incoming)) {
                bad_state  = true;
                accumulate = false;
              }
            }
          } else if (kind == cudf::aggregation::Kind::COUNT_ALL) {
            incoming = 1;
          } else if (kind == cudf::aggregation::Kind::COUNT_VALID) {
            value_valid = true;
            accumulate  = row_is_valid(spec.input_null_mask, spec.input_null_mask_offset, row);
            incoming    = 1;
          } else {
            bad_state   = true;
            value_valid = false;
            accumulate  = false;
          }

          if constexpr (TrackSumValidity) {
            if (value_valid) { valid_bits |= uint32_t{1} << entry; }
          }
          if (accumulate) {
            register_overflow |= !checked_add_int64(accumulators[entry], incoming);
          }
        }
      }
    }
  }

  if (register_overflow) { atomicOr(status, k_status_bounded_register_overflow); }
  if (bad_state) { atomicOr(status, k_status_bad_state); }
  if (late_key) { atomicOr(status, k_status_late_key); }

#pragma unroll
  for (int state = 0; state < NumStates; ++state) {
#pragma unroll
    for (int group = 0; group < GroupCapacity; ++group) {
      if (group >= num_groups) { continue; }
      auto const local_entry    = state * GroupCapacity + group;
      auto const physical_entry = state * num_groups + group;
      auto const output         = static_cast<int64_t>(physical_entry) * total_threads + thread;
      thread_partials[output]   = accumulators[local_entry];
      if constexpr (TrackSumValidity) {
        thread_validity[output] = (valid_bits >> local_entry) & uint32_t{1};
      }
    }
  }
}

__global__ void reduce_bounded_register_partials_kernel(int64_t const* thread_partials,
                                                        uint32_t const* thread_validity,
                                                        int num_partial_threads,
                                                        int num_physical_entries,
                                                        wide_value* physical_results,
                                                        uint32_t* physical_validity,
                                                        int32_t* status)
{
  auto const physical_entry = static_cast<int>(blockIdx.x);
  if (physical_entry >= num_physical_entries) { return; }

  wide_value local{};
  bool local_valid = false;
  bool overflow    = false;
  for (int thread = static_cast<int>(threadIdx.x); thread < num_partial_threads;
       thread += blockDim.x) {
    auto const offset = physical_entry * num_partial_threads + thread;
    if (thread_validity != nullptr && thread_validity[offset] == 0) { continue; }
    auto const incoming = from_signed(thread_partials[offset]);
    if (!local_valid) {
      local       = incoming;
      local_valid = true;
    } else {
      wide_value next{};
      overflow |= add_wide(local, incoming, false, next);
      local = next;
    }
  }
  if (overflow) { atomicOr(status, k_status_overflow); }

  __shared__ wide_value shared_values[k_block_size];
  __shared__ uint32_t shared_validity[k_block_size];
  shared_values[threadIdx.x]   = local;
  shared_validity[threadIdx.x] = local_valid ? uint32_t{1} : uint32_t{0};
  __syncthreads();

  for (int stride = k_block_size / 2; stride > 0; stride /= 2) {
    if (threadIdx.x < stride && shared_validity[threadIdx.x + stride] != 0) {
      if (shared_validity[threadIdx.x] == 0) {
        shared_values[threadIdx.x]   = shared_values[threadIdx.x + stride];
        shared_validity[threadIdx.x] = 1;
      } else {
        wide_value next{};
        if (add_wide(
              shared_values[threadIdx.x], shared_values[threadIdx.x + stride], false, next)) {
          atomicOr(status, k_status_overflow);
        }
        shared_values[threadIdx.x] = next;
      }
    }
    __syncthreads();
  }
  if (threadIdx.x == 0) {
    physical_results[physical_entry]  = shared_values[0];
    physical_validity[physical_entry] = shared_validity[0];
  }
}

__global__ void expand_bounded_register_results_kernel(wide_value const* physical_results,
                                                       uint32_t const* physical_validity,
                                                       aggregate_spec const* aggregate_specs,
                                                       int8_t const* logical_to_physical,
                                                       int num_logical_states,
                                                       int num_physical_states,
                                                       int num_groups,
                                                       cudf::size_type* output_null_counts,
                                                       int32_t* status)
{
  if (*status != 0) { return; }
  auto const logical_entries = num_logical_states * num_groups;
  for (int logical_entry = static_cast<int>(blockIdx.x) * blockDim.x + threadIdx.x;
       logical_entry < logical_entries;
       logical_entry += static_cast<int>(gridDim.x) * blockDim.x) {
    auto const logical_state  = logical_entry / num_groups;
    auto const group          = logical_entry % num_groups;
    auto const physical_state = static_cast<int>(logical_to_physical[logical_state]);
    if (physical_state < 0 || physical_state >= num_physical_states) {
      atomicOr(status, k_status_bad_state);
      continue;
    }

    auto const& spec          = aggregate_specs[logical_state];
    auto const physical_entry = physical_state * num_groups + group;
    if (physical_validity[physical_entry] == 0) {
      if (static_cast<cudf::aggregation::Kind>(spec.kind) != cudf::aggregation::Kind::SUM ||
          spec.output_null_mask == nullptr) {
        atomicOr(status, k_status_bad_state);
      } else {
        atomicAdd(output_null_counts + logical_state, cudf::size_type{1});
        clear_validity(spec.output_null_mask, group);
      }
      continue;
    }

    auto const value = physical_results[physical_entry];
    if (!output_value_fits(spec, value)) {
      atomicOr(status, k_status_overflow);
    } else {
      write_value(spec, group, value);
    }
  }
}

struct bounded_register_launch_args {
  key_spec const* key_specs;
  int num_keys;
  int64_t rows;
  int16_t const* dense_map;
  bounded_register_state_spec const* state_specs;
  int num_groups;
  int32_t* status;
  bool prefix_preflight;
  bool track_sum_validity;
  bool direct_unmasked_64bit_states;
};

struct bounded_register_scratch {
  std::unique_ptr<rmm::device_uvector<int64_t>> partials;
  std::unique_ptr<rmm::device_uvector<uint32_t>> validity;
  std::size_t partial_threads = 0;
};

template <int GroupCapacity, int NumStates, bool TrackSumValidity, bool Direct64BitSums>
[[nodiscard]] bool launch_bounded_register_mode(bounded_register_launch_args const& args,
                                                bounded_register_scratch& scratch,
                                                rmm::cuda_stream_view stream,
                                                rmm::device_async_resource_ref mr)
{
  static_assert(GroupCapacity * NumStates <= k_max_bounded_register_entries);
  int blocks_per_sm{0};
  CUDF_CUDA_TRY(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
    &blocks_per_sm,
    accumulate_bounded_register_kernel<GroupCapacity, NumStates, TrackSumValidity, Direct64BitSums>,
    k_block_size,
    0));
  if (blocks_per_sm < k_min_bounded_register_blocks_per_sm) { return false; }
  auto const row_blocks = static_cast<unsigned>(
    std::max<int64_t>((args.rows + k_block_size - 1) / k_block_size, int64_t{1}));
  auto const grid = std::min(
    row_blocks,
    static_cast<unsigned>(device_attribute<cudaDevAttrMultiProcessorCount>() * blocks_per_sm));
  scratch.partial_threads = static_cast<std::size_t>(grid) * k_block_size;
  auto const entries      = static_cast<std::size_t>(args.num_groups) * NumStates;
  scratch.partials =
    std::make_unique<rmm::device_uvector<int64_t>>(scratch.partial_threads * entries, stream, mr);
  if constexpr (TrackSumValidity) {
    scratch.validity = std::make_unique<rmm::device_uvector<uint32_t>>(
      scratch.partial_threads * entries, stream, mr);
  }
  accumulate_bounded_register_kernel<GroupCapacity, NumStates, TrackSumValidity, Direct64BitSums>
    <<<grid, k_block_size, 0, stream.value()>>>(
      args.key_specs,
      args.num_keys,
      args.rows,
      args.dense_map,
      args.state_specs,
      args.num_groups,
      scratch.partials->data(),
      TrackSumValidity ? scratch.validity->data() : nullptr,
      args.status,
      args.prefix_preflight);
  CUDF_CUDA_TRY(cudaGetLastError());
  return true;
}

template <int GroupCapacity, int NumStates>
[[nodiscard]] bool launch_bounded_register_accumulation(bounded_register_launch_args const& args,
                                                        bounded_register_scratch& scratch,
                                                        rmm::cuda_stream_view stream,
                                                        rmm::device_async_resource_ref mr)
{
  if (args.direct_unmasked_64bit_states) {
    return launch_bounded_register_mode<GroupCapacity, NumStates, false, true>(
      args, scratch, stream, mr);
  }
  if (args.track_sum_validity) {
    return launch_bounded_register_mode<GroupCapacity, NumStates, true, false>(
      args, scratch, stream, mr);
  }
  return launch_bounded_register_mode<GroupCapacity, NumStates, false, false>(
    args, scratch, stream, mr);
}

template <int GroupCapacity, int MaxStates>
[[nodiscard]] bool dispatch_bounded_register_states(int num_states,
                                                    bounded_register_launch_args const& args,
                                                    bounded_register_scratch& scratch,
                                                    rmm::cuda_stream_view stream,
                                                    rmm::device_async_resource_ref mr)
{
#define SIRIUS_DISPATCH_STATE_COUNT(N)                                                          \
  case N:                                                                                       \
    if constexpr (N <= MaxStates) {                                                             \
      return launch_bounded_register_accumulation<GroupCapacity, N>(args, scratch, stream, mr); \
    }                                                                                           \
    return false
  switch (num_states) {
    SIRIUS_DISPATCH_STATE_COUNT(1);
    SIRIUS_DISPATCH_STATE_COUNT(2);
    SIRIUS_DISPATCH_STATE_COUNT(3);
    SIRIUS_DISPATCH_STATE_COUNT(4);
    SIRIUS_DISPATCH_STATE_COUNT(5);
    SIRIUS_DISPATCH_STATE_COUNT(6);
    SIRIUS_DISPATCH_STATE_COUNT(7);
    SIRIUS_DISPATCH_STATE_COUNT(8);
    default: return false;
  }
#undef SIRIUS_DISPATCH_STATE_COUNT
}

[[nodiscard]] bool dispatch_bounded_register_accumulation(int num_states,
                                                          bounded_register_launch_args const& args,
                                                          bounded_register_scratch& scratch,
                                                          rmm::cuda_stream_view stream,
                                                          rmm::device_async_resource_ref mr)
{
  if (args.num_groups <= 1) {
    return dispatch_bounded_register_states<1, 8>(num_states, args, scratch, stream, mr);
  }
  if (args.num_groups <= 2) {
    return dispatch_bounded_register_states<2, 8>(num_states, args, scratch, stream, mr);
  }
  if (args.num_groups <= 4) {
    return dispatch_bounded_register_states<4, 8>(num_states, args, scratch, stream, mr);
  }
  if (args.num_groups <= 8) {
    return dispatch_bounded_register_states<8, 4>(num_states, args, scratch, stream, mr);
  }
  if (args.num_groups <= 16) {
    return dispatch_bounded_register_states<16, 2>(num_states, args, scratch, stream, mr);
  }
  if (args.num_groups <= 32) {
    return dispatch_bounded_register_states<32, 1>(num_states, args, scratch, stream, mr);
  }
  return false;
}

__global__ void accumulate_tiny_domain_kernel(key_spec const* key_specs,
                                              int num_keys,
                                              int64_t rows,
                                              int16_t const* dense_map,
                                              aggregate_spec const* aggregate_specs,
                                              int num_aggregates,
                                              int num_groups,
                                              partial_state* block_partials,
                                              int32_t* status)
{
  auto const entries      = num_aggregates * num_groups;
  auto const warp_entries = entries * k_warps_per_block;
  extern __shared__ __align__(16) unsigned char shared_raw[];
  auto* values = reinterpret_cast<wide_value*>(shared_raw);
  auto* valid  = reinterpret_cast<uint32_t*>(values + warp_entries);

  for (int state = threadIdx.x; state < warp_entries; state += blockDim.x) {
    values[state] = wide_value{};
    valid[state]  = 0;
  }
  __syncthreads();

  auto const stride = static_cast<int64_t>(gridDim.x) * blockDim.x;
  for (int64_t row = static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x; row < rows;
       row += stride) {
    auto const active      = __activemask();
    auto const packed      = packed_key(key_specs, num_keys, row, status);
    auto const group       = packed >= 0 ? static_cast<int>(dense_map[packed]) : -1;
    bool const group_valid = group >= 0 && group < num_groups;
    auto const group_lanes = __ballot_sync(active, group_valid);
    if (!group_valid) { atomicOr(status, k_status_bad_state); }
    if (!group_valid) { continue; }

    auto const lane = static_cast<int>(threadIdx.x & 31);
    auto const warp = static_cast<int>(threadIdx.x >> 5);
    for (int aggregate_idx = 0; aggregate_idx < num_aggregates; ++aggregate_idx) {
      auto const& spec = aggregate_specs[aggregate_idx];
      auto const kind  = static_cast<cudf::aggregation::Kind>(spec.kind);
      bool value_valid = true;
      wide_value value{};
      if (kind == cudf::aggregation::Kind::COUNT_ALL) {
        value = from_unsigned(1);
      } else {
        value_valid = row_is_valid(spec.input_null_mask, spec.input_null_mask_offset, row);
        if (kind == cudf::aggregation::Kind::COUNT_VALID) {
          value = from_unsigned(1);
        } else if (value_valid) {
          value = load_value(spec, row);
        }
      }

      auto const valid_lanes = __ballot_sync(group_lanes, value_valid);
      if (value_valid) {
        auto const peers  = __match_any_sync(valid_lanes, group);
        auto const leader = __ffs(peers) - 1;

        wide_value combined{};
        bool combined_valid = false;
        bool overflow       = false;
        auto remaining      = peers;
        while (remaining != 0) {
          auto const source_lane = __ffs(remaining) - 1;
          wide_value incoming{__shfl_sync(peers, value.lo, source_lane),
                              __shfl_sync(peers, value.hi, source_lane)};
          if (lane == leader) {
            if (!combined_valid) {
              combined       = incoming;
              combined_valid = true;
            } else {
              overflow |= combine_value(spec.kind, spec.is_unsigned != 0, incoming, combined);
            }
          }
          remaining &= remaining - 1;
        }

        if (lane == leader) {
          auto const state = warp * entries + aggregate_idx * num_groups + group;
          if (valid[state] == 0) {
            values[state] = combined;
            valid[state]  = 1;
          } else {
            overflow |= combine_value(spec.kind, spec.is_unsigned != 0, combined, values[state]);
          }
          if (overflow) { atomicOr(status, k_status_overflow); }
        }
      }
      __syncwarp(group_lanes);
    }
  }
  __syncthreads();

  auto* const output = block_partials + static_cast<std::size_t>(blockIdx.x) * entries;
  for (int entry = threadIdx.x; entry < entries; entry += blockDim.x) {
    wide_value result{};
    bool result_valid = false;
    bool overflow     = false;
    auto const& spec  = aggregate_specs[entry / num_groups];
    for (int warp = 0; warp < k_warps_per_block; ++warp) {
      auto const state = warp * entries + entry;
      if (valid[state] == 0) { continue; }
      if (!result_valid) {
        result       = values[state];
        result_valid = true;
      } else {
        overflow |= combine_value(spec.kind, spec.is_unsigned != 0, values[state], result);
      }
    }
    if (overflow) { atomicOr(status, k_status_overflow); }
    output[entry] = partial_state{result, result_valid ? uint32_t{1} : uint32_t{0}, 0};
  }
}

__global__ void reduce_tiny_domain_kernel(partial_state const* block_partials,
                                          int num_blocks,
                                          aggregate_spec const* aggregate_specs,
                                          int num_aggregates,
                                          int num_groups,
                                          cudf::size_type* output_null_counts,
                                          int32_t* status)
{
  auto const entries = num_aggregates * num_groups;
  for (int entry = static_cast<int>(blockIdx.x) * blockDim.x + threadIdx.x; entry < entries;
       entry += static_cast<int>(gridDim.x) * blockDim.x) {
    auto const aggregate_idx = entry / num_groups;
    auto const group         = entry % num_groups;
    auto const& spec         = aggregate_specs[aggregate_idx];
    wide_value result{};
    bool result_valid = false;
    bool overflow     = false;
    for (int block = 0; block < num_blocks; ++block) {
      auto const& partial = block_partials[static_cast<std::size_t>(block) * entries + entry];
      if (partial.valid == 0) { continue; }
      if (!result_valid) {
        result       = partial.value;
        result_valid = true;
      } else {
        overflow |= combine_value(spec.kind, spec.is_unsigned != 0, partial.value, result);
      }
    }

    if (!result_valid) {
      auto const kind = static_cast<cudf::aggregation::Kind>(spec.kind);
      if (kind == cudf::aggregation::Kind::COUNT_VALID) {
        result_valid = true;  // an occupied group with only NULL values has exact count zero
      } else if (spec.output_null_mask == nullptr) {
        atomicOr(status, k_status_bad_state);
        continue;
      } else {
        atomicAdd(output_null_counts + aggregate_idx, cudf::size_type{1});
        clear_validity(spec.output_null_mask, group);
        continue;
      }
    }
    if (!output_value_fits(spec, result)) { overflow = true; }
    if (overflow) {
      atomicOr(status, k_status_overflow);
    } else {
      write_value(spec, group, result);
    }
  }
}

[[nodiscard]] bool supported_key(cudf::column_view const& column)
{
  auto const id = column.type().id();
  return id == cudf::type_id::STRING || id == cudf::type_id::INT8 || id == cudf::type_id::UINT8;
}

[[nodiscard]] bool supported_value_type(cudf::type_id id)
{
  switch (id) {
    case cudf::type_id::INT8:
    case cudf::type_id::INT16:
    case cudf::type_id::INT32:
    case cudf::type_id::INT64:
    case cudf::type_id::UINT8:
    case cudf::type_id::UINT16:
    case cudf::type_id::UINT32:
    case cudf::type_id::UINT64:
    case cudf::type_id::DECIMAL32:
    case cudf::type_id::DECIMAL64:
    case cudf::type_id::DECIMAL128: return true;
    default: return false;
  }
}

[[nodiscard]] bool supported_sum_type(cudf::type_id id)
{
  switch (id) {
    case cudf::type_id::INT8:
    case cudf::type_id::INT16:
    case cudf::type_id::INT32:
    case cudf::type_id::UINT8:
    case cudf::type_id::UINT16:
    case cudf::type_id::UINT32:
    case cudf::type_id::DECIMAL32:
    case cudf::type_id::DECIMAL64: return true;
    default: return false;
  }
}

[[nodiscard]] bool supported_aggregate(cudf::aggregation::Kind kind)
{
  switch (kind) {
    case cudf::aggregation::Kind::SUM:
    case cudf::aggregation::Kind::COUNT_ALL:
    case cudf::aggregation::Kind::COUNT_VALID:
    case cudf::aggregation::Kind::MIN:
    case cudf::aggregation::Kind::MAX: return true;
    default: return false;
  }
}

[[nodiscard]] cudf::data_type aggregate_output_type(cudf::aggregation::Kind kind,
                                                    cudf::data_type input,
                                                    bool isolated_request)
{
  if (kind == cudf::aggregation::Kind::COUNT_ALL || kind == cudf::aggregation::Kind::COUNT_VALID) {
    return cudf::data_type{isolated_request ? cudf::type_id::INT64 : cudf::type_id::INT32};
  }
  if (kind != cudf::aggregation::Kind::SUM) { return input; }
  switch (input.id()) {
    case cudf::type_id::INT8:
    case cudf::type_id::INT16:
    case cudf::type_id::INT32:
    case cudf::type_id::UINT8:
    case cudf::type_id::UINT16:
    case cudf::type_id::UINT32:
      // cuDF grouped SUM uses a signed INT64 target for every smaller integral source.
      return cudf::data_type{cudf::type_id::INT64};
    case cudf::type_id::DECIMAL32: return cudf::data_type{cudf::type_id::DECIMAL64, input.scale()};
    case cudf::type_id::DECIMAL64: return cudf::data_type{cudf::type_id::DECIMAL128, input.scale()};
    default: return input;
  }
}

[[nodiscard]] bool is_unsigned(cudf::type_id id)
{
  return id == cudf::type_id::UINT8 || id == cudf::type_id::UINT16 || id == cudf::type_id::UINT32 ||
         id == cudf::type_id::UINT64;
}

[[nodiscard]] key_spec make_key_spec(cudf::column_view const& column, rmm::cuda_stream_view stream)
{
  auto const mask = column.nullable() ? column.null_mask() : nullptr;
  switch (column.type().id()) {
    case cudf::type_id::STRING: {
      cudf::strings_column_view strings(column);
      auto const offsets = strings.offsets();
      return key_spec{key_kind::STRING,
                      nullptr,
                      offsets.head<cudf::size_type>(),
                      strings.chars_begin(stream),
                      mask,
                      column.offset(),
                      column.offset()};
    }
    case cudf::type_id::INT8:
      return key_spec{
        key_kind::INT8, column.data<int8_t>(), nullptr, nullptr, mask, column.offset(), 0};
    case cudf::type_id::UINT8:
      return key_spec{
        key_kind::UINT8, column.data<uint8_t>(), nullptr, nullptr, mask, column.offset(), 0};
    default: return {};
  }
}

[[nodiscard]] bool same_input_source(aggregate_spec const& lhs, aggregate_spec const& rhs)
{
  return lhs.input_type == rhs.input_type && lhs.is_unsigned == rhs.is_unsigned &&
         lhs.input_data == rhs.input_data && lhs.input_offset == rhs.input_offset &&
         lhs.input_null_mask == rhs.input_null_mask &&
         lhs.input_null_mask_offset == rhs.input_null_mask_offset;
}

[[nodiscard]] bool same_sum_state(aggregate_spec const& lhs,
                                  aggregate_spec const& rhs,
                                  cudf::data_type lhs_type,
                                  cudf::data_type rhs_type)
{
  return lhs_type == rhs_type && same_input_source(lhs, rhs) && lhs.kind == rhs.kind &&
         lhs.output_type == rhs.output_type;
}

[[nodiscard]] bounded_register_state_spec make_bounded_register_state(aggregate_spec const& spec)
{
  return bounded_register_state_spec{spec.kind,
                                     spec.input_type,
                                     spec.input_data,
                                     spec.input_offset,
                                     spec.input_null_mask,
                                     spec.input_null_mask_offset};
}

[[nodiscard]] std::optional<bounded_register_plan> make_bounded_register_plan(
  cudf::table_view input,
  std::vector<cudf::aggregation::Kind> const& aggregates,
  std::vector<int> const& aggregate_idx,
  std::vector<aggregate_spec> const& specs)
{
  if (aggregates.empty() || aggregates.size() != aggregate_idx.size() ||
      aggregates.size() != specs.size()) {
    return std::nullopt;
  }

  bounded_register_plan plan{};
  plan.logical_to_physical.assign(aggregates.size(), int8_t{-1});
  std::vector<int> representative_logicals;
  representative_logicals.reserve(k_max_bounded_register_states);

  auto add_state = [&](bounded_register_state_spec state, int representative_logical) {
    if (plan.states.size() == k_max_bounded_register_states) { return -1; }
    auto const physical = static_cast<int>(plan.states.size());
    plan.states.push_back(state);
    representative_logicals.push_back(representative_logical);
    return physical;
  };
  auto count_all_state = [&]() {
    for (std::size_t physical = 0; physical < plan.states.size(); ++physical) {
      if (static_cast<cudf::aggregation::Kind>(plan.states[physical].kind) ==
          cudf::aggregation::Kind::COUNT_ALL) {
        return static_cast<int>(physical);
      }
    }
    aggregate_spec state{};
    state.kind       = static_cast<int32_t>(cudf::aggregation::Kind::COUNT_ALL);
    state.input_type = static_cast<int32_t>(cudf::type_id::INT8);
    return add_state(make_bounded_register_state(state), -1);
  };

  for (std::size_t logical = 0; logical < aggregates.size(); ++logical) {
    auto const kind  = aggregates[logical];
    auto const& spec = specs[logical];
    int physical     = -1;
    if (kind == cudf::aggregation::Kind::SUM) {
      auto const source      = aggregate_idx[logical];
      auto const source_type = input.column(source).type();
      plan.direct_unmasked_64bit_states &=
        source_type.id() == cudf::type_id::INT64 || source_type.id() == cudf::type_id::DECIMAL64;
      for (std::size_t candidate = 0; candidate < plan.states.size(); ++candidate) {
        auto const representative = representative_logicals[candidate];
        if (representative < 0 || static_cast<cudf::aggregation::Kind>(
                                    plan.states[candidate].kind) != cudf::aggregation::Kind::SUM) {
          continue;
        }
        auto const other_source = aggregate_idx[static_cast<std::size_t>(representative)];
        if (same_sum_state(spec,
                           specs[static_cast<std::size_t>(representative)],
                           source_type,
                           input.column(other_source).type())) {
          physical = static_cast<int>(candidate);
          break;
        }
      }
      if (physical < 0) {
        auto state = make_bounded_register_state(spec);
        if (input.column(source).null_count() == 0) {
          state.input_null_mask        = nullptr;
          state.input_null_mask_offset = 0;
        } else {
          plan.track_sum_validity = true;
        }
        physical = add_state(state, static_cast<int>(logical));
      }
    } else if (kind == cudf::aggregation::Kind::COUNT_ALL) {
      physical = count_all_state();
    } else if (kind == cudf::aggregation::Kind::COUNT_VALID) {
      auto const column = input.column(aggregate_idx[logical]);
      if (!column.nullable() || column.null_count() == 0) {
        physical = count_all_state();
      } else {
        for (std::size_t candidate = 0; candidate < plan.states.size(); ++candidate) {
          auto const& state = plan.states[candidate];
          if (static_cast<cudf::aggregation::Kind>(state.kind) ==
                cudf::aggregation::Kind::COUNT_VALID &&
              state.input_null_mask == spec.input_null_mask &&
              state.input_null_mask_offset == spec.input_null_mask_offset) {
            physical = static_cast<int>(candidate);
            break;
          }
        }
        if (physical < 0) {
          auto state         = make_bounded_register_state(spec);
          state.input_type   = static_cast<int32_t>(cudf::type_id::INT8);
          state.input_data   = nullptr;
          state.input_offset = 0;
          physical           = add_state(state, static_cast<int>(logical));
        }
      }
    } else {
      return std::nullopt;
    }

    if (physical < 0) { return std::nullopt; }
    plan.logical_to_physical[logical] = static_cast<int8_t>(physical);
  }
  plan.direct_unmasked_64bit_states &=
    std::all_of(plan.states.begin(), plan.states.end(), [](auto const& state) {
      return state.input_null_mask == nullptr;
    });
  return plan;
}

[[nodiscard]] bool bounded_register_shape_fits(int num_groups, int num_states)
{
  if (num_groups <= 0 || num_groups > k_max_bounded_register_groups || num_states <= 0 ||
      num_states > k_max_bounded_register_states) {
    return false;
  }
  auto const group_capacity = num_groups <= 1    ? 1
                              : num_groups <= 2  ? 2
                              : num_groups <= 4  ? 4
                              : num_groups <= 8  ? 8
                              : num_groups <= 16 ? 16
                                                 : 32;
  return group_capacity * num_states <= k_max_bounded_register_entries;
}

[[nodiscard]] std::string status_reason(int status)
{
  if ((status & k_status_bad_state) != 0) {
    return "tiny-domain kernel reported an invalid internal state";
  }
  if ((status & k_status_overflow) != 0) {
    return "aggregate arithmetic exceeded its exact carrier";
  }
  if ((status & k_status_bad_key) != 0) { return "a non-null STRING key is not exactly one byte"; }
  if ((status & k_status_late_key) != 0) {
    return "prefix preflight missed a grouping key outside its sampled rows";
  }
  if ((status & k_status_bounded_register_overflow) != 0) {
    return "bounded register partial exceeded its INT64 carrier";
  }
  return "tiny-domain kernel reported an invalid internal state";
}

}  // namespace

tiny_domain_grouped_aggregate_attempt try_tiny_domain_grouped_aggregate(
  cudf::table_view input,
  std::vector<int> const& group_idx,
  std::vector<cudf::aggregation::Kind> const& aggregates,
  std::vector<int> const& aggregate_idx,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr)
{
  auto fallback = [](std::string reason) {
    return tiny_domain_grouped_aggregate_attempt{nullptr, std::move(reason), 0};
  };
  if (group_idx.empty() || group_idx.size() > 2) {
    return fallback("requires one or two grouping keys");
  }
  if (aggregates.empty() || aggregates.size() != aggregate_idx.size()) {
    return fallback("aggregate definition vectors are empty or inconsistent");
  }
  if (input.num_rows() == 0) { return fallback("empty input uses the generic schema path"); }
  if (input.num_rows() > std::numeric_limits<int32_t>::max()) {
    return fallback("input row count exceeds the representative-row carrier");
  }

  std::vector<cudf::column_view> group_columns;
  std::vector<key_spec> host_key_specs;
  group_columns.reserve(group_idx.size());
  host_key_specs.reserve(group_idx.size());
  for (auto const index : group_idx) {
    if (index < 0 || index >= input.num_columns()) {
      return fallback("grouping-key index is outside the input schema");
    }
    auto const column = input.column(index);
    if (!supported_key(column)) { return fallback("grouping key is not STRING, INT8, or UINT8"); }
    group_columns.push_back(column);
    host_key_specs.push_back(make_key_spec(column, stream));
  }

  // Match the generic grouped-aggregate local-state schema exactly. cuDF produces INT32 counts;
  // the legacy path widens only a request containing a single COUNT. Aggregates sharing an input
  // column are combined into one request, including COUNT(*) column-zero placeholder.
  std::vector<std::size_t> request_multiplicity(static_cast<std::size_t>(input.num_columns()), 0);
  for (auto const index : aggregate_idx) {
    if (index < 0 || index >= input.num_columns()) {
      return fallback("aggregate input index is outside the input schema");
    }
    ++request_multiplicity[static_cast<std::size_t>(index)];
  }
  std::vector<cudf::data_type> output_types;
  output_types.reserve(aggregates.size());
  for (std::size_t i = 0; i < aggregates.size(); ++i) {
    auto const kind = aggregates[i];
    if (!supported_aggregate(kind)) { return fallback("aggregate kind is not bounded"); }
    auto const index      = aggregate_idx[i];
    auto const input_type = kind == cudf::aggregation::Kind::COUNT_ALL
                              ? cudf::data_type{cudf::type_id::INT8}
                              : input.column(index).type();
    if (kind != cudf::aggregation::Kind::COUNT_VALID && !supported_value_type(input_type.id())) {
      return fallback("aggregate value carrier is unsupported");
    }
    if (kind == cudf::aggregation::Kind::SUM && !supported_sum_type(input_type.id())) {
      return fallback("SUM input carrier is not overflow-safe under the bounded row count");
    }
    output_types.push_back(aggregate_output_type(
      kind, input_type, request_multiplicity[static_cast<std::size_t>(index)] == 1));
  }

  auto make_host_aggregate_spec = [&](std::size_t i,
                                      void* output_data,
                                      cudf::bitmask_type* output_null_mask) {
    auto const kind = aggregates[i];
    cudf::column_view input_column;
    if (kind != cudf::aggregation::Kind::COUNT_ALL) {
      input_column = input.column(aggregate_idx[i]);
    }
    return aggregate_spec{
      static_cast<int32_t>(kind),
      static_cast<int32_t>(kind == cudf::aggregation::Kind::COUNT_ALL ? cudf::type_id::INT8
                                                                      : input_column.type().id()),
      static_cast<int32_t>(output_types[i].id()),
      static_cast<int32_t>(
        kind == cudf::aggregation::Kind::COUNT_ALL ? false : is_unsigned(input_column.type().id())),
      kind == cudf::aggregation::Kind::COUNT_ALL ? nullptr : input_column.head(),
      kind == cudf::aggregation::Kind::COUNT_ALL ? 0 : input_column.offset(),
      kind == cudf::aggregation::Kind::COUNT_ALL || !input_column.nullable()
        ? nullptr
        : input_column.null_mask(),
      kind == cudf::aggregation::Kind::COUNT_ALL ? 0 : input_column.offset(),
      output_data,
      output_null_mask};
  };

  std::vector<aggregate_spec> planning_specs;
  planning_specs.reserve(aggregates.size());
  for (std::size_t i = 0; i < aggregates.size(); ++i) {
    planning_specs.push_back(make_host_aggregate_spec(i, nullptr, nullptr));
  }
  auto bounded_plan = make_bounded_register_plan(input, aggregates, aggregate_idx, planning_specs);

  auto const domain = group_idx.size() == 1 ? k_key_radix : k_max_key_domain;
  rmm::device_uvector<key_spec> device_key_specs(host_key_specs.size(), stream, mr);
  rmm::device_uvector<int32_t> representatives(domain, stream, mr);
  rmm::device_uvector<int32_t> device_status(1, stream, mr);
  CUDF_CUDA_TRY(cudaMemcpyAsync(device_key_specs.data(),
                                host_key_specs.data(),
                                host_key_specs.size() * sizeof(key_spec),
                                cudaMemcpyHostToDevice,
                                stream.value()));

  std::vector<int32_t> host_representatives(domain);
  int32_t host_status{0};
  auto run_preflight = [&](int64_t rows) {
    CUDF_CUDA_TRY(cudaMemsetAsync(
      representatives.data(), 0xff, representatives.size() * sizeof(int32_t), stream.value()));
    CUDF_CUDA_TRY(cudaMemsetAsync(device_status.data(), 0, sizeof(int32_t), stream.value()));
    auto const preflight_grid = resident_grid_for(rows, preflight_keys_kernel);
    preflight_keys_kernel<<<preflight_grid, k_block_size, 0, stream.value()>>>(
      device_key_specs.data(),
      static_cast<int>(group_idx.size()),
      rows,
      representatives.data(),
      device_status.data());
    CUDF_CUDA_TRY(cudaGetLastError());
    CUDF_CUDA_TRY(cudaMemcpyAsync(host_representatives.data(),
                                  representatives.data(),
                                  host_representatives.size() * sizeof(int32_t),
                                  cudaMemcpyDeviceToHost,
                                  stream.value()));
    CUDF_CUDA_TRY(cudaMemcpyAsync(&host_status,
                                  device_status.data(),
                                  sizeof(host_status),
                                  cudaMemcpyDeviceToHost,
                                  stream.value()));
    stream.synchronize();
  };

  bool const prefix_is_candidate =
    bounded_plan.has_value() && input.num_rows() > k_preflight_sample_rows;
  run_preflight(prefix_is_candidate ? k_preflight_sample_rows : input.num_rows());
  if (host_status != 0) { return fallback(status_reason(host_status)); }
  auto observed_groups = static_cast<int>(std::count_if(host_representatives.begin(),
                                                        host_representatives.end(),
                                                        [](int32_t row) { return row >= 0; }));
  bool const used_prefix_preflight =
    prefix_is_candidate &&
    bounded_register_shape_fits(observed_groups, static_cast<int>(bounded_plan->states.size()));
  if (prefix_is_candidate && !used_prefix_preflight) {
    run_preflight(input.num_rows());
    if (host_status != 0) { return fallback(status_reason(host_status)); }
    observed_groups = static_cast<int>(std::count_if(host_representatives.begin(),
                                                     host_representatives.end(),
                                                     [](int32_t row) { return row >= 0; }));
  }

  std::vector<cudf::size_type> selected_rows;
  std::vector<int16_t> host_dense_map(domain, int16_t{-1});
  selected_rows.reserve(k_max_groups);
  for (int packed = 0; packed < domain; ++packed) {
    if (host_representatives[packed] < 0) { continue; }
    if (selected_rows.size() == k_max_groups) {
      return fallback("runtime key cardinality exceeds 64 groups");
    }
    host_dense_map[packed] = static_cast<int16_t>(selected_rows.size());
    selected_rows.push_back(host_representatives[packed]);
  }
  if (selected_rows.empty()) { return fallback("key preflight found no occupied group"); }
  auto const num_groups = static_cast<int>(selected_rows.size());
  if (!bounded_plan ||
      !bounded_register_shape_fits(num_groups, static_cast<int>(bounded_plan->states.size()))) {
    bounded_plan.reset();
  }
  if (used_prefix_preflight && !bounded_plan) {
    return fallback("prefix preflight lost bounded-register resource eligibility");
  }

  rmm::device_uvector<int16_t> device_dense_map(domain, stream, mr);
  CUDF_CUDA_TRY(cudaMemcpyAsync(device_dense_map.data(),
                                host_dense_map.data(),
                                host_dense_map.size() * sizeof(int16_t),
                                cudaMemcpyHostToDevice,
                                stream.value()));

  std::vector<std::unique_ptr<cudf::column>> aggregate_columns;
  std::vector<aggregate_spec> host_aggregate_specs;
  aggregate_columns.reserve(aggregates.size());
  host_aggregate_specs.reserve(aggregates.size());
  for (std::size_t i = 0; i < aggregates.size(); ++i) {
    auto const kind = aggregates[i];
    bool const is_count =
      kind == cudf::aggregation::Kind::COUNT_ALL || kind == cudf::aggregation::Kind::COUNT_VALID;
    cudf::column_view input_column;
    if (kind != cudf::aggregation::Kind::COUNT_ALL) {
      input_column = input.column(aggregate_idx[i]);
    }
    bool const output_nullable = !is_count && input_column.nullable();
    auto output                = cudf::make_fixed_width_column(
      output_types[i],
      num_groups,
      output_nullable ? cudf::mask_state::ALL_VALID : cudf::mask_state::UNALLOCATED,
      stream,
      mr);
    auto mutable_output = output->mutable_view();
    host_aggregate_specs.push_back(make_host_aggregate_spec(
      i, mutable_output.head(), output_nullable ? mutable_output.null_mask() : nullptr));
    aggregate_columns.push_back(std::move(output));
  }

  rmm::device_uvector<aggregate_spec> device_aggregate_specs(
    host_aggregate_specs.size(), stream, mr);
  CUDF_CUDA_TRY(cudaMemcpyAsync(device_aggregate_specs.data(),
                                host_aggregate_specs.data(),
                                host_aggregate_specs.size() * sizeof(aggregate_spec),
                                cudaMemcpyHostToDevice,
                                stream.value()));

  auto const entries = static_cast<std::size_t>(num_groups) * aggregates.size();
  constexpr auto state_bytes_per_entry =
    k_warps_per_block * (sizeof(wide_value) + sizeof(uint32_t));
  auto const smem_bytes = entries * state_bytes_per_entry;
  rmm::device_uvector<cudf::size_type> device_null_counts(aggregates.size(), stream, mr);
  std::unique_ptr<rmm::device_uvector<partial_state>> block_partials;

  auto reset_status = [&] {
    CUDF_CUDA_TRY(cudaMemsetAsync(device_status.data(), 0, sizeof(int32_t), stream.value()));
    CUDF_CUDA_TRY(cudaMemsetAsync(device_null_counts.data(),
                                  0,
                                  device_null_counts.size() * sizeof(cudf::size_type),
                                  stream.value()));
  };
  auto launch_wide = [&] {
    if (entries > k_max_dynamic_smem / state_bytes_per_entry ||
        smem_bytes >
          static_cast<std::size_t>(device_attribute<cudaDevAttrMaxSharedMemoryPerBlock>())) {
      return false;
    }
    auto const aggregate_grid =
      resident_grid_for(input.num_rows(), accumulate_tiny_domain_kernel, smem_bytes);
    auto const reduction_grid =
      static_cast<unsigned>(std::max<std::size_t>((entries + k_block_size - 1) / k_block_size, 1));
    block_partials = std::make_unique<rmm::device_uvector<partial_state>>(
      static_cast<std::size_t>(aggregate_grid) * entries, stream, mr);
    accumulate_tiny_domain_kernel<<<aggregate_grid, k_block_size, smem_bytes, stream.value()>>>(
      device_key_specs.data(),
      static_cast<int>(group_idx.size()),
      input.num_rows(),
      device_dense_map.data(),
      device_aggregate_specs.data(),
      static_cast<int>(aggregates.size()),
      num_groups,
      block_partials->data(),
      device_status.data());
    CUDF_CUDA_TRY(cudaGetLastError());
    reduce_tiny_domain_kernel<<<reduction_grid, k_block_size, 0, stream.value()>>>(
      block_partials->data(),
      static_cast<int>(aggregate_grid),
      device_aggregate_specs.data(),
      static_cast<int>(aggregates.size()),
      num_groups,
      device_null_counts.data(),
      device_status.data());
    CUDF_CUDA_TRY(cudaGetLastError());
    return true;
  };

  bool bounded_register_attempted = false;
  bool used_bounded_register      = false;
  std::unique_ptr<rmm::device_uvector<bounded_register_state_spec>> device_register_states;
  std::unique_ptr<rmm::device_uvector<int8_t>> device_logical_map;
  std::unique_ptr<rmm::device_uvector<wide_value>> physical_results;
  std::unique_ptr<rmm::device_uvector<uint32_t>> physical_validity;
  bounded_register_scratch register_scratch;

  reset_status();
  if (bounded_plan) {
    device_register_states = std::make_unique<rmm::device_uvector<bounded_register_state_spec>>(
      bounded_plan->states.size(), stream, mr);
    device_logical_map = std::make_unique<rmm::device_uvector<int8_t>>(
      bounded_plan->logical_to_physical.size(), stream, mr);
    CUDF_CUDA_TRY(cudaMemcpyAsync(device_register_states->data(),
                                  bounded_plan->states.data(),
                                  bounded_plan->states.size() * sizeof(bounded_register_state_spec),
                                  cudaMemcpyHostToDevice,
                                  stream.value()));
    CUDF_CUDA_TRY(cudaMemcpyAsync(device_logical_map->data(),
                                  bounded_plan->logical_to_physical.data(),
                                  bounded_plan->logical_to_physical.size() * sizeof(int8_t),
                                  cudaMemcpyHostToDevice,
                                  stream.value()));

    auto const physical_entries = static_cast<int>(bounded_plan->states.size()) * num_groups;
    physical_results =
      std::make_unique<rmm::device_uvector<wide_value>>(physical_entries, stream, mr);
    physical_validity =
      std::make_unique<rmm::device_uvector<uint32_t>>(physical_entries, stream, mr);
    bounded_register_attempted = dispatch_bounded_register_accumulation(
      static_cast<int>(bounded_plan->states.size()),
      bounded_register_launch_args{device_key_specs.data(),
                                   static_cast<int>(group_idx.size()),
                                   input.num_rows(),
                                   device_dense_map.data(),
                                   device_register_states->data(),
                                   num_groups,
                                   device_status.data(),
                                   used_prefix_preflight,
                                   bounded_plan->track_sum_validity,
                                   bounded_plan->direct_unmasked_64bit_states},
      register_scratch,
      stream,
      mr);
    if (!bounded_register_attempted) {
      if (used_prefix_preflight) {
        return fallback("bounded-register specialization is below the resident-block floor");
      }
      physical_results.reset();
      physical_validity.reset();
      device_register_states.reset();
      device_logical_map.reset();
      bounded_plan.reset();
      if (!launch_wide()) {
        return fallback("block-private aggregate state exceeds the shared-memory budget");
      }
    } else {
      reduce_bounded_register_partials_kernel<<<physical_entries,
                                                k_block_size,
                                                0,
                                                stream.value()>>>(
        register_scratch.partials->data(),
        register_scratch.validity ? register_scratch.validity->data() : nullptr,
        static_cast<int>(register_scratch.partial_threads),
        physical_entries,
        physical_results->data(),
        physical_validity->data(),
        device_status.data());
      CUDF_CUDA_TRY(cudaGetLastError());
      auto const expansion_grid = static_cast<unsigned>(
        std::max<std::size_t>((entries + k_block_size - 1) / k_block_size, 1));
      expand_bounded_register_results_kernel<<<expansion_grid, k_block_size, 0, stream.value()>>>(
        physical_results->data(),
        physical_validity->data(),
        device_aggregate_specs.data(),
        device_logical_map->data(),
        static_cast<int>(aggregates.size()),
        static_cast<int>(bounded_plan->states.size()),
        num_groups,
        device_null_counts.data(),
        device_status.data());
      CUDF_CUDA_TRY(cudaGetLastError());
    }
  } else if (!launch_wide()) {
    return fallback("block-private aggregate state exceeds the shared-memory budget");
  }

  std::vector<cudf::size_type> host_null_counts(aggregates.size());
  auto fetch_status = [&] {
    CUDF_CUDA_TRY(cudaMemcpyAsync(host_null_counts.data(),
                                  device_null_counts.data(),
                                  host_null_counts.size() * sizeof(cudf::size_type),
                                  cudaMemcpyDeviceToHost,
                                  stream.value()));
    CUDF_CUDA_TRY(cudaMemcpyAsync(&host_status,
                                  device_status.data(),
                                  sizeof(host_status),
                                  cudaMemcpyDeviceToHost,
                                  stream.value()));
    stream.synchronize();
  };
  fetch_status();

  constexpr int expected_prefix_status =
    k_status_bad_key | k_status_late_key | k_status_bounded_register_overflow;
  auto const has_late_key_status = (host_status & (k_status_bad_key | k_status_late_key)) != 0;
  if (used_prefix_preflight && has_late_key_status &&
      (host_status & ~expected_prefix_status) == 0) {
    return fallback(status_reason(host_status));
  }
  if (bounded_register_attempted && host_status == k_status_bounded_register_overflow) {
    // fetch_status synchronized the stream, so release register scratch before allocating the
    // larger exact-wide fallback state.
    register_scratch.partials.reset();
    register_scratch.validity.reset();
    physical_results.reset();
    physical_validity.reset();
    device_register_states.reset();
    device_logical_map.reset();
    reset_status();
    if (!launch_wide()) {
      return fallback("exact-wide overflow retry exceeds the shared-memory budget");
    }
    fetch_status();
  } else if (bounded_register_attempted && host_status == 0) {
    used_bounded_register = true;
  }
  if ((host_status & k_status_overflow) != 0) {
    throw std::overflow_error("tiny-domain aggregate invariant failed: " +
                              status_reason(host_status));
  }
  if (host_status != 0) {
    throw std::runtime_error("tiny-domain aggregate invariant failed: " +
                             status_reason(host_status));
  }

  for (std::size_t i = 0; i < aggregate_columns.size(); ++i) {
    aggregate_columns[i]->set_null_count(host_null_counts[i]);
  }

  rmm::device_uvector<cudf::size_type> device_selected_rows(selected_rows.size(), stream, mr);
  CUDF_CUDA_TRY(cudaMemcpyAsync(device_selected_rows.data(),
                                selected_rows.data(),
                                selected_rows.size() * sizeof(cudf::size_type),
                                cudaMemcpyHostToDevice,
                                stream.value()));
  auto gather_map =
    cudf::column_view(cudf::device_span<cudf::size_type const>(device_selected_rows));
  auto restored_keys  = cudf::gather(cudf::table_view(group_columns),
                                    gather_map,
                                    cudf::out_of_bounds_policy::DONT_CHECK,
                                    cudf::negative_index_policy::NOT_ALLOWED,
                                    stream,
                                    mr);
  auto output_columns = restored_keys->release();
  for (auto& column : aggregate_columns) {
    output_columns.push_back(std::move(column));
  }
  return tiny_domain_grouped_aggregate_attempt{
    std::make_unique<cudf::table>(std::move(output_columns)),
    {},
    selected_rows.size(),
    bounded_plan ? bounded_plan->states.size() : 0,
    bounded_register_attempted,
    used_bounded_register,
    used_prefix_preflight};
}

}  // namespace sirius::op
