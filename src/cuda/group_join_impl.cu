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
#include <op/aggregate/group_join_impl.hpp>
#include <sirius/exception.hpp>

#include <cudf/column/column.hpp>
#include <cudf/column/column_factories.hpp>
#include <cudf/null_mask.hpp>
#include <cudf/reduction.hpp>
#include <cudf/scalar/scalar.hpp>
#include <cudf/utilities/bit.hpp>
#include <cudf/utilities/error.hpp>

#include <rmm/exec_policy.hpp>

#include <thrust/copy.h>
#include <thrust/count.h>
#include <thrust/fill.h>
#include <thrust/iterator/counting_iterator.h>

#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>
// clang-format on

namespace sirius::op {

namespace {

constexpr int k_block_size      = 256;
constexpr int64_t k_max_blocks  = 4096;
constexpr uint64_t k_bigint_max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

template <typename KeyT>
[[nodiscard]] constexpr cudf::type_id key_type_id_for()
{
  static_assert(std::is_same_v<KeyT, int32_t> || std::is_same_v<KeyT, int64_t>,
                "group_join keys are INT32 or INT64");
  return std::is_same_v<KeyT, int32_t> ? cudf::type_id::INT32 : cudf::type_id::INT64;
}

[[nodiscard]] unsigned grid_size_for(int64_t n)
{
  auto const blocks = std::min<int64_t>(n / k_block_size + (n % k_block_size != 0), k_max_blocks);
  return static_cast<unsigned>(std::max<int64_t>(blocks, 1));
}

[[nodiscard]] cudf::size_type checked_output_rows(int64_t num_groups, bool append_null_group)
{
  auto const max   = static_cast<int64_t>(std::numeric_limits<cudf::size_type>::max());
  auto const extra = append_null_group ? int64_t{1} : int64_t{0};
  if (num_groups < 0 || num_groups > max - extra) {
    throw sirius::invalid_input_exception(
      "group_join: output row count {} plus NULL-group row {} exceeds "
      "cudf::size_type max {}",
      num_groups,
      extra,
      max);
  }
  return static_cast<cudf::size_type>(num_groups + extra);
}

struct histogram_layout {
  std::size_t slots;
  std::size_t bytes_per_histogram;
};

[[nodiscard]] histogram_layout checked_histogram_layout(int64_t range, std::size_t slot_bytes)
{
  if (range <= 0) {
    throw sirius::internal_exception("group_join_state: non-positive range {}", range);
  }
  auto const slots    = static_cast<uint64_t>(range);
  auto const size_max = std::numeric_limits<std::size_t>::max();
  if (slot_bytes == 0 || slot_bytes > size_max / 2 || slots > size_max / (2 * slot_bytes)) {
    throw sirius::invalid_input_exception(
      "group_join: histogram range {} with {}-byte slots exceeds size_t allocation "
      "capacity",
      range,
      slot_bytes);
  }
  auto const slot_count = static_cast<std::size_t>(slots);
  return histogram_layout{slot_count, slot_count * slot_bytes};
}

__global__ void initialize_extrema_kernel(int64_t* extrema)
{
  if (blockIdx.x == 0 && threadIdx.x == 0) {
    extrema[0] = std::numeric_limits<int64_t>::max();
    extrema[1] = std::numeric_limits<int64_t>::min();
  }
}

template <typename KeyT>
__global__ void merge_extrema_kernel(KeyT const* batch_min, KeyT const* batch_max, int64_t* extrema)
{
  if (blockIdx.x == 0 && threadIdx.x == 0) {
    auto const lo = static_cast<int64_t>(*batch_min);
    auto const hi = static_cast<int64_t>(*batch_max);
    if (lo < extrema[0]) { extrema[0] = lo; }
    if (hi > extrema[1]) { extrema[1] = hi; }
  }
}

__device__ __forceinline__ void histogram_add(uint32_t* slot) { atomicAdd(slot, 1u); }

__device__ __forceinline__ void histogram_add(uint64_t* slot)
{
  static_assert(sizeof(unsigned long long) == sizeof(uint64_t));
  atomicAdd(reinterpret_cast<unsigned long long*>(slot), 1ULL);
}

// Slot policies: the per-aggregate device accumulators the bundles compose (one policy per
// agg_op). Whether a slot zero-initializes or fill-initializes is part of the policy.
enum class slot_init : uint8_t { ZERO_MEMSET, VALUE_FILL };

/// Argument tag for slots that consume no value column; its update folds to a bare count.
struct no_arg {};

template <class P>
concept slot_policy = std::is_trivially_copyable_v<typename P::slot_type> &&
                      requires(typename P::slot_type* s, typename P::arg_type v) {
                        { P::init_fill } -> std::convertible_to<slot_init>;
                        { P::update(s, v) } -> std::same_as<void>;
                      };

template <class CountT>
struct count_slot {
  using slot_type                      = CountT;
  using arg_type                       = no_arg;
  static constexpr slot_init init_fill = slot_init::ZERO_MEMSET;
  __device__ __forceinline__ static void update(slot_type* s, no_arg) { histogram_add(s); }
};

// Maps a host-visible bundle tag to its device-side slot policies. A specialization here plus an
// entry on the explicit-instantiation whitelist at the end of this file admits a new bundle.
template <typename Bundle>
struct bundle_policies;

template <typename CountT>
struct bundle_policies<groupjoin::count_bundle<CountT>> {
  using matched_slot = count_slot<CountT>;
  static_assert(slot_policy<matched_slot>);
};

// Value slot policies for the INNER/DIRECT forms. Every payload accumulates as int64: integer
// arguments widen and DECIMAL32/64 arguments accumulate as their unscaled representation
// (order-preserving under a fixed scale), with the declared-type cast applied at emit.
struct sum_slot_i64 {
  using slot_type                      = int64_t;
  using arg_type                       = int64_t;
  static constexpr slot_init init_fill = slot_init::ZERO_MEMSET;
  __device__ __forceinline__ static void update(slot_type* s, int64_t v)
  {
    // Unsigned add wraps modulo 2^64, matching the generic path's int64 accumulation; the host
    // coarse bound (counted rows x max absolute argument) declines dense before a wrap can
    // diverge from that parity.
    static_assert(sizeof(unsigned long long) == sizeof(int64_t));
    atomicAdd(reinterpret_cast<unsigned long long*>(s), static_cast<unsigned long long>(v));
  }
};

struct min_slot_i64 {
  using slot_type                      = int64_t;
  using arg_type                       = int64_t;
  static constexpr slot_init init_fill = slot_init::VALUE_FILL;
  static constexpr int64_t fill_value  = std::numeric_limits<int64_t>::max();
  __device__ __forceinline__ static void update(slot_type* s, int64_t v)
  {
    static_assert(sizeof(long long) == sizeof(int64_t));
    atomicMin(reinterpret_cast<long long*>(s), static_cast<long long>(v));
  }
};

struct max_slot_i64 {
  using slot_type                      = int64_t;
  using arg_type                       = int64_t;
  static constexpr slot_init init_fill = slot_init::VALUE_FILL;
  static constexpr int64_t fill_value  = std::numeric_limits<int64_t>::min();
  __device__ __forceinline__ static void update(slot_type* s, int64_t v)
  {
    static_assert(sizeof(long long) == sizeof(int64_t));
    atomicMax(reinterpret_cast<long long*>(s), static_cast<long long>(v));
  }
};

static_assert(slot_policy<sum_slot_i64>);
static_assert(slot_policy<min_slot_i64>);
static_assert(slot_policy<max_slot_i64>);

template <typename KeyT, typename Bundle>
__global__ void accumulate_kernel(KeyT const* __restrict__ keys,
                                  cudf::bitmask_type const* __restrict__ key_mask,
                                  cudf::size_type key_mask_offset,
                                  cudf::bitmask_type const* __restrict__ value_mask,
                                  cudf::size_type value_mask_offset,
                                  int64_t n,
                                  int64_t min_key,
                                  int64_t range,
                                  bool bounds_check,
                                  typename Bundle::count_type* __restrict__ bins)
{
  using matched_slot = typename bundle_policies<Bundle>::matched_slot;
  auto const stride  = static_cast<int64_t>(gridDim.x) * blockDim.x;
  for (int64_t i = static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < n;
       i += stride) {
    if (key_mask != nullptr &&
        !cudf::bit_is_set(key_mask, key_mask_offset + static_cast<cudf::size_type>(i))) {
      continue;
    }
    if (value_mask != nullptr &&
        !cudf::bit_is_set(value_mask, value_mask_offset + static_cast<cudf::size_type>(i))) {
      continue;
    }
    // Unsigned subtraction avoids signed overflow; offset < range exactly for in-domain keys.
    auto const offset =
      static_cast<uint64_t>(static_cast<int64_t>(keys[i])) - static_cast<uint64_t>(min_key);
    if (bounds_check && offset >= static_cast<uint64_t>(range)) { continue; }
    matched_slot::update(&bins[offset], no_arg{});
  }
}

// Counted-pass slot routing for the INNER/DIRECT forms: NULL keys go to null_slot (negative
// means skip, the join behavior; DIRECT passes the extra slot at index range) and out-of-domain
// keys are dropped only when bounds_check is set.
template <typename KeyT>
__device__ __forceinline__ int64_t
counted_slot_index(KeyT const* __restrict__ keys,
                   cudf::bitmask_type const* __restrict__ key_mask,
                   cudf::size_type key_mask_offset,
                   int64_t i,
                   int64_t min_key,
                   int64_t range,
                   bool bounds_check,
                   int64_t null_slot)
{
  if (key_mask != nullptr &&
      !cudf::bit_is_set(key_mask, key_mask_offset + static_cast<cudf::size_type>(i))) {
    return null_slot;
  }
  auto const offset =
    static_cast<uint64_t>(static_cast<int64_t>(keys[i])) - static_cast<uint64_t>(min_key);
  if (bounds_check && offset >= static_cast<uint64_t>(range)) { return -1; }
  return static_cast<int64_t>(offset);
}

// Counted pass for the payload-less COUNT bundle on the INNER/DIRECT forms. The argument-validity
// gate makes COUNT(col) and COUNT(*) identical here, so no value mask is consulted.
template <typename KeyT, typename MatchedT>
__global__ void accumulate_matched_kernel(KeyT const* __restrict__ keys,
                                          cudf::bitmask_type const* __restrict__ key_mask,
                                          cudf::size_type key_mask_offset,
                                          int64_t n,
                                          int64_t min_key,
                                          int64_t range,
                                          bool bounds_check,
                                          int64_t null_slot,
                                          MatchedT* __restrict__ matched)
{
  auto const stride = static_cast<int64_t>(gridDim.x) * blockDim.x;
  for (int64_t i = static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < n;
       i += stride) {
    auto const slot = counted_slot_index(
      keys, key_mask, key_mask_offset, i, min_key, range, bounds_check, null_slot);
    if (slot < 0) { continue; }
    histogram_add(&matched[slot]);
  }
}

// Fused counted pass for the value bundles: one mask check per row, then the matched count and
// the payload atomic. Arguments must be NULL-free (the argument-validity gate); Payload is one of
// the value slot policies above.
template <typename KeyT, typename MatchedT, typename ArgT, typename Payload>
__global__ void accumulate_value_kernel(KeyT const* __restrict__ keys,
                                        cudf::bitmask_type const* __restrict__ key_mask,
                                        cudf::size_type key_mask_offset,
                                        ArgT const* __restrict__ args,
                                        int64_t n,
                                        int64_t min_key,
                                        int64_t range,
                                        bool bounds_check,
                                        int64_t null_slot,
                                        MatchedT* __restrict__ matched,
                                        int64_t* __restrict__ payload)
{
  auto const stride = static_cast<int64_t>(gridDim.x) * blockDim.x;
  for (int64_t i = static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < n;
       i += stride) {
    auto const slot = counted_slot_index(
      keys, key_mask, key_mask_offset, i, min_key, range, bounds_check, null_slot);
    if (slot < 0) { continue; }
    histogram_add(&matched[slot]);
    Payload::update(&payload[slot], static_cast<int64_t>(args[i]));
  }
}

template <typename CountT>
struct presence_positive {
  CountT const* presence;
  __device__ bool operator()(int64_t k) const { return presence[k] != CountT{0}; }
};

// INNER emit selection: a group survives only with preserved presence and at least one counted
// match.
template <typename PresenceT, typename MatchedT>
struct inner_positive {
  PresenceT const* presence;
  MatchedT const* matched;
  __device__ bool operator()(int64_t k) const
  {
    return presence[k] != PresenceT{0} && matched[k] != MatchedT{0};
  }
};

// DIRECT emit selection: a group exists exactly when rows accumulated into its slot.
template <typename MatchedT>
struct matched_positive {
  MatchedT const* matched;
  __device__ bool operator()(int64_t k) const { return matched[k] != MatchedT{0}; }
};

template <typename KeyT, typename Bundle>
__global__ void emit_kernel(int64_t const* __restrict__ selected,
                            int64_t num_selected,
                            typename Bundle::count_type const* __restrict__ presence,
                            typename Bundle::count_type const* __restrict__ counts,
                            int64_t min_key,
                            bool count_star,
                            KeyT* __restrict__ out_keys,
                            int64_t* __restrict__ out_values,
                            int32_t* __restrict__ overflow_flag)
{
  auto const stride = static_cast<int64_t>(gridDim.x) * blockDim.x;
  for (int64_t i = static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < num_selected;
       i += stride) {
    auto const k = selected[i];
    auto const p = static_cast<uint64_t>(presence[k]);
    auto matched = static_cast<uint64_t>(counts[k]);
    if (count_star && matched == 0) { matched = 1; }
    out_keys[i] = static_cast<KeyT>(min_key + k);
    if (overflow_flag != nullptr && matched != 0 && p > k_bigint_max / matched) {
      atomicExch(overflow_flag, 1);
      out_values[i] = 0;
    } else {
      out_values[i] = static_cast<int64_t>(p * matched);
    }
  }
}

// INNER COUNT emit: presence x matched with the optional BIGINT-overflow flag. No empty-group
// fix exists here -- the selection predicate already guarantees matched > 0.
template <typename KeyT, typename PresenceT, typename MatchedT>
__global__ void inner_count_emit_kernel(int64_t const* __restrict__ selected,
                                        int64_t num_selected,
                                        PresenceT const* __restrict__ presence,
                                        MatchedT const* __restrict__ matched,
                                        int64_t min_key,
                                        KeyT* __restrict__ out_keys,
                                        int64_t* __restrict__ out_values,
                                        int32_t* __restrict__ overflow_flag)
{
  auto const stride = static_cast<int64_t>(gridDim.x) * blockDim.x;
  for (int64_t i = static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < num_selected;
       i += stride) {
    auto const k = selected[i];
    auto const p = static_cast<uint64_t>(presence[k]);
    auto const m = static_cast<uint64_t>(matched[k]);
    out_keys[i]  = static_cast<KeyT>(min_key + k);
    if (overflow_flag != nullptr && m != 0 && p > k_bigint_max / m) {
      atomicExch(overflow_flag, 1);
      out_values[i] = 0;
    } else {
      out_values[i] = static_cast<int64_t>(p * m);
    }
  }
}

// INNER SUM emit: the Yan-Larson scaling presence x sum, computed modulo 2^64 exactly as the
// generic path's int64 accumulation over the joined rows would.
template <typename KeyT, typename PresenceT>
__global__ void inner_sum_emit_kernel(int64_t const* __restrict__ selected,
                                      int64_t num_selected,
                                      PresenceT const* __restrict__ presence,
                                      int64_t const* __restrict__ sum,
                                      int64_t min_key,
                                      KeyT* __restrict__ out_keys,
                                      int64_t* __restrict__ out_values)
{
  auto const stride = static_cast<int64_t>(gridDim.x) * blockDim.x;
  for (int64_t i = static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < num_selected;
       i += stride) {
    auto const k = selected[i];
    out_keys[i]  = static_cast<KeyT>(min_key + k);
    out_values[i] =
      static_cast<int64_t>(static_cast<uint64_t>(presence[k]) * static_cast<uint64_t>(sum[k]));
  }
}

// Raw slot emit: MIN/MAX extremes, DIRECT counts and sums, and AVG's numerator and divisor.
// out_keys may be nullptr when an earlier launch already wrote the key column.
template <typename KeyT, typename SlotT>
__global__ void emit_raw_slot_kernel(int64_t const* __restrict__ selected,
                                     int64_t num_selected,
                                     SlotT const* __restrict__ slots,
                                     int64_t min_key,
                                     KeyT* __restrict__ out_keys,
                                     int64_t* __restrict__ out_values)
{
  auto const stride = static_cast<int64_t>(gridDim.x) * blockDim.x;
  for (int64_t i = static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < num_selected;
       i += stride) {
    auto const k = selected[i];
    if (out_keys != nullptr) { out_keys[i] = static_cast<KeyT>(min_key + k); }
    out_values[i] = static_cast<int64_t>(slots[k]);
  }
}

__global__ void validate_product_kernel(int64_t const* __restrict__ lhs,
                                        int64_t const* __restrict__ rhs,
                                        int64_t n,
                                        int32_t* __restrict__ status)
{
  auto const stride = static_cast<int64_t>(gridDim.x) * blockDim.x;
  for (int64_t i = static_cast<int64_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < n;
       i += stride) {
    auto const left  = lhs[i];
    auto const right = rhs[i];
    if (left < 0 || right < 0) {
      atomicMax(status, int32_t{2});
      continue;
    }
    auto const left_u  = static_cast<uint64_t>(left);
    auto const right_u = static_cast<uint64_t>(right);
    if (right_u != 0 && left_u > k_bigint_max / right_u) { atomicMax(status, int32_t{1}); }
  }
}

template <typename KeyT, typename Bundle>
void accumulate_impl(cudf::column_view const& keys,
                     cudf::column_view const* count_validity_source,
                     int64_t min_key,
                     int64_t range,
                     bool bounds_check,
                     typename Bundle::count_type* bins,
                     rmm::cuda_stream_view stream)
{
  if (count_validity_source != nullptr && count_validity_source->size() != keys.size()) {
    throw sirius::internal_exception(
      "group_join: COUNT argument has {} rows but its key column has {}",
      count_validity_source->size(),
      keys.size());
  }
  if (keys.type().id() != key_type_id_for<KeyT>()) {
    throw sirius::internal_exception(
      "group_join: key column type {} does not match the state's instantiated key type {}",
      static_cast<int32_t>(keys.type().id()),
      static_cast<int32_t>(key_type_id_for<KeyT>()));
  }
  auto const n = static_cast<int64_t>(keys.size());
  if (n == 0) { return; }

  cudf::bitmask_type const* key_mask = keys.null_count() > 0 ? keys.null_mask() : nullptr;
  cudf::bitmask_type const* val_mask = nullptr;
  cudf::size_type val_mask_offset    = 0;
  if (count_validity_source != nullptr && count_validity_source->null_count() > 0) {
    val_mask        = count_validity_source->null_mask();
    val_mask_offset = count_validity_source->offset();
  }

  auto const grid = grid_size_for(n);
  accumulate_kernel<KeyT, Bundle>
    <<<grid, k_block_size, 0, stream.value()>>>(keys.template data<KeyT>(),
                                                key_mask,
                                                keys.offset(),
                                                val_mask,
                                                val_mask_offset,
                                                n,
                                                min_key,
                                                range,
                                                bounds_check,
                                                bins);
  CUDF_CUDA_TRY(cudaGetLastError());
}

// Zero the key payload so bytes beneath the NULL are deterministic.
template <typename KeyT>
__global__ void write_null_group_kernel(
  KeyT* key, int64_t* value, cudf::size_type row, bool count_star, int64_t null_group_rows)
{
  if (blockIdx.x == 0 && threadIdx.x == 0) {
    key[row]   = KeyT{0};
    value[row] = count_star ? null_group_rows : 0;
  }
}

void write_null_group_row(cudf::column& key_col,
                          cudf::column& value_col,
                          cudf::size_type row_idx,
                          bool count_star,
                          int64_t null_group_rows,
                          rmm::cuda_stream_view stream,
                          rmm::device_async_resource_ref mr)
{
  if (row_idx < 0 || row_idx >= key_col.size() || row_idx >= value_col.size()) {
    throw sirius::internal_exception(
      "group_join: NULL-group row {} is outside output column sizes {} and {}",
      row_idx,
      key_col.size(),
      value_col.size());
  }
  auto key_view   = key_col.mutable_view();
  auto value_view = value_col.mutable_view();
  auto launch     = [&](auto key_tag) {
    using KeyT = decltype(key_tag);
    write_null_group_kernel<KeyT><<<1, 1, 0, stream.value()>>>(key_view.template data<KeyT>(),
                                                               value_view.template data<int64_t>(),
                                                               row_idx,
                                                               count_star,
                                                               null_group_rows);
  };
  switch (key_view.type().id()) {
    case cudf::type_id::INT32: launch(int32_t{}); break;
    case cudf::type_id::INT64: launch(int64_t{}); break;
    default:
      throw sirius::internal_exception(
        "group_join: unsupported output key type {} (expected INT32/INT64)",
        static_cast<int32_t>(key_view.type().id()));
  }
  CUDF_CUDA_TRY(cudaGetLastError());

  auto mask = cudf::create_null_mask(key_col.size(), cudf::mask_state::ALL_VALID, stream, mr);
  cudf::set_null_mask(
    static_cast<cudf::bitmask_type*>(mask.data()), row_idx, row_idx + 1, false, stream);
  key_col.set_null_mask(std::move(mask), 1);
}

// One-thread writers for the DIRECT form's NULL-group output row: the key payload is zeroed
// beneath its NULL bit and each value copies straight from the extra slot at index range.
template <typename KeyT>
__global__ void write_null_group_key_kernel(KeyT* __restrict__ key, cudf::size_type row)
{
  if (blockIdx.x == 0 && threadIdx.x == 0) { key[row] = KeyT{0}; }
}

template <typename SlotT>
__global__ void write_null_group_value_kernel(int64_t* __restrict__ out,
                                              cudf::size_type row,
                                              SlotT const* __restrict__ slot)
{
  if (blockIdx.x == 0 && threadIdx.x == 0) { out[row] = static_cast<int64_t>(*slot); }
}

/// Validates a value-form state layout and returns the per-array slot count (range plus the
/// DIRECT NULL-group slot when requested).
[[nodiscard]] std::size_t checked_value_slots(int64_t range,
                                              std::size_t combined_slot_bytes,
                                              bool with_null_slot)
{
  if (range <= 0) {
    throw sirius::internal_exception("group_join_state: non-positive range {}", range);
  }
  auto const size_max = std::numeric_limits<std::size_t>::max();
  auto const extra    = with_null_slot ? uint64_t{1} : uint64_t{0};
  auto const slots    = static_cast<uint64_t>(range);
  if (combined_slot_bytes == 0 || slots > (size_max - extra) ||
      slots + extra > size_max / combined_slot_bytes) {
    throw sirius::invalid_input_exception(
      "group_join: histogram range {} with {}-byte combined slots exceeds size_t allocation "
      "capacity",
      range,
      combined_slot_bytes);
  }
  return static_cast<std::size_t>(slots + extra);
}

/// Allocates and initializes the value-bundle payload array per its slot policy (zero for SUM/AVG,
/// sentinel fill for MIN/MAX); COUNT carries no payload.
std::optional<rmm::device_uvector<int64_t>> make_payload_array(groupjoin::dense_value_op op,
                                                               std::size_t slots,
                                                               rmm::cuda_stream_view stream,
                                                               rmm::device_async_resource_ref mr)
{
  using groupjoin::dense_value_op;
  if (op == dense_value_op::COUNT) { return std::nullopt; }
  rmm::device_uvector<int64_t> payload(slots, stream, mr);
  switch (op) {
    case dense_value_op::SUM:
    case dense_value_op::AVG:
      static_assert(sum_slot_i64::init_fill == slot_init::ZERO_MEMSET);
      CUDF_CUDA_TRY(cudaMemsetAsync(payload.data(), 0, slots * sizeof(int64_t), stream.value()));
      break;
    case dense_value_op::MIN:
      static_assert(min_slot_i64::init_fill == slot_init::VALUE_FILL);
      thrust::fill(
        rmm::exec_policy(stream, mr), payload.begin(), payload.end(), min_slot_i64::fill_value);
      break;
    case dense_value_op::MAX:
      static_assert(max_slot_i64::init_fill == slot_init::VALUE_FILL);
      thrust::fill(
        rmm::exec_policy(stream, mr), payload.begin(), payload.end(), max_slot_i64::fill_value);
      break;
    default: throw sirius::internal_exception("group_join: unexpected payload op");
  }
  return payload;
}

/// Runs one counted-side batch through the form's fused accumulate kernel. @p args must be the
/// argument column viewed as ArgT (NULL-free per the argument-validity gate) for value ops and
/// nullptr for COUNT.
template <typename KeyT, typename MatchedT, typename ArgT>
void accumulate_counted_form(groupjoin::dense_value_op op,
                             cudf::column_view const& keys,
                             cudf::column_view const* args,
                             int64_t min_key,
                             int64_t range,
                             bool bounds_check,
                             int64_t null_slot,
                             MatchedT* matched,
                             int64_t* payload,
                             rmm::cuda_stream_view stream)
{
  using groupjoin::dense_value_op;
  if (keys.type().id() != key_type_id_for<KeyT>()) {
    throw sirius::internal_exception(
      "group_join: key column type {} does not match the state's instantiated key type {}",
      static_cast<int32_t>(keys.type().id()),
      static_cast<int32_t>(key_type_id_for<KeyT>()));
  }
  if ((args != nullptr) != (op != dense_value_op::COUNT)) {
    throw sirius::internal_exception(
      "group_join: value ops take an argument column and COUNT "
      "takes none");
  }
  if (args != nullptr) {
    if (args->size() != keys.size()) {
      throw sirius::internal_exception(
        "group_join: aggregate argument has {} rows but its key column has {}",
        args->size(),
        keys.size());
    }
    if (args->null_count() != 0) {
      throw sirius::internal_exception(
        "group_join: dense value accumulation requires NULL-free arguments (argument-validity "
        "gate)");
    }
    if (args->type().id() != key_type_id_for<ArgT>()) {
      throw sirius::internal_exception(
        "group_join: argument representation type {} does not match the instantiated ArgT {}",
        static_cast<int32_t>(args->type().id()),
        static_cast<int32_t>(key_type_id_for<ArgT>()));
    }
  }
  auto const n = static_cast<int64_t>(keys.size());
  if (n == 0) { return; }

  cudf::bitmask_type const* key_mask = keys.null_count() > 0 ? keys.null_mask() : nullptr;
  auto const grid                    = grid_size_for(n);
  auto launch_value                  = [&](auto payload_policy) {
    using Payload = decltype(payload_policy);
    accumulate_value_kernel<KeyT, MatchedT, ArgT, Payload>
      <<<grid, k_block_size, 0, stream.value()>>>(keys.template data<KeyT>(),
                                                  key_mask,
                                                  keys.offset(),
                                                  args->template data<ArgT>(),
                                                  n,
                                                  min_key,
                                                  range,
                                                  bounds_check,
                                                  null_slot,
                                                  matched,
                                                  payload);
  };
  switch (op) {
    case dense_value_op::COUNT:
      accumulate_matched_kernel<KeyT, MatchedT>
        <<<grid, k_block_size, 0, stream.value()>>>(keys.template data<KeyT>(),
                                                    key_mask,
                                                    keys.offset(),
                                                    n,
                                                    min_key,
                                                    range,
                                                    bounds_check,
                                                    null_slot,
                                                    matched);
      break;
    case dense_value_op::SUM:
    case dense_value_op::AVG: launch_value(sum_slot_i64{}); break;
    case dense_value_op::MIN: launch_value(min_slot_i64{}); break;
    case dense_value_op::MAX: launch_value(max_slot_i64{}); break;
  }
  CUDF_CUDA_TRY(cudaGetLastError());
}

/// Selection + emit tail shared by the one-task INNER driver and the streamed dense state: count
/// and gather the surviving slots, then write `[key, value:int64]` (plus AVG's divisor) with the
/// op's emit kernel.
template <typename KeyT, typename PresenceT, typename MatchedT>
std::unique_ptr<cudf::table> emit_inner_dense_groups(groupjoin::dense_value_op op,
                                                     cudf::data_type key_type,
                                                     int64_t min_key,
                                                     int64_t range,
                                                     PresenceT const* presence,
                                                     MatchedT const* matched,
                                                     int64_t const* payload,
                                                     bool check_count_product_overflow,
                                                     rmm::cuda_stream_view stream,
                                                     rmm::device_async_resource_ref mr)
{
  using groupjoin::dense_value_op;
  auto const policy = rmm::exec_policy(stream, mr);
  auto const begin  = thrust::make_counting_iterator<int64_t>(0);
  auto const end    = thrust::make_counting_iterator<int64_t>(range);
  inner_positive<PresenceT, MatchedT> const selector{presence, matched};
  int64_t const num_groups = thrust::count_if(policy, begin, end, selector);
  auto const group_rows    = checked_output_rows(num_groups, false);

  auto key_col =
    cudf::make_fixed_width_column(key_type, group_rows, cudf::mask_state::UNALLOCATED, stream, mr);
  auto value_col = cudf::make_fixed_width_column(
    cudf::data_type{cudf::type_id::INT64}, group_rows, cudf::mask_state::UNALLOCATED, stream, mr);
  std::unique_ptr<cudf::column> divisor_col;
  if (op == dense_value_op::AVG) {
    divisor_col = cudf::make_fixed_width_column(
      cudf::data_type{cudf::type_id::INT64}, group_rows, cudf::mask_state::UNALLOCATED, stream, mr);
  }
  std::optional<cudf::numeric_scalar<int32_t>> overflow_flag;
  if (op == dense_value_op::COUNT && check_count_product_overflow && num_groups > 0) {
    overflow_flag.emplace(0, true, stream, mr);
  }

  if (num_groups > 0) {
    rmm::device_uvector<int64_t> selected(static_cast<std::size_t>(group_rows), stream, mr);
    thrust::copy_if(policy, begin, end, selected.begin(), selector);

    auto const grid = grid_size_for(num_groups);
    auto key_out    = key_col->mutable_view().template data<KeyT>();
    auto value_out  = value_col->mutable_view().template data<int64_t>();
    switch (op) {
      case dense_value_op::COUNT:
        inner_count_emit_kernel<KeyT, PresenceT, MatchedT>
          <<<grid, k_block_size, 0, stream.value()>>>(
            selected.data(),
            num_groups,
            presence,
            matched,
            min_key,
            key_out,
            value_out,
            overflow_flag ? overflow_flag->data() : nullptr);
        break;
      case dense_value_op::SUM:
        inner_sum_emit_kernel<KeyT, PresenceT><<<grid, k_block_size, 0, stream.value()>>>(
          selected.data(), num_groups, presence, payload, min_key, key_out, value_out);
        break;
      case dense_value_op::MIN:
      case dense_value_op::MAX:
        emit_raw_slot_kernel<KeyT, int64_t><<<grid, k_block_size, 0, stream.value()>>>(
          selected.data(), num_groups, payload, min_key, key_out, value_out);
        break;
      case dense_value_op::AVG:
        emit_raw_slot_kernel<KeyT, int64_t><<<grid, k_block_size, 0, stream.value()>>>(
          selected.data(), num_groups, payload, min_key, key_out, value_out);
        emit_raw_slot_kernel<KeyT, MatchedT><<<grid, k_block_size, 0, stream.value()>>>(
          selected.data(),
          num_groups,
          matched,
          min_key,
          static_cast<KeyT*>(nullptr),
          divisor_col->mutable_view().template data<int64_t>());
        break;
    }
    CUDF_CUDA_TRY(cudaGetLastError());
    // The scalar read synchronizes only on the rare path whose coarse host bound was inconclusive.
    if (overflow_flag && overflow_flag->value(stream) != 0) {
      throw sirius::invalid_input_exception("group_join: COUNT result exceeds BIGINT max {}",
                                            k_bigint_max);
    }
  }

  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(std::move(key_col));
  columns.push_back(std::move(value_col));
  if (divisor_col) { columns.push_back(std::move(divisor_col)); }
  return std::make_unique<cudf::table>(std::move(columns));
}

/// Streamed dense INNER state (see the interface documentation): PresenceT is chosen from the
/// preserved row count; matched is always uint64; the aggregate argument's representation is
/// dispatched per accumulate call.
template <typename KeyT, typename PresenceT>
class stream_dense_inner_impl final : public group_join_stream_dense_state {
 public:
  stream_dense_inner_impl(groupjoin::dense_value_op op,
                          cudf::data_type key_type,
                          int64_t min_key,
                          int64_t range,
                          rmm::cuda_stream_view stream,
                          rmm::device_async_resource_ref mr)
    : _op(op),
      _key_type(key_type),
      _min_key(min_key),
      _range(range),
      _slots(checked_value_slots(range, slot_width(op), /*with_null_slot=*/false)),
      _presence(_slots, stream, mr),
      _matched(_slots, stream, mr),
      _payload(make_payload_array(op, _slots, stream, mr))
  {
    if (key_type.id() != key_type_id_for<KeyT>()) {
      throw sirius::internal_exception("group_join: stream state key type mismatch");
    }
    CUDF_CUDA_TRY(cudaMemsetAsync(_presence.data(), 0, _slots * sizeof(PresenceT), stream.value()));
    CUDF_CUDA_TRY(cudaMemsetAsync(_matched.data(), 0, _slots * sizeof(uint64_t), stream.value()));
  }

  void accumulate_preserved(cudf::column_view const& keys, rmm::cuda_stream_view stream) override
  {
    // The count bundle's kernel verbatim: no bounds check, the state is sized from the preserved
    // extrema.
    accumulate_impl<KeyT, groupjoin::count_bundle<PresenceT>>(
      keys, nullptr, _min_key, _range, /*bounds_check=*/false, _presence.data(), stream);
  }

  void accumulate_counted(cudf::column_view const& keys,
                          cudf::column_view const* rep_args,
                          rmm::cuda_stream_view stream) override
  {
    auto launch = [&](auto arg_tag) {
      using ArgT = decltype(arg_tag);
      accumulate_counted_form<KeyT, uint64_t, ArgT>(_op,
                                                    keys,
                                                    rep_args,
                                                    _min_key,
                                                    _range,
                                                    /*bounds_check=*/true,
                                                    /*null_slot=*/-1,
                                                    _matched.data(),
                                                    _payload ? _payload->data() : nullptr,
                                                    stream);
    };
    if (rep_args != nullptr && rep_args->type().id() == cudf::type_id::INT32) {
      launch(int32_t{});
    } else {
      launch(int64_t{});
    }
  }

  std::unique_ptr<cudf::table> emit(bool check_count_product_overflow,
                                    rmm::cuda_stream_view stream,
                                    rmm::device_async_resource_ref mr) const override
  {
    return emit_inner_dense_groups<KeyT, PresenceT, uint64_t>(_op,
                                                              _key_type,
                                                              _min_key,
                                                              _range,
                                                              _presence.data(),
                                                              _matched.data(),
                                                              _payload ? _payload->data() : nullptr,
                                                              check_count_product_overflow,
                                                              stream,
                                                              mr);
  }

  [[nodiscard]] std::size_t state_bytes() const noexcept override
  {
    return _slots * slot_width(_op);
  }

 private:
  [[nodiscard]] static std::size_t slot_width(groupjoin::dense_value_op op) noexcept
  {
    return sizeof(PresenceT) + sizeof(uint64_t) +
           (op == groupjoin::dense_value_op::COUNT ? 0 : sizeof(int64_t));
  }

  groupjoin::dense_value_op _op;
  cudf::data_type _key_type;
  int64_t _min_key;
  int64_t _range;
  std::size_t _slots;
  rmm::device_uvector<PresenceT> _presence;
  rmm::device_uvector<uint64_t> _matched;
  std::optional<rmm::device_uvector<int64_t>> _payload;
};

}  // namespace

std::unique_ptr<group_join_stream_dense_state> make_group_join_stream_dense_state(
  groupjoin::dense_value_op op,
  cudf::data_type key_type,
  bool presence_wide,
  int64_t min_key,
  int64_t range,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr)
{
  auto const make = [&](auto key_tag,
                        auto presence_tag) -> std::unique_ptr<group_join_stream_dense_state> {
    using KeyT      = decltype(key_tag);
    using PresenceT = decltype(presence_tag);
    return std::make_unique<stream_dense_inner_impl<KeyT, PresenceT>>(
      op, key_type, min_key, range, stream, mr);
  };
  switch (key_type.id()) {
    case cudf::type_id::INT32:
      return presence_wide ? make(int32_t{}, uint64_t{}) : make(int32_t{}, uint32_t{});
    case cudf::type_id::INT64:
      return presence_wide ? make(int64_t{}, uint64_t{}) : make(int64_t{}, uint32_t{});
    default:
      throw sirius::internal_exception(
        "group_join: unsupported stream state key type {} (expected INT32/INT64)",
        static_cast<int32_t>(key_type.id()));
  }
}

std::optional<std::pair<int64_t, int64_t>> group_join_global_minmax(
  std::vector<cudf::column_view> const& keys,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr)
{
  rmm::device_uvector<int64_t> extrema(2, stream, mr);
  initialize_extrema_kernel<<<1, 1, 0, stream.value()>>>(extrema.data());
  CUDF_CUDA_TRY(cudaGetLastError());

  bool has_values   = false;
  using scalar_pair = std::pair<std::unique_ptr<cudf::scalar>, std::unique_ptr<cudf::scalar>>;
  std::vector<scalar_pair> scalar_owners;
  scalar_owners.reserve(keys.size());

  for (auto const& column : keys) {
    if (column.size() == 0 || column.size() == column.null_count()) { continue; }

    auto minmax = cudf::minmax(column, stream, mr);
    auto launch = [&](auto key_tag) {
      using KeyT            = decltype(key_tag);
      auto const& batch_min = static_cast<cudf::numeric_scalar<KeyT> const&>(*minmax.first);
      auto const& batch_max = static_cast<cudf::numeric_scalar<KeyT> const&>(*minmax.second);
      merge_extrema_kernel<KeyT>
        <<<1, 1, 0, stream.value()>>>(batch_min.data(), batch_max.data(), extrema.data());
    };
    switch (column.type().id()) {
      case cudf::type_id::INT32: launch(int32_t{}); break;
      case cudf::type_id::INT64: launch(int64_t{}); break;
      default:
        throw sirius::internal_exception(
          "group_join: unsupported minmax key type {} (expected INT32/INT64)",
          static_cast<int32_t>(column.type().id()));
    }
    CUDF_CUDA_TRY(cudaGetLastError());
    has_values = true;
    scalar_owners.push_back(std::move(minmax));
  }

  if (!has_values) { return std::nullopt; }

  std::array<int64_t, 2> host_extrema{};
  CUDF_CUDA_TRY(cudaMemcpyAsync(host_extrema.data(),
                                extrema.data(),
                                sizeof(host_extrema),
                                cudaMemcpyDeviceToHost,
                                stream.value()));
  stream.synchronize();
  return std::pair{host_extrema[0], host_extrema[1]};
}

std::optional<group_join_extrema> group_join_global_minmax_with_values(
  std::vector<cudf::column_view> const& keys,
  std::vector<cudf::column_view> const& values,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr)
{
  // Slots [0, 1] hold the key extrema and [2, 3] the argument extrema; both pairs merge on-device
  // and read back in the one memcpy below, preserving the single-sync extrema pass.
  rmm::device_uvector<int64_t> extrema(4, stream, mr);
  initialize_extrema_kernel<<<1, 1, 0, stream.value()>>>(extrema.data());
  initialize_extrema_kernel<<<1, 1, 0, stream.value()>>>(extrema.data() + 2);
  CUDF_CUDA_TRY(cudaGetLastError());

  using scalar_pair = std::pair<std::unique_ptr<cudf::scalar>, std::unique_ptr<cudf::scalar>>;
  std::vector<scalar_pair> scalar_owners;
  scalar_owners.reserve(keys.size() + values.size());

  auto merge_column = [&](cudf::column_view const& column, int64_t* slot_pair) {
    if (column.size() == 0 || column.size() == column.null_count()) { return false; }
    auto minmax = cudf::minmax(column, stream, mr);
    auto launch = [&](auto value_tag) {
      using ValueT          = decltype(value_tag);
      auto const& batch_min = static_cast<cudf::numeric_scalar<ValueT> const&>(*minmax.first);
      auto const& batch_max = static_cast<cudf::numeric_scalar<ValueT> const&>(*minmax.second);
      merge_extrema_kernel<ValueT>
        <<<1, 1, 0, stream.value()>>>(batch_min.data(), batch_max.data(), slot_pair);
    };
    switch (column.type().id()) {
      case cudf::type_id::INT32: launch(int32_t{}); break;
      case cudf::type_id::INT64: launch(int64_t{}); break;
      default:
        throw sirius::internal_exception(
          "group_join: unsupported minmax column type {} (expected INT32/INT64)",
          static_cast<int32_t>(column.type().id()));
    }
    CUDF_CUDA_TRY(cudaGetLastError());
    scalar_owners.push_back(std::move(minmax));
    return true;
  };

  bool has_keys = false;
  for (auto const& column : keys) {
    has_keys |= merge_column(column, extrema.data());
  }
  bool has_values = false;
  for (auto const& column : values) {
    has_values |= merge_column(column, extrema.data() + 2);
  }
  if (!has_keys) { return std::nullopt; }

  std::array<int64_t, 4> host_extrema{};
  CUDF_CUDA_TRY(cudaMemcpyAsync(host_extrema.data(),
                                extrema.data(),
                                sizeof(host_extrema),
                                cudaMemcpyDeviceToHost,
                                stream.value()));
  stream.synchronize();
  return group_join_extrema{
    host_extrema[0], host_extrema[1], has_values, host_extrema[2], host_extrema[3]};
}

template <typename KeyT, typename Bundle>
std::size_t group_join_state<KeyT, Bundle>::checked_slots(int64_t range)
{
  return checked_histogram_layout(range, sizeof(count_type)).slots;
}

template <typename KeyT, typename Bundle>
group_join_state<KeyT, Bundle>::group_join_state(int64_t min_key,
                                                 int64_t range,
                                                 rmm::cuda_stream_view stream,
                                                 rmm::device_async_resource_ref mr)
  : _min_key(min_key),
    _range(range),
    _presence(checked_slots(range), stream, mr),
    _matched(_presence.size(), stream, mr)
{
  static_assert(bundle_policies<Bundle>::matched_slot::init_fill == slot_init::ZERO_MEMSET,
                "this constructor zero-initializes; VALUE_FILL slots need a fill pass");
  auto const bytes_per_histogram = _presence.size() * sizeof(count_type);
  CUDF_CUDA_TRY(cudaMemsetAsync(_presence.data(), 0, bytes_per_histogram, stream.value()));
  CUDF_CUDA_TRY(cudaMemsetAsync(_matched.data(), 0, bytes_per_histogram, stream.value()));
}

template <typename KeyT, typename Bundle>
void group_join_state<KeyT, Bundle>::accumulate_preserved(cudf::column_view const& keys,
                                                          rmm::cuda_stream_view stream)
{
  // No bounds check: the state is sized from these columns' global min/max.
  accumulate_impl<KeyT, Bundle>(
    keys, nullptr, _min_key, _range, /*bounds_check=*/false, _presence.data(), stream);
}

template <typename KeyT, typename Bundle>
void group_join_state<KeyT, Bundle>::accumulate_counted(
  cudf::column_view const& keys,
  cudf::column_view const* count_validity_source,
  rmm::cuda_stream_view stream)
{
  accumulate_impl<KeyT, Bundle>(keys,
                                count_validity_source,
                                _min_key,
                                _range,
                                /*bounds_check=*/true,
                                _matched.data(),
                                stream);
}

template <typename KeyT, typename Bundle>
std::unique_ptr<cudf::table> group_join_state<KeyT, Bundle>::emit(cudf::data_type key_type,
                                                                  bool count_star,
                                                                  int64_t null_group_rows,
                                                                  rmm::cuda_stream_view stream,
                                                                  rmm::device_async_resource_ref mr,
                                                                  bool check_product_overflow) const
{
  if (null_group_rows < 0) {
    throw sirius::internal_exception("group_join: negative NULL-group row count");
  }
  if (key_type.id() != key_type_id_for<KeyT>()) {
    throw sirius::internal_exception(
      "group_join: output key type {} does not match the state's instantiated key type {}",
      static_cast<int32_t>(key_type.id()),
      static_cast<int32_t>(key_type_id_for<KeyT>()));
  }
  // Use this memory space's resource for Thrust/CUB temporaries so reservations account for them.
  auto const policy = rmm::exec_policy(stream, mr);
  auto const begin  = thrust::make_counting_iterator<int64_t>(0);
  auto const end    = thrust::make_counting_iterator<int64_t>(_range);

  int64_t const num_groups =
    thrust::count_if(policy, begin, end, presence_positive<count_type>{_presence.data()});
  auto const group_rows = checked_output_rows(num_groups, false);
  auto const total_rows = checked_output_rows(num_groups, null_group_rows > 0);

  auto key_col =
    cudf::make_fixed_width_column(key_type, total_rows, cudf::mask_state::UNALLOCATED, stream, mr);
  auto value_col = cudf::make_fixed_width_column(
    cudf::data_type{cudf::type_id::INT64}, total_rows, cudf::mask_state::UNALLOCATED, stream, mr);
  std::optional<cudf::numeric_scalar<int32_t>> overflow_flag;
  if (check_product_overflow && num_groups > 0) { overflow_flag.emplace(0, true, stream, mr); }

  if (num_groups > 0) {
    rmm::device_uvector<int64_t> selected(static_cast<std::size_t>(group_rows), stream, mr);
    thrust::copy_if(
      policy, begin, end, selected.begin(), presence_positive<count_type>{_presence.data()});

    auto const grid = grid_size_for(num_groups);
    auto key_view   = key_col->mutable_view();
    auto value_view = value_col->mutable_view();
    emit_kernel<KeyT, Bundle>
      <<<grid, k_block_size, 0, stream.value()>>>(selected.data(),
                                                  num_groups,
                                                  _presence.data(),
                                                  _matched.data(),
                                                  _min_key,
                                                  count_star,
                                                  key_view.template data<KeyT>(),
                                                  value_view.template data<int64_t>(),
                                                  overflow_flag ? overflow_flag->data() : nullptr);
    CUDF_CUDA_TRY(cudaGetLastError());
    // The scalar read synchronizes only on the rare path whose coarse host bound was inconclusive.
    if (overflow_flag && overflow_flag->value(stream) != 0) {
      throw sirius::invalid_input_exception("group_join: COUNT result exceeds BIGINT max {}",
                                            k_bigint_max);
    }
  }

  if (null_group_rows > 0) {
    write_null_group_row(*key_col, *value_col, group_rows, count_star, null_group_rows, stream, mr);
  }

  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(std::move(key_col));
  columns.push_back(std::move(value_col));
  return std::make_unique<cudf::table>(std::move(columns));
}

template <typename KeyT, typename PresenceT, typename MatchedT, typename ArgT>
std::unique_ptr<cudf::table> group_join_dense_inner(
  groupjoin::dense_value_op op,
  int64_t min_key,
  int64_t range,
  std::vector<cudf::column_view> const& preserved_keys,
  std::vector<cudf::column_view> const& counted_keys,
  std::vector<cudf::column_view> const& counted_args,
  cudf::data_type key_type,
  bool check_count_product_overflow,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr)
{
  using groupjoin::dense_value_op;
  bool const has_payload = op != dense_value_op::COUNT;
  if (has_payload && counted_args.size() != counted_keys.size()) {
    throw sirius::internal_exception(
      "group_join: {} argument batches are not aligned with {} counted key batches",
      counted_args.size(),
      counted_keys.size());
  }
  if (key_type.id() != key_type_id_for<KeyT>()) {
    throw sirius::internal_exception(
      "group_join: output key type {} does not match the state's instantiated key type {}",
      static_cast<int32_t>(key_type.id()),
      static_cast<int32_t>(key_type_id_for<KeyT>()));
  }

  auto const combined_slot_bytes =
    sizeof(PresenceT) + sizeof(MatchedT) + (has_payload ? sizeof(int64_t) : 0);
  auto const slots = checked_value_slots(range, combined_slot_bytes, /*with_null_slot=*/false);

  rmm::device_uvector<PresenceT> presence(slots, stream, mr);
  rmm::device_uvector<MatchedT> matched(slots, stream, mr);
  CUDF_CUDA_TRY(cudaMemsetAsync(presence.data(), 0, slots * sizeof(PresenceT), stream.value()));
  CUDF_CUDA_TRY(cudaMemsetAsync(matched.data(), 0, slots * sizeof(MatchedT), stream.value()));
  auto payload = make_payload_array(op, slots, stream, mr);

  // The preserved pass is the count bundle's kernel verbatim: no bounds check, since the state is
  // sized from these columns' global min/max.
  for (auto const& col : preserved_keys) {
    accumulate_impl<KeyT, groupjoin::count_bundle<PresenceT>>(
      col, nullptr, min_key, range, /*bounds_check=*/false, presence.data(), stream);
  }
  for (std::size_t i = 0; i < counted_keys.size(); ++i) {
    accumulate_counted_form<KeyT, MatchedT, ArgT>(op,
                                                  counted_keys[i],
                                                  has_payload ? &counted_args[i] : nullptr,
                                                  min_key,
                                                  range,
                                                  /*bounds_check=*/true,
                                                  /*null_slot=*/-1,
                                                  matched.data(),
                                                  payload ? payload->data() : nullptr,
                                                  stream);
  }

  return emit_inner_dense_groups<KeyT, PresenceT, MatchedT>(op,
                                                            key_type,
                                                            min_key,
                                                            range,
                                                            presence.data(),
                                                            matched.data(),
                                                            payload ? payload->data() : nullptr,
                                                            check_count_product_overflow,
                                                            stream,
                                                            mr);
}

template <typename KeyT, typename MatchedT, typename ArgT>
std::unique_ptr<cudf::table> group_join_dense_direct(groupjoin::dense_value_op op,
                                                     int64_t min_key,
                                                     int64_t range,
                                                     std::vector<cudf::column_view> const& keys,
                                                     std::vector<cudf::column_view> const& args,
                                                     cudf::data_type key_type,
                                                     bool has_null_group,
                                                     rmm::cuda_stream_view stream,
                                                     rmm::device_async_resource_ref mr)
{
  using groupjoin::dense_value_op;
  bool const has_payload = op != dense_value_op::COUNT;
  if (has_payload && args.size() != keys.size()) {
    throw sirius::internal_exception(
      "group_join: {} argument batches are not aligned with {} key batches",
      args.size(),
      keys.size());
  }
  if (key_type.id() != key_type_id_for<KeyT>()) {
    throw sirius::internal_exception(
      "group_join: output key type {} does not match the state's instantiated key type {}",
      static_cast<int32_t>(key_type.id()),
      static_cast<int32_t>(key_type_id_for<KeyT>()));
  }

  auto const combined_slot_bytes = sizeof(MatchedT) + (has_payload ? sizeof(int64_t) : 0);
  auto const slots = checked_value_slots(range, combined_slot_bytes, /*with_null_slot=*/true);

  rmm::device_uvector<MatchedT> matched(slots, stream, mr);
  CUDF_CUDA_TRY(cudaMemsetAsync(matched.data(), 0, slots * sizeof(MatchedT), stream.value()));
  auto payload = make_payload_array(op, slots, stream, mr);

  // Single pass, no presence side. Non-NULL keys are in-domain by construction (the state is
  // sized from these columns' extrema) and NULL keys route to the extra slot at index range.
  for (std::size_t i = 0; i < keys.size(); ++i) {
    accumulate_counted_form<KeyT, MatchedT, ArgT>(op,
                                                  keys[i],
                                                  has_payload ? &args[i] : nullptr,
                                                  min_key,
                                                  range,
                                                  /*bounds_check=*/false,
                                                  /*null_slot=*/range,
                                                  matched.data(),
                                                  payload ? payload->data() : nullptr,
                                                  stream);
  }

  auto const policy = rmm::exec_policy(stream, mr);
  auto const begin  = thrust::make_counting_iterator<int64_t>(0);
  auto const end    = thrust::make_counting_iterator<int64_t>(range);
  matched_positive<MatchedT> const selector{matched.data()};
  int64_t const num_groups = thrust::count_if(policy, begin, end, selector);

  auto const group_rows = checked_output_rows(num_groups, false);
  auto const total_rows = checked_output_rows(num_groups, has_null_group);
  auto key_col =
    cudf::make_fixed_width_column(key_type, total_rows, cudf::mask_state::UNALLOCATED, stream, mr);
  auto value_col = cudf::make_fixed_width_column(
    cudf::data_type{cudf::type_id::INT64}, total_rows, cudf::mask_state::UNALLOCATED, stream, mr);
  std::unique_ptr<cudf::column> divisor_col;
  if (op == dense_value_op::AVG) {
    divisor_col = cudf::make_fixed_width_column(
      cudf::data_type{cudf::type_id::INT64}, total_rows, cudf::mask_state::UNALLOCATED, stream, mr);
  }

  auto key_out   = key_col->mutable_view().template data<KeyT>();
  auto value_out = value_col->mutable_view().template data<int64_t>();
  if (num_groups > 0) {
    rmm::device_uvector<int64_t> selected(static_cast<std::size_t>(group_rows), stream, mr);
    thrust::copy_if(policy, begin, end, selected.begin(), selector);

    auto const grid = grid_size_for(num_groups);
    switch (op) {
      case dense_value_op::COUNT:
        emit_raw_slot_kernel<KeyT, MatchedT><<<grid, k_block_size, 0, stream.value()>>>(
          selected.data(), num_groups, matched.data(), min_key, key_out, value_out);
        break;
      case dense_value_op::SUM:
      case dense_value_op::MIN:
      case dense_value_op::MAX:
        emit_raw_slot_kernel<KeyT, int64_t><<<grid, k_block_size, 0, stream.value()>>>(
          selected.data(), num_groups, payload->data(), min_key, key_out, value_out);
        break;
      case dense_value_op::AVG:
        emit_raw_slot_kernel<KeyT, int64_t><<<grid, k_block_size, 0, stream.value()>>>(
          selected.data(), num_groups, payload->data(), min_key, key_out, value_out);
        emit_raw_slot_kernel<KeyT, MatchedT><<<grid, k_block_size, 0, stream.value()>>>(
          selected.data(),
          num_groups,
          matched.data(),
          min_key,
          static_cast<KeyT*>(nullptr),
          divisor_col->mutable_view().template data<int64_t>());
        break;
    }
    CUDF_CUDA_TRY(cudaGetLastError());
  }

  if (has_null_group) {
    write_null_group_key_kernel<KeyT><<<1, 1, 0, stream.value()>>>(key_out, group_rows);
    auto write_value = [&](int64_t* out, auto const* slot) {
      using SlotT = std::remove_const_t<std::remove_pointer_t<decltype(slot)>>;
      write_null_group_value_kernel<SlotT><<<1, 1, 0, stream.value()>>>(out, group_rows, slot);
    };
    switch (op) {
      case dense_value_op::COUNT: write_value(value_out, matched.data() + range); break;
      case dense_value_op::SUM:
      case dense_value_op::MIN:
      case dense_value_op::MAX: write_value(value_out, payload->data() + range); break;
      case dense_value_op::AVG:
        write_value(value_out, payload->data() + range);
        write_value(divisor_col->mutable_view().template data<int64_t>(), matched.data() + range);
        break;
    }
    CUDF_CUDA_TRY(cudaGetLastError());

    auto mask = cudf::create_null_mask(total_rows, cudf::mask_state::ALL_VALID, stream, mr);
    cudf::set_null_mask(
      static_cast<cudf::bitmask_type*>(mask.data()), group_rows, group_rows + 1, false, stream);
    key_col->set_null_mask(std::move(mask), 1);
  }

  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(std::move(key_col));
  columns.push_back(std::move(value_col));
  if (divisor_col) { columns.push_back(std::move(divisor_col)); }
  return std::make_unique<cudf::table>(std::move(columns));
}

// Instantiation whitelist: the COUNT bundle only. A bundle/key combination absent from this list
// does not exist; extending the framework means specializing bundle_policies above and adding
// entries here.
template class group_join_state<int32_t, groupjoin::count_bundle<uint32_t>>;
template class group_join_state<int32_t, groupjoin::count_bundle<uint64_t>>;
template class group_join_state<int64_t, groupjoin::count_bundle<uint32_t>>;
template class group_join_state<int64_t, groupjoin::count_bundle<uint64_t>>;

// Instantiation whitelist for the INNER/DIRECT dense drivers. Each driver internally covers the
// COUNT/SUM/MIN/MAX/AVG ops; PresenceT derives from preserved rows, MatchedT from counted rows,
// and ArgT is the argument column's integer representation (INT32/DECIMAL32 -> int32_t,
// INT64/DECIMAL64 -> int64_t).
#define SIRIUS_GROUP_JOIN_INSTANTIATE_INNER(KeyT, PresenceT, MatchedT, ArgT)                     \
  template std::unique_ptr<cudf::table> group_join_dense_inner<KeyT, PresenceT, MatchedT, ArgT>( \
    groupjoin::dense_value_op,                                                                   \
    int64_t,                                                                                     \
    int64_t,                                                                                     \
    std::vector<cudf::column_view> const&,                                                       \
    std::vector<cudf::column_view> const&,                                                       \
    std::vector<cudf::column_view> const&,                                                       \
    cudf::data_type,                                                                             \
    bool,                                                                                        \
    rmm::cuda_stream_view,                                                                       \
    rmm::device_async_resource_ref)

#define SIRIUS_GROUP_JOIN_INSTANTIATE_DIRECT(KeyT, MatchedT, ArgT)                     \
  template std::unique_ptr<cudf::table> group_join_dense_direct<KeyT, MatchedT, ArgT>( \
    groupjoin::dense_value_op,                                                         \
    int64_t,                                                                           \
    int64_t,                                                                           \
    std::vector<cudf::column_view> const&,                                             \
    std::vector<cudf::column_view> const&,                                             \
    cudf::data_type,                                                                   \
    bool,                                                                              \
    rmm::cuda_stream_view,                                                             \
    rmm::device_async_resource_ref)

SIRIUS_GROUP_JOIN_INSTANTIATE_INNER(int32_t, uint32_t, uint32_t, int32_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_INNER(int32_t, uint32_t, uint32_t, int64_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_INNER(int32_t, uint32_t, uint64_t, int32_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_INNER(int32_t, uint32_t, uint64_t, int64_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_INNER(int32_t, uint64_t, uint32_t, int32_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_INNER(int32_t, uint64_t, uint32_t, int64_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_INNER(int32_t, uint64_t, uint64_t, int32_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_INNER(int32_t, uint64_t, uint64_t, int64_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_INNER(int64_t, uint32_t, uint32_t, int32_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_INNER(int64_t, uint32_t, uint32_t, int64_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_INNER(int64_t, uint32_t, uint64_t, int32_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_INNER(int64_t, uint32_t, uint64_t, int64_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_INNER(int64_t, uint64_t, uint32_t, int32_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_INNER(int64_t, uint64_t, uint32_t, int64_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_INNER(int64_t, uint64_t, uint64_t, int32_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_INNER(int64_t, uint64_t, uint64_t, int64_t);

SIRIUS_GROUP_JOIN_INSTANTIATE_DIRECT(int32_t, uint32_t, int32_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_DIRECT(int32_t, uint32_t, int64_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_DIRECT(int32_t, uint64_t, int32_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_DIRECT(int32_t, uint64_t, int64_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_DIRECT(int64_t, uint32_t, int32_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_DIRECT(int64_t, uint32_t, int64_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_DIRECT(int64_t, uint64_t, int32_t);
SIRIUS_GROUP_JOIN_INSTANTIATE_DIRECT(int64_t, uint64_t, int64_t);

#undef SIRIUS_GROUP_JOIN_INSTANTIATE_INNER
#undef SIRIUS_GROUP_JOIN_INSTANTIATE_DIRECT

void throw_if_count_product_overflows(cudf::column_view const& lhs,
                                      cudf::column_view const& rhs,
                                      rmm::cuda_stream_view stream,
                                      rmm::device_async_resource_ref mr)
{
  if (lhs.type().id() != cudf::type_id::INT64 || rhs.type().id() != cudf::type_id::INT64) {
    throw sirius::internal_exception(
      "group_join: overflow validation requires two INT64 columns, got {} and {}",
      static_cast<int32_t>(lhs.type().id()),
      static_cast<int32_t>(rhs.type().id()));
  }
  if (lhs.size() != rhs.size()) {
    throw sirius::internal_exception(
      "group_join: overflow validation column sizes differ ({} versus {})", lhs.size(), rhs.size());
  }
  if (lhs.null_count() != 0 || rhs.null_count() != 0) {
    throw sirius::internal_exception(
      "group_join: overflow validation requires non-NULL count columns");
  }
  if (lhs.size() == 0) { return; }

  cudf::numeric_scalar<int32_t> status(0, true, stream, mr);
  validate_product_kernel<<<grid_size_for(lhs.size()), k_block_size, 0, stream.value()>>>(
    lhs.data<int64_t>(), rhs.data<int64_t>(), static_cast<int64_t>(lhs.size()), status.data());
  CUDF_CUDA_TRY(cudaGetLastError());

  // This rare-path validation synchronizes only when the host bound cannot prove safety.
  auto const result = status.value(stream);
  if (result == 2) {
    throw sirius::internal_exception("group_join: aggregate multiplicities must be nonnegative");
  }
  if (result == 1) {
    throw sirius::invalid_input_exception("group_join: COUNT result exceeds BIGINT max {}",
                                          k_bigint_max);
  }
}

std::unique_ptr<cudf::table> group_join_empty_output(cudf::data_type key_type,
                                                     bool count_star,
                                                     int64_t null_group_rows,
                                                     rmm::cuda_stream_view stream,
                                                     rmm::device_async_resource_ref mr)
{
  if (null_group_rows < 0) {
    throw sirius::internal_exception("group_join: negative NULL-group row count");
  }
  cudf::size_type const total_rows = null_group_rows > 0 ? 1 : 0;
  auto key_col =
    cudf::make_fixed_width_column(key_type, total_rows, cudf::mask_state::UNALLOCATED, stream, mr);
  auto value_col = cudf::make_fixed_width_column(
    cudf::data_type{cudf::type_id::INT64}, total_rows, cudf::mask_state::UNALLOCATED, stream, mr);
  if (total_rows == 1) {
    write_null_group_row(*key_col,
                         *value_col,
                         /*row_idx=*/0,
                         count_star,
                         null_group_rows,
                         stream,
                         mr);
  }
  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(std::move(key_col));
  columns.push_back(std::move(value_col));
  return std::make_unique<cudf::table>(std::move(columns));
}

}  // namespace sirius::op
