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
#include <cudf/types.hpp>

#include <thrust/iterator/transform_iterator.h>

#include <cstdint>
#include <type_traits>

namespace sirius::op::detail {

/**
 * @brief Whether @p probe can be probed against a membership filter keyed on @p key
 *
 * The key type itself, or any narrower signed-integer carrier of it. Compressed materialization
 * stores the same values in a narrower carrier (no offset, no scale change), so widening a probe
 * value with a plain conversion reproduces the native key exactly.
 */
[[nodiscard]] constexpr bool probe_carrier_compatible(cudf::data_type probe,
                                                      cudf::data_type key) noexcept
{
  using cudf::type_id;
  if (probe == key) { return true; }
  if (key.id() != type_id::INT32 && key.id() != type_id::INT64) { return false; }
  switch (probe.id()) {
    case type_id::INT8:
    case type_id::INT16: return true;
    case type_id::INT32: return key.id() == type_id::INT64;
    default: return false;
  }
}

/// Widens one probe value to the key type. Value-preserving by the carrier contract above.
template <class KeyT, class ProbeT>
struct widen_probe {
  __host__ __device__ constexpr KeyT operator()(ProbeT value) const noexcept
  {
    return static_cast<KeyT>(value);
  }
};

/**
 * @brief Calls `f(ProbeT const* data)` with the probe's element type when it is compatible with
 *        @p KeyT (see probe_carrier_compatible); returns false without calling @p f otherwise.
 */
template <class KeyT, class F>
bool dispatch_probe_carrier(cudf::column_view const& probe, F&& f)
{
  static_assert(std::is_same_v<KeyT, std::int32_t> || std::is_same_v<KeyT, std::int64_t>);
  switch (probe.type().id()) {
    case cudf::type_id::INT8: f(probe.data<std::int8_t>()); return true;
    case cudf::type_id::INT16: f(probe.data<std::int16_t>()); return true;
    case cudf::type_id::INT32: f(probe.data<std::int32_t>()); return true;
    case cudf::type_id::INT64:
      if constexpr (std::is_same_v<KeyT, std::int64_t>) {
        f(probe.data<std::int64_t>());
        return true;
      }
      return false;
    default: return false;
  }
}

/**
 * @brief Calls `f(first, last)` with a random-access iterator range yielding @p KeyT values over
 *        @p probe, widening narrower carriers on the fly. Returns false when incompatible.
 */
template <class KeyT, class F>
bool with_probe_as_key(cudf::column_view const& probe, F&& f)
{
  auto const n = probe.size();
  return dispatch_probe_carrier<KeyT>(probe, [&](auto const* data) {
    using probe_type = std::remove_cv_t<std::remove_pointer_t<decltype(data)>>;
    if constexpr (std::is_same_v<probe_type, KeyT>) {
      f(data, data + n);
    } else {
      auto const first = thrust::make_transform_iterator(data, widen_probe<KeyT, probe_type>{});
      f(first, first + n);
    }
  });
}

}  // namespace sirius::op::detail
