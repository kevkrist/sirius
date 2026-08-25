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

template <typename CountT>
struct presence_positive {
  CountT const* presence;
  __device__ bool operator()(int64_t k) const { return presence[k] != CountT{0}; }
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

}  // namespace

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

// Instantiation whitelist: the COUNT bundle only. A bundle/key combination absent from this list
// does not exist; extending the framework means specializing bundle_policies above and adding
// entries here.
template class group_join_state<int32_t, groupjoin::count_bundle<uint32_t>>;
template class group_join_state<int32_t, groupjoin::count_bundle<uint64_t>>;
template class group_join_state<int64_t, groupjoin::count_bundle<uint32_t>>;
template class group_join_state<int64_t, groupjoin::count_bundle<uint64_t>>;

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
