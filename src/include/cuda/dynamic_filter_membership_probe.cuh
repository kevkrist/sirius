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

// Device side of the membership probe descriptor: the concrete cuco container types every
// membership filter kind is built on, their device references, and the per-row dispatch the fused
// mask kernel runs. Included only by CUDA translation units; hosts see
// op/dynamic_filter/dynamic_filter_membership_probe.hpp.

#include <cudf/column/column_view.hpp>
#include <cudf/types.hpp>
#include <cudf/utilities/bit.hpp>

#include <cuco/bloom_filter.cuh>
#include <cuco/hash_functions.cuh>
#include <cuco/operator.hpp>
#include <cuco/static_set.cuh>
#include <cuco/storage.cuh>
#include <cuda/sirius_rmm_cuco_allocator.cuh>
#include <cuda/std/bit>
#include <cuda/std/cstddef>
#include <cuda/std/functional>
#include <cuda/std/limits>

#include <op/dynamic_filter/dynamic_filter_membership_probe.hpp>

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

namespace sirius::op {

//===----------------------------------------------------------------------===//
// Bloom filter (sirius_dynamic_bloom_filter)
//===----------------------------------------------------------------------===//

/**
 * @brief cuco-compatible Bloom policy using Lemire fast-range
 *
 * Arrow's policy caps filter size, while cuco's default uses costly 64-bit modulo. Construction
 * and lookup share this mapping, preserving the no-false-negative contract.
 */
template <class KeyT>
class sirius_bloom_policy {
 public:
  using hasher             = cuco::xxhash_64<KeyT>;
  using word_type          = std::uint32_t;
  using hash_argument_type = typename hasher::argument_type;
  using hash_result_type   = decltype(std::declval<hasher>()(std::declval<hash_argument_type>()));

  static constexpr std::uint32_t words_per_block = 8;

 private:
  static constexpr std::uint32_t word_bits       = cuda::std::numeric_limits<word_type>::digits;
  static constexpr std::uint32_t bit_index_width = cuda::std::bit_width(word_bits - 1);
  static constexpr word_type bit_index_mask      = (word_type{1} << bit_index_width) - 1;

  static_assert(words_per_block * bit_index_width <=
                  cuda::std::numeric_limits<hash_result_type>::digits,
                "hash is too narrow to supply one fingerprint bit per word");

 public:
  __device__ constexpr hash_result_type hash(hash_argument_type const& key) const
  {
    return hash_(key);
  }

  template <class Extent>
  [[nodiscard]] __device__ constexpr Extent block_index(hash_result_type hash,
                                                        Extent num_blocks) const
  {
    auto const wide = static_cast<__uint128_t>(static_cast<std::uint64_t>(hash)) *
                      static_cast<__uint128_t>(static_cast<std::uint64_t>(num_blocks));
    return static_cast<Extent>(static_cast<std::uint64_t>(wide >> 64));
  }

  [[nodiscard]] __device__ constexpr word_type word_pattern(hash_result_type hash,
                                                            std::uint32_t word_index) const
  {
    return word_type{1} << ((hash >> (word_index * bit_index_width)) & bit_index_mask);
  }

 private:
  hasher hash_{};
};

using bloom_alloc = sirius::rmm_cuco_allocator<cuda::std::byte>;

template <class KeyT>
using sirius_bloom = cuco::bloom_filter<KeyT,
                                        cuco::extent<std::size_t>,
                                        cuda::thread_scope_device,
                                        sirius_bloom_policy<KeyT>,
                                        bloom_alloc>;

/// Non-owning device reference of a `sirius_bloom<KeyT>` (`contains(KeyT)`).
template <class KeyT>
using sirius_bloom_ref = decltype(std::declval<sirius_bloom<KeyT> const&>().ref());

//===----------------------------------------------------------------------===//
// Exact hash set (sirius_dynamic_in_list_filter)
//===----------------------------------------------------------------------===//

/// Threads per key probe. cuco::static_set requires 1 for device_for bulk iteration.
constexpr std::size_t k_in_list_cg_size = 1;

/// Keys per bucket. Sized for a double-hashed probe, where each step is a random fetch and a
/// wider bucket retires several dependent fetches in one; the right value depends on the probing
/// scheme, not on the key type.
constexpr std::int32_t k_in_list_bucket_size = 4;

template <class KeyT>
using in_list_set_alloc = sirius::rmm_cuco_allocator<KeyT>;

template <class KeyT>
using in_list_set =
  cuco::static_set<KeyT,
                   cuco::extent<std::size_t>,
                   cuda::thread_scope_device,
                   cuda::std::equal_to<KeyT>,
                   cuco::double_hashing<k_in_list_cg_size, cuco::default_hash_function<KeyT>>,
                   in_list_set_alloc<KeyT>,
                   cuco::storage<k_in_list_bucket_size>>;

/// Non-owning `contains` reference of an `in_list_set<KeyT>`.
template <class KeyT>
using in_list_set_ref = decltype(std::declval<in_list_set<KeyT> const&>().ref(cuco::contains));

/// The set cannot store this value; probes equal to it are kept (no false negatives).
template <class KeyT>
[[nodiscard]] __host__ __device__ constexpr KeyT in_list_sentinel() noexcept
{
  return cuda::std::numeric_limits<KeyT>::min();
}

//===----------------------------------------------------------------------===//
// Small exact list (sirius_dynamic_small_in_list_filter)
//===----------------------------------------------------------------------===//

/**
 * @brief Brute-force membership over a few device-resident needles
 *
 * For the handful of keys this kind gates on, a compare-all scan beats a hash probe and reserves
 * no sentinel value. The needles live in device memory (L1/L2-resident after the first row).
 */
template <class KeyT>
struct small_in_list_ref {
  KeyT const* needles = nullptr;
  int count           = 0;

  [[nodiscard]] __device__ __forceinline__ bool contains(KeyT key) const noexcept
  {
    bool hit = false;
    for (int j = 0; j < count; ++j) {
      hit |= (key == needles[j]);
    }
    return hit;
  }
};

//===----------------------------------------------------------------------===//
// Descriptor filling (host) and per-row dispatch (device)
//===----------------------------------------------------------------------===//

/// Copies a trivially-copyable device reference into the descriptor's storage.
template <class Ref>
void store_probe_ref(membership_probe& probe, Ref const& ref) noexcept
{
  static_assert(std::is_trivially_copyable_v<Ref>,
                "membership device references must be trivially copyable");
  static_assert(sizeof(Ref) <= membership_probe::k_ref_bytes,
                "membership_probe::k_ref_bytes is too small for this device reference");
  static_assert(alignof(Ref) <= 16, "membership_probe::ref is 16-byte aligned");
  std::memcpy(probe.ref, &ref, sizeof(Ref));
}

/// Reads back a reference stored by store_probe_ref() with the same type.
template <class Ref>
[[nodiscard]] __device__ __forceinline__ Ref const& load_probe_ref(
  membership_probe const& probe) noexcept
{
  // The bytes were written from an object of exactly this trivially-copyable type and the
  // storage is at least as aligned as the type requires (checked in store_probe_ref).
  return *reinterpret_cast<Ref const*>(probe.ref);
}

/**
 * @brief Records @p probe's storage in @p out
 *
 * @pre `detail::probe_carrier_compatible(probe.type(), key_type)`
 */
inline void describe_probe_column(membership_probe& out,
                                  cudf::column_view const& probe,
                                  cudf::type_id key_type) noexcept
{
  out.key_type = key_type;
  out.carrier  = probe.type().id();
  switch (probe.type().id()) {
    case cudf::type_id::INT8: out.data = probe.data<std::int8_t>(); break;
    case cudf::type_id::INT16: out.data = probe.data<std::int16_t>(); break;
    case cudf::type_id::INT32: out.data = probe.data<std::int32_t>(); break;
    default: out.data = probe.data<std::int64_t>(); break;
  }
  // Same rule as compute_mask_if(): a null mask travels only when it marks a null row.
  bool const has_nulls = probe.nullable() && probe.null_count() > 0;
  out.null_mask        = has_nulls ? probe.null_mask() : nullptr;
  out.null_offset      = probe.offset();
}

/// Widens the probe value at @p row to INT64 (value-preserving by the carrier contract).
[[nodiscard]] __device__ __forceinline__ std::int64_t load_probe_key(membership_probe const& probe,
                                                                     cudf::size_type row) noexcept
{
  switch (probe.carrier) {
    case cudf::type_id::INT8: return static_cast<std::int8_t const*>(probe.data)[row];
    case cudf::type_id::INT16: return static_cast<std::int16_t const*>(probe.data)[row];
    case cudf::type_id::INT32: return static_cast<std::int32_t const*>(probe.data)[row];
    default: return static_cast<std::int64_t const*>(probe.data)[row];
  }
}

/// Membership answer of @p probe for @p key, in the filter's key type.
template <class KeyT>
[[nodiscard]] __device__ __forceinline__ bool membership_probe_contains(
  membership_probe const& probe, KeyT key) noexcept
{
  switch (probe.kind) {
    case membership_probe_kind::bloom:
      return load_probe_ref<sirius_bloom_ref<KeyT>>(probe).contains(key);
    case membership_probe_kind::hash_set:
      return load_probe_ref<in_list_set_ref<KeyT>>(probe).contains(key) ||
             key == in_list_sentinel<KeyT>();
    case membership_probe_kind::small_in_list:
      return load_probe_ref<small_in_list_ref<KeyT>>(probe).contains(key);
  }
  return true;  // unknown kind: fail open, the join answers exactly
}

/**
 * @brief Whether @p row passes the membership step @p probe
 *
 * Exactly `compute_mask_if()`'s per-row verdict: a null probe row is `false`; otherwise the
 * carrier value widened to the key type is looked up in the filter's device-local replica.
 */
[[nodiscard]] __device__ __forceinline__ bool membership_probe_keeps(membership_probe const& probe,
                                                                     cudf::size_type row) noexcept
{
  if (probe.null_mask != nullptr && !cudf::bit_is_set(probe.null_mask, probe.null_offset + row)) {
    return false;
  }
  auto const key = load_probe_key(probe, row);
  return probe.key_type == cudf::type_id::INT64
           ? membership_probe_contains<std::int64_t>(probe, key)
           : membership_probe_contains<std::int32_t>(probe, static_cast<std::int32_t>(key));
}

}  // namespace sirius::op
