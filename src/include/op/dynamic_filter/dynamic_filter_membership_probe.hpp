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

#include <cudf/types.hpp>

#include <cstddef>
#include <cstdint>

namespace sirius::op {

/// Membership structure one fused probe step reads
enum class membership_probe_kind : std::uint8_t { bloom, hash_set, small_in_list };

/// Outcome of asking a membership filter for a fused device probe
enum class device_probe_status : std::uint8_t {
  /// The filter kind has no fused form; apply it through `compute_mask_if()`.
  unsupported,
  /// The filter cannot serve this probe on this device (incompatible carrier, no local replica):
  /// the same condition under which `compute_mask_if()` returns null.
  unservable,
  /// The descriptor is filled in.
  ready,
};

/**
 * @brief Type-erased, device-copyable description of one membership probe step
 *
 * Filled by `sirius_mask_applicable::device_probe()` and consumed by the fused mask kernel, which
 * reinterprets `ref` as the concrete device reference selected by `kind` and `key_type`
 * (`cuda/dynamic_filter_membership_probe.cuh`). Plain data: the kernel receives an array of these
 * as its argument. Valid only while the filter's replica and the probe column's storage are.
 */
struct membership_probe {
  /// Room for the largest device reference; each kind checks its own at compile time.
  static constexpr std::size_t k_ref_bytes = 128;

  membership_probe_kind kind = membership_probe_kind::bloom;
  /// INT32 or INT64: the filter's key type. Probe values are widened to it before probing.
  cudf::type_id key_type = cudf::type_id::EMPTY;
  /// INT8, INT16, INT32 or INT64: the probe column's stored carrier (a value-preserving narrower
  /// carrier of `key_type`, or `key_type` itself).
  cudf::type_id carrier = cudf::type_id::EMPTY;
  /// First probe element, with the column offset already applied.
  void const* data = nullptr;
  /// Probe validity bitmask, or null when every row is valid; bit `null_offset + row`.
  cudf::bitmask_type const* null_mask = nullptr;
  cudf::size_type null_offset         = 0;
  /// Trivially-copyable device reference of the membership structure.
  alignas(16) unsigned char ref[k_ref_bytes] = {};
};

}  // namespace sirius::op
