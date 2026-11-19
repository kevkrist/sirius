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

/*
 * Prototype tooling: dynamic-filter Bloom microbenchmarks (MB1 insert, MB6 probe + measured FPR).
 *
 * The `prod` insert is `detail::accumulated_bloom_builder::enqueue_add` (cuco `add_if_async` with
 * the validity stencil) and the probe is `sirius_dynamic_bloom_filter::compute_mask`, i.e. the
 * calls the engine makes. Every other variant is a throwaway kernel prototype measured against it.
 *
 * Usage: df_bloom_bench suite=<selfcheck|insert|probe> [key=value ...]; prints `RES,...` rows.
 */

#include <cudf/column/column.hpp>
#include <cudf/column/column_view.hpp>
#include <cudf/null_mask.hpp>
#include <cudf/partitioning.hpp>
#include <cudf/table/table.hpp>
#include <cudf/table/table_view.hpp>
#include <cudf/types.hpp>

#include <rmm/cuda_device.hpp>
#include <rmm/cuda_stream.hpp>
#include <rmm/device_buffer.hpp>
#include <rmm/device_uvector.hpp>
#include <rmm/exec_policy.hpp>
#include <rmm/mr/cuda_memory_resource.hpp>

#include <cuco/bloom_filter.cuh>
#include <cuco/bloom_filter_policy.cuh>
#include <cuco/bloom_filter_ref.cuh>
#include <cuco/hash_functions.cuh>
#include <cuda/dynamic_filter_probe.cuh>
#include <cuda/stream>
#include <cuda_runtime_api.h>
#include <thrust/count.h>
#include <thrust/for_each.h>
#include <thrust/iterator/counting_iterator.h>
#include <thrust/sort.h>

#include <cucascade/memory/common.hpp>
#include <cucascade/memory/memory_space.hpp>
#include <cucascade/memory/reservation_manager_configurator.hpp>
#include <memory/sirius_memory_reservation_manager.hpp>
#include <op/dynamic_filter/detail/accumulated_bloom_builder.hpp>
#include <op/dynamic_filter/dynamic_filter_replica_space.hpp>
#include <op/dynamic_filter/sirius_dynamic_filter.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace op = sirius::op;
using builder = op::detail::accumulated_bloom_builder;
using clk     = std::chrono::steady_clock;

constexpr std::size_t mib = std::size_t{1} << 20;

void check(cudaError_t e, char const* what)
{
  if (e != cudaSuccess) {
    std::fprintf(stderr, "CUDA error %s: %s\n", what, cudaGetErrorString(e));
    std::abort();
  }
}

double ms_since(clk::time_point t0)
{
  return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

// ---------------------------------------------------------------------------------------------
// Key generation
// ---------------------------------------------------------------------------------------------

__host__ __device__ inline std::uint64_t mix64(std::uint64_t x)
{
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

struct key_spec {
  bool random            = true;  // mix64(index) (a bijection) or the index itself
  std::uint64_t distinct = 0;     // nonzero: index % distinct first (duplicates)
  __host__ __device__ std::uint64_t operator()(std::uint64_t index) const
  {
    auto const idx = distinct != 0 ? index % distinct : index;
    return random ? mix64(idx) : idx;
  }
};

template <class T>
void generate_keys(T* out, std::uint64_t base, std::int64_t n, key_spec spec, rmm::cuda_stream_view s)
{
  thrust::for_each_n(rmm::exec_policy_nosync(s),
                     thrust::counting_iterator<std::int64_t>{0},
                     n,
                     [=] __device__(std::int64_t i) {
                       out[i] = static_cast<T>(spec(base + static_cast<std::uint64_t>(i)));
                     });
}

// ~12.5% nulls, random positions.
void generate_validity(cudf::bitmask_type* words, std::int64_t n, std::uint64_t seed, rmm::cuda_stream_view s)
{
  auto const nwords = (n + 31) / 32;
  thrust::for_each_n(rmm::exec_policy_nosync(s),
                     thrust::counting_iterator<std::int64_t>{0},
                     nwords,
                     [=] __device__(std::int64_t w) {
                       auto const a = mix64(seed + 3 * static_cast<std::uint64_t>(w));
                       auto const b = mix64(seed + 3 * static_cast<std::uint64_t>(w) + 1);
                       words[w]     = static_cast<std::uint32_t>(a | (a >> 32) | b);
                     });
}

// ---------------------------------------------------------------------------------------------
// Prototype insert variants on a raw array with the production block layout
// ---------------------------------------------------------------------------------------------

template <class Key>
using ref_prod = cuco::bloom_filter_ref<Key,
                                        cuco::extent<std::size_t>,
                                        ::cuda::thread_scope_device,
                                        cuco::bloom_filter_policy<Key>>;
// ConditionalAdd = true (read before atomic).
template <class Key>
using policy_cond =
  cuco::bloom_filter_policy<Key, cuco::xxhash_64<Key>, 4, 8, 8, 8, 1, 1, 8, true, false, false>;
template <class Key>
using ref_cond =
  cuco::bloom_filter_ref<Key, cuco::extent<std::size_t>, ::cuda::thread_scope_device, policy_cond<Key>>;
// One thread per key doing the 8 atomics (no 8-thread tile, no shuffles).
template <class Key>
using policy_v8 =
  cuco::bloom_filter_policy<Key, cuco::xxhash_64<Key>, 4, 8, 8, 1, 8, 1, 8, false, false, false>;
template <class Key>
using ref_v8 =
  cuco::bloom_filter_ref<Key, cuco::extent<std::size_t>, ::cuda::thread_scope_device, policy_v8<Key>>;
template <class Key>
using policy_v8c =
  cuco::bloom_filter_policy<Key, cuco::xxhash_64<Key>, 4, 8, 8, 1, 8, 1, 8, true, false, false>;
template <class Key>
using ref_v8c =
  cuco::bloom_filter_ref<Key, cuco::extent<std::size_t>, ::cuda::thread_scope_device, policy_v8c<Key>>;

template <class Ref>
Ref make_ref(void* bits, std::size_t blocks)
{
  using block = typename Ref::filter_block_type;
  return Ref{static_cast<block*>(bits), cuco::extent<std::size_t>{blocks}, {}, {}};
}

// First 8 salts of cuco's policy: word j of a block gets bit (salt[j] * lower_hash) >> 27.
__device__ constexpr std::uint32_t k_salts[8] = {0x47b6137bU,
                                                 0x44974d91U,
                                                 0x8824ad5bU,
                                                 0xa2b7289dU,
                                                 0x705495c7U,
                                                 0x2df1424bU,
                                                 0x9efc4947U,
                                                 0x5c6bfb31U};

struct hashed {
  std::uint64_t block;
  std::uint32_t lower;
};

template <class Key>
__device__ __forceinline__ hashed hash_key(Key key, std::uint64_t blocks)
{
  auto const h = cuco::xxhash_64<Key>{}(key);
  return {(static_cast<std::uint64_t>(static_cast<std::uint32_t>(h >> 32)) * blocks) >> 32,
          static_cast<std::uint32_t>(h)};
}

__device__ __forceinline__ bool row_valid(std::uint32_t const* mask, std::int64_t i)
{
  return mask == nullptr || ((mask[i >> 5] >> (i & 31)) & 1U) != 0U;
}

// Same bits as production, written as 4 x 64-bit atomicOr (pairs of adjacent 32-bit words) by one
// thread per key. Mode 0: unconditional. Mode 1: per-word read first. Mode 2: one 32-byte read,
// atomics only for the 64-bit words that miss bits.
template <class Key, int Mode, int BlockSize>
__global__ void __launch_bounds__(BlockSize) add_w64_kernel(Key const* keys,
                                                           std::int64_t n,
                                                           std::uint32_t const* mask,
                                                           unsigned long long* words,
                                                           std::uint64_t blocks)
{
  auto const i = static_cast<std::int64_t>(blockIdx.x) * BlockSize + threadIdx.x;
  if (i >= n || !row_valid(mask, i)) { return; }
  auto const h = hash_key(keys[i], blocks);
  auto* w      = words + h.block * 4;
  unsigned long long p[4];
#pragma unroll
  for (int j = 0; j < 4; ++j) {
    p[j] = (1ULL << ((k_salts[2 * j] * h.lower) >> 27)) |
           (1ULL << (32 + ((k_salts[2 * j + 1] * h.lower) >> 27)));
  }
  if constexpr (Mode == 2) {
    auto const lo = *reinterpret_cast<ulonglong2 const*>(w);
    auto const hi = *reinterpret_cast<ulonglong2 const*>(w + 2);
    if ((lo.x & p[0]) != p[0]) { atomicOr(w + 0, p[0]); }
    if ((lo.y & p[1]) != p[1]) { atomicOr(w + 1, p[1]); }
    if ((hi.x & p[2]) != p[2]) { atomicOr(w + 2, p[2]); }
    if ((hi.y & p[3]) != p[3]) { atomicOr(w + 3, p[3]); }
  } else {
#pragma unroll
    for (int j = 0; j < 4; ++j) {
      if constexpr (Mode == 1) {
        if ((w[j] & p[j]) == p[j]) { continue; }
      }
      atomicOr(w + j, p[j]);
    }
  }
}

// Diagnostic only (racy, loses bits): non-atomic 32-byte load, OR, 32-byte store. Prices the
// atomics against a plain read-modify-write of the same sector.
template <class Key>
__global__ void __launch_bounds__(256) diag_rmw_kernel(Key const* keys,
                                                      std::int64_t n,
                                                      unsigned long long* words,
                                                      std::uint64_t blocks)
{
  auto const i = static_cast<std::int64_t>(blockIdx.x) * 256 + threadIdx.x;
  if (i >= n) { return; }
  auto const h = hash_key(keys[i], blocks);
  auto* w      = reinterpret_cast<ulonglong2*>(words + h.block * 4);
  unsigned long long p[4];
#pragma unroll
  for (int j = 0; j < 4; ++j) {
    p[j] = (1ULL << ((k_salts[2 * j] * h.lower) >> 27)) |
           (1ULL << (32 + ((k_salts[2 * j + 1] * h.lower) >> 27)));
  }
  auto lo = w[0];
  auto hi = w[1];
  lo.x |= p[0];
  lo.y |= p[1];
  hi.x |= p[2];
  hi.y |= p[3];
  w[0] = lo;
  w[1] = hi;
}

// Diagnostic only: the sector is read and nothing is written (the probe's access shape).
template <class Key>
__global__ void __launch_bounds__(256) diag_read_kernel(Key const* keys,
                                                       std::int64_t n,
                                                       unsigned long long* words,
                                                       std::uint64_t blocks)
{
  auto const i = static_cast<std::int64_t>(blockIdx.x) * 256 + threadIdx.x;
  if (i >= n) { return; }
  auto const h  = hash_key(keys[i], blocks);
  auto* w       = reinterpret_cast<ulonglong2*>(words + h.block * 4);
  auto const lo = w[0];
  auto const hi = w[1];
  // Never true for a zeroed array; keeps the loads alive.
  if ((lo.x & lo.y & hi.x & hi.y) == ~0ULL) { atomicOr(words, 1ULL); }
}

// Different block: one 64-bit word per key, 8 pattern bits, a single atomicOr (Mode 1: read
// first). NOT the production layout: the probe and the false-positive rate change.
template <class Key, int Mode>
__global__ void __launch_bounds__(256) add_blk64_kernel(Key const* keys,
                                                       std::int64_t n,
                                                       unsigned long long* words,
                                                       std::uint64_t num_words)
{
  auto const i = static_cast<std::int64_t>(blockIdx.x) * 256 + threadIdx.x;
  if (i >= n) { return; }
  auto const h = hash_key(keys[i], num_words);
  unsigned long long p = 0;
#pragma unroll
  for (int j = 0; j < 8; ++j) {
    p |= 1ULL << ((k_salts[j] * h.lower) >> 26);
  }
  if constexpr (Mode == 1) {
    if ((words[h.block] & p) == p) { return; }
  }
  atomicOr(words + h.block, p);
}

// Insert from precomputed (block << 32 | lower_hash) pairs: used by the bucket/sort pre-pass.
__global__ void __launch_bounds__(256) add_hashed_kernel(std::uint64_t const* pairs,
                                                        std::int64_t n,
                                                        unsigned long long* words)
{
  auto const i = static_cast<std::int64_t>(blockIdx.x) * 256 + threadIdx.x;
  if (i >= n) { return; }
  auto const lower = static_cast<std::uint32_t>(pairs[i]);
  auto* w          = words + (pairs[i] >> 32) * 4;
#pragma unroll
  for (int j = 0; j < 4; ++j) {
    atomicOr(w + j,
             (1ULL << ((k_salts[2 * j] * lower) >> 27)) |
               (1ULL << (32 + ((k_salts[2 * j + 1] * lower) >> 27))));
  }
}

template <class Key>
void compute_pairs(Key const* keys, std::int64_t n, std::uint64_t blocks, std::uint64_t* out, rmm::cuda_stream_view s)
{
  thrust::for_each_n(rmm::exec_policy_nosync(s),
                     thrust::counting_iterator<std::int64_t>{0},
                     n,
                     [=] __device__(std::int64_t i) {
                       auto const h = hash_key(keys[i], blocks);
                       out[i]       = (h.block << 32) | h.lower;
                     });
}

// ---------------------------------------------------------------------------------------------
// Environment: the same reservation manager / replica space the tests and the engine use
// ---------------------------------------------------------------------------------------------

struct environment {
  std::unique_ptr<sirius::memory::sirius_memory_reservation_manager> manager;
  std::vector<op::dynamic_filter_replica_space> spaces;
  rmm::cuda_stream task{rmm::cuda_stream::flags::non_blocking};
  rmm::cuda_stream second{rmm::cuda_stream::flags::non_blocking};
  rmm::cuda_stream publication{rmm::cuda_stream::flags::non_blocking};
  rmm::mr::cuda_memory_resource raw;  // benchmark inputs, outside the reservation accounting

  explicit environment(std::size_t gpu_bytes)
  {
    cucascade::memory::reservation_manager_configurator cfg;
    cfg.set_number_of_gpus(1)
      .set_gpu_usage_limit(gpu_bytes)
      .set_reservation_fraction_per_gpu(0.9)
      .set_per_numa_region_capacity(std::size_t{1} << 30)
      .use_gpu_id_as_host_id()
      .set_reservation_fraction_per_numa_region(0.75)
      .track_reservation_per_stream(false);
    manager     = std::make_unique<sirius::memory::sirius_memory_reservation_manager>(cfg.build());
    auto hosts  = manager->get_memory_spaces_for_tier(cucascade::memory::Tier::HOST);
    auto* space = manager->get_memory_space(cucascade::memory::Tier::GPU, 0);
    if (hosts.empty() || space == nullptr) { throw std::runtime_error("no memory spaces"); }
    spaces.emplace_back(*space, *hosts.front());
  }

  cucascade::memory::memory_space& gpu() const { return spaces.front().get_gpu_space(); }
  ::cuda::stream_ref task_ref() const { return ::cuda::stream_ref{task.value()}; }
  ::cuda::stream_ref second_ref() const { return ::cuda::stream_ref{second.value()}; }
};

// ---------------------------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------------------------

struct arguments {
  std::map<std::string, std::string> kv;
  std::string str(std::string const& k, std::string const& d) const
  {
    auto it = kv.find(k);
    return it == kv.end() ? d : it->second;
  }
  double num(std::string const& k, double d) const
  {
    auto it = kv.find(k);
    return it == kv.end() ? d : std::stod(it->second);
  }
};

struct stats {
  double median, min, max, sum;
  std::size_t n;
};

stats summarize(std::vector<double> v)
{
  if (v.empty()) { return {0, 0, 0, 0, 0}; }
  std::sort(v.begin(), v.end());
  return {v[v.size() / 2], v.front(), v.back(), std::accumulate(v.begin(), v.end(), 0.0), v.size()};
}

// ---------------------------------------------------------------------------------------------
// Insert (MB1)
// ---------------------------------------------------------------------------------------------

struct key_batch {
  rmm::device_buffer data;
  rmm::device_buffer mask;
  cudf::type_id type;
  cudf::size_type rows       = 0;
  cudf::size_type null_count = 0;
  bool nulls;

  cudf::column_view view() const
  {
    return cudf::column_view{cudf::data_type{type},
                             rows,
                             data.data(),
                             nulls ? static_cast<cudf::bitmask_type const*>(mask.data()) : nullptr,
                             null_count};
  }
};

template <class Key>
void fill_batch(key_batch& batch, std::uint64_t base, cudf::size_type rows, key_spec spec, rmm::cuda_stream_view s)
{
  batch.rows = rows;
  generate_keys(static_cast<Key*>(batch.data.data()), base, rows, spec, s);
  if (batch.nulls) {
    generate_validity(static_cast<cudf::bitmask_type*>(batch.mask.data()), rows, base * 31 + 7, s);
    batch.null_count =
      cudf::null_count(static_cast<cudf::bitmask_type const*>(batch.mask.data()), 0, rows, s);
  }
  s.synchronize();
}

template <class Key>
int run_insert(arguments const& args)
{
  auto const array_bytes = static_cast<std::size_t>(args.num("array_mib", 300) * mib);
  auto const bpk         = static_cast<std::size_t>(args.num("bpk", 16));
  auto const batch_rows  = static_cast<std::int64_t>(args.num("batch", 16e6));
  auto const dup         = static_cast<std::uint64_t>(args.num("dup", 1));
  auto const nulls       = args.num("nulls", 0) != 0;
  auto const variant     = args.str("variant", "prod");
  auto const mode        = args.str("mode", "alone");  // alone | hp_only | hp_same | hp_second
  auto const payload     = static_cast<int>(args.num("payload", 3));
  auto const parts       = static_cast<int>(args.num("parts", 32));
  auto const max_batches = static_cast<std::int64_t>(args.num("max_batches", 1e9));
  auto const label       = args.str("label", "");
  key_spec spec{args.str("order", "random") == "random", 0};

  auto const blocks     = array_bytes / 32;
  auto const total_rows = static_cast<std::uint64_t>(array_bytes) * 8 / bpk;
  if (dup > 1) { spec.distinct = std::max<std::uint64_t>(total_rows / dup, 1); }
  // At least `min_batches` batches: a small array is refilled with the same keys.
  auto const min_batches = static_cast<std::int64_t>(args.num("min_batches", 6));
  auto const batches     = std::min<std::int64_t>(
    max_batches,
    std::max<std::int64_t>(min_batches,
                           static_cast<std::int64_t>((total_rows + batch_rows - 1) / batch_rows)));

  environment env{static_cast<std::size_t>(args.num("gpu_gib", 80)) << 30};
  rmm::cuda_set_device_raii guard{rmm::cuda_device_id{0}};
  rmm::cuda_stream_view const task{env.task.value()};
  rmm::cuda_stream_view const second{env.second.value()};
  auto const type = sizeof(Key) == 8 ? cudf::type_id::INT64 : cudf::type_id::INT32;

  key_batch batch{rmm::device_buffer{static_cast<std::size_t>(batch_rows) * sizeof(Key), task, env.raw},
                  rmm::device_buffer{cudf::bitmask_allocation_size_bytes(batch_rows), task, env.raw},
                  type,
                  0,
                  0,
                  nulls};
  bool const with_hp = mode != "alone";
  std::vector<rmm::device_buffer> payload_cols;
  if (with_hp) {
    for (int c = 0; c < payload; ++c) {
      payload_cols.emplace_back(static_cast<std::size_t>(batch_rows) * 8, task, env.raw);
      check(cudaMemsetAsync(payload_cols.back().data(), c + 1, payload_cols.back().size(), task.value()),
            "memset payload");
    }
  }

  // Target array.
  auto const t_create = clk::now();
  std::optional<builder> accumulated;
  rmm::device_buffer raw_bits;
  bool const prod = variant == "prod" || variant == "sorted";
  if (mode != "hp_only") {
    if (prod) {
      accumulated = builder::try_create({{0, cudf::data_type{type}, blocks}}, env.spaces);
      if (!accumulated) { throw std::runtime_error("lease refused"); }
    } else {
      raw_bits = rmm::device_buffer{array_bytes, task, env.raw};
      check(cudaMemsetAsync(raw_bits.data(), 0, array_bytes, task.value()), "memset");
    }
  }
  check(cudaDeviceSynchronize(), "sync create");
  auto const create_ms = ms_since(t_create);

  rmm::device_uvector<std::uint64_t> pairs{0, task, env.raw};
  if (variant == "sortpairs" || variant == "pairs" || variant == "sorted") {
    pairs.resize(static_cast<std::size_t>(batch_rows), task);
  }
  auto const mr = env.gpu().get_default_allocator();
  auto* const words = static_cast<unsigned long long*>(raw_bits.data());

  std::vector<double> insert_ms;
  std::vector<double> prep_ms;
  std::vector<double> batch_row_counts;
  std::uint64_t rows_done = 0;
  std::uint64_t cursor    = 0;
  for (std::int64_t b = 0; b < batches; ++b) {
    if (cursor >= total_rows) { cursor = 0; }
    auto const rows = static_cast<cudf::size_type>(
      std::min<std::uint64_t>(static_cast<std::uint64_t>(batch_rows), total_rows - cursor));
    fill_batch<Key>(batch, cursor, rows, spec, task);
    cursor += static_cast<std::uint64_t>(rows);
    batch_row_counts.push_back(rows);
    auto const keys      = batch.view();
    auto const* key_data = static_cast<Key const*>(batch.data.data());
    auto const* mask     = nulls ? static_cast<std::uint32_t const*>(batch.mask.data()) : nullptr;
    auto const grid      = static_cast<unsigned>((rows + 255) / 256);

    // Untimed or separately timed pre-pass.
    if (variant == "sorted") {
      // Upper bound on locality: keys pre-sorted by block index (pre-pass timed in prep_ms).
      auto const t0 = clk::now();
      compute_pairs(key_data, rows, blocks, pairs.data(), task);
      thrust::sort_by_key(rmm::exec_policy_nosync(task),
                          pairs.begin(),
                          pairs.begin() + rows,
                          static_cast<Key*>(batch.data.data()));
      task.synchronize();
      prep_ms.push_back(ms_since(t0));
    } else if (variant == "sortpairs") {
      auto const t0 = clk::now();
      compute_pairs(key_data, rows, blocks, pairs.data(), task);
      thrust::sort(rmm::exec_policy_nosync(task), pairs.begin(), pairs.begin() + rows);
      task.synchronize();
      prep_ms.push_back(ms_since(t0));
    } else if (variant == "pairs") {
      auto const t0 = clk::now();
      compute_pairs(key_data, rows, blocks, pairs.data(), task);
      task.synchronize();
      prep_ms.push_back(ms_since(t0));
    }

    std::vector<cudf::column_view> columns{keys};
    for (auto const& col : payload_cols) {
      columns.emplace_back(cudf::data_type{cudf::type_id::INT64}, rows, col.data(), nullptr, 0);
    }
    cudf::table_view const table{columns};
    std::unique_ptr<cudf::table> scattered;

    auto const insert = [&](rmm::cuda_stream_view s) {
      ::cuda::stream_ref const sref{s.value()};
      if (prod) {
        if (!accumulated->enqueue_add(cudf::table_view{{keys}}, rmm::cuda_device_id{0}, sref)) {
          throw std::runtime_error("no partial");
        }
      } else if (variant == "cuco_addif") {
        make_ref<ref_prod<Key>>(raw_bits.data(), blocks)
          .add_if_async(key_data,
                        key_data + rows,
                        thrust::counting_iterator<cudf::size_type>{0},
                        op::detail::probe_validity_of(keys),
                        sref);
      } else if (variant == "cuco_add") {
        make_ref<ref_prod<Key>>(raw_bits.data(), blocks).add_async(key_data, key_data + rows, sref);
      } else if (variant == "cuco_cond") {
        make_ref<ref_cond<Key>>(raw_bits.data(), blocks)
          .add_if_async(key_data,
                        key_data + rows,
                        thrust::counting_iterator<cudf::size_type>{0},
                        op::detail::probe_validity_of(keys),
                        sref);
      } else if (variant == "cuco_v8") {
        make_ref<ref_v8<Key>>(raw_bits.data(), blocks)
          .add_if_async(key_data,
                        key_data + rows,
                        thrust::counting_iterator<cudf::size_type>{0},
                        op::detail::probe_validity_of(keys),
                        sref);
      } else if (variant == "cuco_v8c") {
        make_ref<ref_v8c<Key>>(raw_bits.data(), blocks)
          .add_if_async(key_data,
                        key_data + rows,
                        thrust::counting_iterator<cudf::size_type>{0},
                        op::detail::probe_validity_of(keys),
                        sref);
      } else if (variant == "w64") {
        add_w64_kernel<Key, 0, 256><<<grid, 256, 0, s.value()>>>(key_data, rows, mask, words, blocks);
      } else if (variant == "w64_1024") {
        add_w64_kernel<Key, 0, 1024>
          <<<static_cast<unsigned>((rows + 1023) / 1024), 1024, 0, s.value()>>>(
            key_data, rows, mask, words, blocks);
      } else if (variant == "w64c") {
        add_w64_kernel<Key, 1, 256><<<grid, 256, 0, s.value()>>>(key_data, rows, mask, words, blocks);
      } else if (variant == "w64v") {
        add_w64_kernel<Key, 2, 256><<<grid, 256, 0, s.value()>>>(key_data, rows, mask, words, blocks);
      } else if (variant == "diag_rmw") {
        diag_rmw_kernel<Key><<<grid, 256, 0, s.value()>>>(key_data, rows, words, blocks);
      } else if (variant == "diag_read") {
        diag_read_kernel<Key><<<grid, 256, 0, s.value()>>>(key_data, rows, words, blocks);
      } else if (variant == "blk64") {
        add_blk64_kernel<Key, 0><<<grid, 256, 0, s.value()>>>(key_data, rows, words, blocks * 4);
      } else if (variant == "blk64c") {
        add_blk64_kernel<Key, 1><<<grid, 256, 0, s.value()>>>(key_data, rows, words, blocks * 4);
      } else if (variant == "sortpairs" || variant == "pairs") {
        add_hashed_kernel<<<grid, 256, 0, s.value()>>>(pairs.data(), rows, words);
      } else {
        throw std::runtime_error("unknown variant " + variant);
      }
      check(cudaGetLastError(), "insert launch");
    };
    auto const scatter = [&] {
      scattered = std::move(
        cudf::hash_partition(table, {0}, parts, cudf::hash_id::HASH_MURMUR3, cudf::DEFAULT_HASH_SEED, task, mr)
          .first);
    };

    auto const t0 = clk::now();
    if (mode == "alone") {
      insert(task);
      task.synchronize();
    } else if (mode == "hp_only") {
      scatter();
      task.synchronize();
    } else if (mode == "hp_same") {
      insert(task);
      scatter();
      task.synchronize();
    } else if (mode == "hp_second") {
      insert(second);
      scatter();
      task.synchronize();
      second.synchronize();
    } else {
      throw std::runtime_error("unknown mode " + mode);
    }
    insert_ms.push_back(ms_since(t0));
    scattered.reset();
    rows_done += static_cast<std::uint64_t>(rows);
  }
  check(cudaDeviceSynchronize(), "sync end");

  // Population count of the array, as a cheap cross-variant correctness signal.
  std::uint64_t checksum = 0;
  if (args.num("checksum", 0) != 0 && mode != "hp_only") {
    std::vector<std::byte> host;
    if (prod) {
      auto contents = accumulated->inspect_partial(0, rmm::cuda_device_id{0});
      host          = std::move(contents->bytes);
    } else {
      host.resize(array_bytes);
      check(cudaMemcpy(host.data(), raw_bits.data(), array_bytes, cudaMemcpyDeviceToHost), "d2h");
    }
    std::uint64_t fnv = 1469598103934665603ULL;
    for (std::size_t i = 0; i + 8 <= host.size(); i += 8) {
      std::uint64_t w;
      std::memcpy(&w, host.data() + i, 8);
      fnv = (fnv ^ w) * 1099511628211ULL;
    }
    checksum = fnv;
  }

  // Drop the first batch (warm-up) when there are enough batches.
  std::vector<double> steady = insert_ms;
  if (steady.size() > 2) {
    steady.erase(steady.begin());
    batch_row_counts.erase(batch_row_counts.begin());
  }
  // Rate over the full-size steady batches only (a trailing partial batch is excluded).
  auto const full_rows = *std::max_element(batch_row_counts.begin(), batch_row_counts.end());
  std::vector<double> full;
  for (std::size_t i = 0; i < steady.size(); ++i) {
    if (batch_row_counts[i] == full_rows) { full.push_back(steady[i]); }
  }
  auto const st     = summarize(full);
  auto const all    = summarize(insert_ms);
  auto const prep   = summarize(prep_ms);
  auto const rows_s = full_rows / (st.median / 1e3);
  std::printf(
    "RES,insert,label=%s,variant=%s,mode=%s,key=%zu,array_mib=%.0f,bpk=%zu,batch=%lld,batch_rows=%.0f,nulls=%d,dup=%llu,"
    "order=%s,batches=%lld,rows=%llu,create_ms=%.2f,ms_batch_med=%.3f,ms_batch_min=%.3f,"
    "ms_batch_max=%.3f,total_ms=%.1f,Mrows_s=%.1f,keyGB_s=%.2f,sectorGB_s=%.2f,prep_ms_med=%.3f,"
    "checksum=%llx\n",
    label.c_str(),
    variant.c_str(),
    mode.c_str(),
    sizeof(Key) * 8,
    static_cast<double>(array_bytes) / mib,
    bpk,
    static_cast<long long>(batch_rows),
    full_rows,
    nulls ? 1 : 0,
    static_cast<unsigned long long>(dup),
    spec.random ? "random" : "seq",
    static_cast<long long>(batches),
    static_cast<unsigned long long>(rows_done),
    create_ms,
    st.median,
    st.min,
    st.max,
    all.sum,
    rows_s / 1e6,
    rows_s * sizeof(Key) / 1e9,
    rows_s * 32 / 1e9,
    prep.median,
    static_cast<unsigned long long>(checksum));
  std::fflush(stdout);
  return 0;
}

// ---------------------------------------------------------------------------------------------
// Probe + measured false-positive rate (MB6)
// ---------------------------------------------------------------------------------------------

struct probe_spec {
  std::uint64_t members;   // inserted keys are mix64(0 .. members-1)
  std::uint64_t base;      // per-repetition offset
  std::uint32_t permille;  // hit rate
  __host__ __device__ bool is_member(std::uint64_t i) const
  {
    return mix64((base + i) * 0x2545F4914F6CDD1DULL + 11) % 1000 < permille;
  }
  __host__ __device__ std::uint64_t operator()(std::uint64_t i) const
  {
    return is_member(i) ? mix64(mix64(base + i + 0x51ED) % members)
                        : mix64(members + base + i);  // mix64 is a bijection: never inserted
  }
};

int run_probe(arguments const& args)
{
  auto const array_bytes = static_cast<std::size_t>(args.num("array_mib", 300) * mib);
  auto const bpk         = static_cast<std::size_t>(args.num("bpk", 16));
  auto const build_batch = static_cast<std::int64_t>(args.num("batch", 64e6));
  auto const probe_rows  = static_cast<std::int64_t>(args.num("probe", 64e6));
  auto const reps        = static_cast<int>(args.num("reps", 6));
  auto const sorted      = args.num("sorted_probe", 0) != 0;
  auto const label       = args.str("label", "");
  auto const blocks      = array_bytes / 32;
  auto const members     = static_cast<std::uint64_t>(array_bytes) * 8 / bpk;

  environment env{static_cast<std::size_t>(args.num("gpu_gib", 80)) << 30};
  rmm::cuda_set_device_raii guard{rmm::cuda_device_id{0}};
  rmm::cuda_stream_view const task{env.task.value()};
  auto const mr = env.gpu().get_default_allocator();

  auto accumulated = builder::try_create({{0, cudf::data_type{cudf::type_id::INT64}, blocks}}, env.spaces);
  if (!accumulated) { throw std::runtime_error("lease refused"); }
  {
    key_batch batch{rmm::device_buffer{static_cast<std::size_t>(build_batch) * 8, task, env.raw},
                    rmm::device_buffer{},
                    cudf::type_id::INT64,
                    0,
                    0,
                    false};
    for (std::uint64_t done = 0; done < members;) {
      auto const rows =
        static_cast<cudf::size_type>(std::min<std::uint64_t>(static_cast<std::uint64_t>(build_batch), members - done));
      fill_batch<std::int64_t>(batch, done, rows, key_spec{}, task);
      if (!accumulated->enqueue_add(cudf::table_view{{batch.view()}}, rmm::cuda_device_id{0}, env.task_ref())) {
        throw std::runtime_error("no partial");
      }
      task.synchronize();
      done += static_cast<std::uint64_t>(rows);
    }
  }
  auto const published = accumulated->finish(
    rmm::cuda_device_id{0}, ::cuda::stream_ref{env.publication.value()}, mr);
  if (!published || published->filters.size() != 1) { throw std::runtime_error("finish failed"); }
  auto const& filter = *published->filters.front();
  check(cudaDeviceSynchronize(), "sync finish");

  rmm::device_buffer probe_data{static_cast<std::size_t>(probe_rows) * 8, task, env.raw};
  auto* const probe_keys = static_cast<std::int64_t*>(probe_data.data());
  for (std::uint32_t permille : {10U, 100U, 500U}) {
    std::vector<double> ms;
    std::uint64_t passed = 0, hits = 0, rows_total = 0;
    for (int rep = 0; rep < reps + 1; ++rep) {
      probe_spec const spec{members, static_cast<std::uint64_t>(rep) * static_cast<std::uint64_t>(probe_rows) +
                                       std::uint64_t{permille} * 0x100000000ULL,
                            permille};
      thrust::for_each_n(rmm::exec_policy_nosync(task),
                         thrust::counting_iterator<std::int64_t>{0},
                         probe_rows,
                         [=] __device__(std::int64_t i) {
                           probe_keys[i] = static_cast<std::int64_t>(spec(static_cast<std::uint64_t>(i)));
                         });
      auto const member_rows = static_cast<std::uint64_t>(thrust::count_if(
        rmm::exec_policy(task),
        thrust::counting_iterator<std::int64_t>{0},
        thrust::counting_iterator<std::int64_t>{probe_rows},
        [=] __device__(std::int64_t i) { return spec.is_member(static_cast<std::uint64_t>(i)); }));
      if (sorted) { thrust::sort(rmm::exec_policy(task), probe_keys, probe_keys + probe_rows); }
      task.synchronize();
      cudf::column_view const probe{
        cudf::data_type{cudf::type_id::INT64}, static_cast<cudf::size_type>(probe_rows), probe_keys, nullptr, 0};
      auto const t0   = clk::now();
      auto const mask = filter.compute_mask(probe, nullptr, 0, env.task_ref(), mr);
      task.synchronize();
      auto const elapsed = ms_since(t0);
      if (!mask) { throw std::runtime_error("probe declined"); }
      auto const kept = static_cast<std::uint64_t>(thrust::count(
        rmm::exec_policy(task), mask->view().data<bool>(), mask->view().data<bool>() + probe_rows, true));
      if (rep == 0) { continue; }  // warm-up
      ms.push_back(elapsed);
      passed += kept;
      hits += member_rows;
      rows_total += static_cast<std::uint64_t>(probe_rows);
    }
    auto const st     = summarize(ms);
    auto const rows_s = static_cast<double>(probe_rows) / (st.median / 1e3);
    if (passed < hits) { throw std::runtime_error("false negatives"); }
    std::printf(
      "RES,probe,label=%s,array_mib=%.0f,bpk=%zu,members=%llu,probe=%lld,sorted=%d,hit_permille=%u,reps=%d,"
      "ms_med=%.3f,ms_min=%.3f,ms_max=%.3f,Mrows_s=%.1f,keyGB_s=%.2f,nonmembers=%llu,false_pos=%llu,"
      "fpr=%.6f\n",
      label.c_str(),
      static_cast<double>(array_bytes) / mib,
      bpk,
      static_cast<unsigned long long>(members),
      static_cast<long long>(probe_rows),
      sorted ? 1 : 0,
      permille,
      reps,
      st.median,
      st.min,
      st.max,
      rows_s / 1e6,
      rows_s * 8 / 1e9,
      static_cast<unsigned long long>(rows_total - hits),
      static_cast<unsigned long long>(passed - hits),
      static_cast<double>(passed - hits) / static_cast<double>(rows_total - hits));
    std::fflush(stdout);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv)
{
  arguments args;
  for (int i = 1; i < argc; ++i) {
    std::string const a{argv[i]};
    auto const eq = a.find('=');
    if (eq == std::string::npos) {
      std::fprintf(stderr, "bad argument %s\n", a.c_str());
      return 2;
    }
    args.kv[a.substr(0, eq)] = a.substr(eq + 1);
  }
  try {
    auto const suite = args.str("suite", "insert");
    if (suite == "probe") { return run_probe(args); }
    if (suite == "insert") {
      return args.num("key", 64) == 32 ? run_insert<std::int32_t>(args) : run_insert<std::int64_t>(args);
    }
    std::fprintf(stderr, "unknown suite\n");
    return 2;
  } catch (std::exception const& e) {
    std::fprintf(stderr, "FAILED: %s\n", e.what());
    return 1;
  }
}
