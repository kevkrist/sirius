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

#include <cudf/column/column_view.hpp>
#include <cudf/table/table.hpp>
#include <cudf/types.hpp>

#include <rmm/cuda_stream_view.hpp>
#include <rmm/device_uvector.hpp>
#include <rmm/resource_ref.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

/**
 * @file
 * @brief Host interface to the GROUPJOIN dense-strategy device state.
 *
 * `sirius_physical_group_join::execute` drives these entry points; the kernels and the
 * whitelisted `group_join_state` instantiations live in `src/cuda/group_join_impl.cu`. A bundle
 * names the fixed tuple of per-key slot arrays one accumulate pass updates; each bundle tag
 * declared here pairs with a device-side slot policy in the `.cu` and must appear on that file's
 * explicit-instantiation whitelist before it is usable.
 */

namespace sirius::op {

namespace groupjoin {

/// COUNT bundle: one matched-count array of CountT per key, plus the shared presence array.
template <typename CountT>
struct count_bundle {
  using count_type = CountT;
};

/// Aggregate selector for the INNER/DIRECT dense drivers. COUNT covers both COUNT(*) and
/// COUNT(col): the argument-validity gate admits only NULL-free argument columns to the dense
/// strategy, under which the two are identical. AVG shares SUM's accumulator arrays and differs
/// only at emit, where it produces the raw per-key sum and match count for a host-side divide.
enum class dense_value_op : uint8_t { COUNT, SUM, MIN, MAX, AVG };

}  // namespace groupjoin

/** @brief Find global non-NULL extrema across INT32 or INT64 key batches.
 *
 * Returns std::nullopt for empty/all-NULL input and synchronizes @p stream once.
 */
std::optional<std::pair<int64_t, int64_t>> group_join_global_minmax(
  std::vector<cudf::column_view> const& keys,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr);

/// Key extrema plus, when a SUM/AVG bundle supplied argument batches, the argument extrema that
/// feed the host-side int64 accumulation-overflow bound.
struct group_join_extrema {
  int64_t key_min;
  int64_t key_max;
  bool has_value_extrema;  ///< False when @p values held no non-NULL row.
  int64_t value_min;
  int64_t value_max;
};

/** @brief Find key and aggregate-argument extrema in one device pass with a single sync.
 *
 * The SUM/AVG counterpart of `group_join_global_minmax`: @p values are the argument columns
 * viewed as their INT32/INT64 representations, reduced into the same device extrema array and
 * read back in the same memcpy. Returns std::nullopt when @p keys hold no non-NULL row.
 */
std::optional<group_join_extrema> group_join_global_minmax_with_values(
  std::vector<cudf::column_view> const& keys,
  std::vector<cudf::column_view> const& values,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr);

/** @brief Dense INNER-form drive: accumulate both sides and emit groups with presence and
 * matches.
 *
 * Array widths follow the per-array rule: PresenceT from preserved rows, MatchedT from counted
 * rows; the aggregate payload is always int64. @p counted_args are the argument columns viewed as
 * ArgT (their integer representation), batch-aligned with @p counted_keys; pass an empty vector
 * for COUNT. Emits `[key, value:int64]` for COUNT (presence x matched, optionally
 * overflow-checked), SUM (presence x sum), and MIN/MAX (raw extreme), and
 * `[key, sum:int64, matched:int64]` for AVG. The argument-validity gate must hold: argument
 * columns must have no NULLs.
 */
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
  rmm::device_async_resource_ref mr);

/** @brief Dense DIRECT-form drive: plain GROUP BY over one input.
 *
 * No presence array or preserved pass exists; NULL keys accumulate into one extra slot at index
 * @p range and, when @p has_null_group is set, emit as a final NULL-masked key row. The caller
 * supplies @p has_null_group from its host-side NULL-key row count so this driver never reads
 * the slot back (it would cost a second unconditional stream sync). Output layout matches
 * `group_join_dense_inner` with COUNT emitting the raw match count and SUM the raw sum (no
 * presence scaling exists on this form).
 */
template <typename KeyT, typename MatchedT, typename ArgT>
std::unique_ptr<cudf::table> group_join_dense_direct(groupjoin::dense_value_op op,
                                                     int64_t min_key,
                                                     int64_t range,
                                                     std::vector<cudf::column_view> const& keys,
                                                     std::vector<cudf::column_view> const& args,
                                                     cudf::data_type key_type,
                                                     bool has_null_group,
                                                     rmm::cuda_stream_view stream,
                                                     rmm::device_async_resource_ref mr);

/** @brief Accumulate preserved-key multiplicities and per-key bundle slots in direct-address
 * arrays.
 *
 * KeyT must match the key columns' element type; Bundle selects the slot arrays and their device
 * update policy.
 */
template <typename KeyT, typename Bundle>
class group_join_state {
 public:
  using count_type = typename Bundle::count_type;

  group_join_state(int64_t min_key,
                   int64_t range,
                   rmm::cuda_stream_view stream,
                   rmm::device_async_resource_ref mr);

  /** @brief Accumulate non-NULL preserved keys, which must lie in the state domain. */
  void accumulate_preserved(cudf::column_view const& keys, rmm::cuda_stream_view stream);

  /** @brief Accumulate in-domain counted keys; nullptr validity applies COUNT(*) semantics. */
  void accumulate_counted(cudf::column_view const& keys,
                          cudf::column_view const* count_validity_source,
                          rmm::cuda_stream_view stream);

  /** @brief Emit `[key, BIGINT count]`, optionally checking products for BIGINT overflow. */
  std::unique_ptr<cudf::table> emit(cudf::data_type key_type,
                                    bool count_star,
                                    int64_t null_group_rows,
                                    rmm::cuda_stream_view stream,
                                    rmm::device_async_resource_ref mr,
                                    bool check_product_overflow) const;

  [[nodiscard]] int64_t min_key() const noexcept { return _min_key; }
  [[nodiscard]] int64_t range() const noexcept { return _range; }

 private:
  /// Validates the range against allocation capacity and returns the per-array slot count.
  [[nodiscard]] static std::size_t checked_slots(int64_t range);

  int64_t _min_key;
  int64_t _range;
  rmm::device_uvector<count_type> _presence;
  rmm::device_uvector<count_type> _matched;
};

/** @brief Build the NULL-group-only or empty output when no non-NULL preserved key exists. */
std::unique_ptr<cudf::table> group_join_empty_output(cudf::data_type key_type,
                                                     bool count_star,
                                                     int64_t null_group_rows,
                                                     rmm::cuda_stream_view stream,
                                                     rmm::device_async_resource_ref mr);

/** @brief Validate equal-length, non-null INT64 products against BIGINT overflow.
 *
 * Synchronizes @p stream to read the result.
 */
void throw_if_count_product_overflows(cudf::column_view const& lhs,
                                      cudf::column_view const& rhs,
                                      rmm::cuda_stream_view stream,
                                      rmm::device_async_resource_ref mr);

}  // namespace sirius::op
