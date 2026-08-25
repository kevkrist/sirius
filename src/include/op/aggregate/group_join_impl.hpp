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

}  // namespace groupjoin

/** @brief Find global non-NULL extrema across INT32 or INT64 key batches.
 *
 * Returns std::nullopt for empty/all-NULL input and synchronizes @p stream once.
 */
std::optional<std::pair<int64_t, int64_t>> group_join_global_minmax(
  std::vector<cudf::column_view> const& keys,
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
