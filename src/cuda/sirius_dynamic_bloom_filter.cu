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

#include <cudf/column/column_factories.hpp>
#include <cudf/null_mask.hpp>
#include <cudf/stream_compaction.hpp>
#include <cudf/table/table.hpp>
#include <cudf/table/table_view.hpp>

#include <rmm/cuda_device.hpp>
#include <rmm/device_buffer.hpp>

#include <cuco/bloom_filter.cuh>
#include <cuco/bloom_filter_policies.cuh>
#include <cuco/hash_functions.cuh>
#include <cuda/dynamic_filter_probe_carrier.cuh>
#include <cuda/sirius_rmm_cuco_allocator.cuh>
#include <cuda/std/bit>
#include <cuda/std/cstddef>
#include <cuda/std/functional>
#include <cuda/std/limits>
#include <cuda/stream_ref>
#include <cuda_runtime_api.h>
#include <nvtx3/nvtx3.hpp>

#include <cucascade/memory/common.hpp>
#include <cucascade/memory/memory_space.hpp>
#include <log/logging.hpp>
#include <op/dynamic_filter/dynamic_filter_device.hpp>
#include <op/dynamic_filter/dynamic_filter_replica_reservation.hpp>
#include <op/dynamic_filter/dynamic_filter_replica_transfer.hpp>
#include <op/dynamic_filter/sirius_dynamic_filter.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace sirius::op {

namespace {
constexpr std::size_t kBitsPerBlock                 = 256;
constexpr std::size_t kTargetBitsPerKey             = 16;
constexpr std::size_t kBytesPerBlock                = kBitsPerBlock / 8;
constexpr std::size_t kMaximumReductionScratchBytes = 4U * 1024U * 1024U;

std::size_t blocks_for(std::size_t num_keys)
{
  static_assert(kBitsPerBlock % kTargetBitsPerKey == 0);
  constexpr auto keys_per_block = kBitsPerBlock / kTargetBitsPerKey;
  return num_keys == 0 ? 1 : 1 + (num_keys - 1) / keys_per_block;
}

using bloom_alloc = sirius::rmm_cuco_allocator<cuda::std::byte>;

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

template <class KeyT>
using sirius_bloom = cuco::bloom_filter<KeyT,
                                        cuco::extent<std::size_t>,
                                        cuda::thread_scope_device,
                                        sirius_bloom_policy<KeyT>,
                                        bloom_alloc>;

template <class Filter>
using bloom_owner = std::unique_ptr<Filter>;

using bloom_storage =
  std::variant<bloom_owner<sirius_bloom<std::int32_t>>, bloom_owner<sirius_bloom<std::int64_t>>>;

template <class Filter>
bloom_owner<Filter> make_bloom(std::size_t num_blocks,
                               rmm::device_async_resource_ref mr,
                               cuda::stream_ref stream)
{
  return bloom_owner<Filter>(
    new Filter{cuco::extent<std::size_t>{num_blocks}, {}, {}, bloom_alloc{mr}, stream});
}

template <class Filter>
void copy_filter_storage(Filter const& source,
                         cucascade::memory::memory_space const& source_space,
                         Filter& destination,
                         rmm::cuda_device_id destination_device,
                         rmm::cuda_stream_view stream,
                         cucascade::memory::memory_space const& host_staging_space,
                         std::size_t& bytes)
{
  auto const source_blocks = source.block_extent();
  if (destination.block_extent() != source_blocks) {
    throw std::runtime_error("destination Bloom block extent changed during replication");
  }
  bytes = source_blocks * Filter::words_per_block * sizeof(typename Filter::word_type);
  detail::enqueue_replica_copy(destination.data(),
                               destination_device,
                               source.data(),
                               source_space,
                               bytes,
                               stream,
                               host_staging_space);
}

// Each thread owns one destination word; callers stream-order merges on the root stream.
template <class Word>
__global__ void or_bloom_words(Word* destination, Word const* source, std::size_t count)
{
  auto const index = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (index < count) { destination[index] |= source[index]; }
}

// Grid-stride 16-byte OR of `source_count` scratch slots, `slot_stride` uint4 apart, into one
// destination chunk. Memory-bound; the pipeline overlaps it with the copies of the next chunk.
__global__ void or_bloom_chunk(uint4* destination,
                               uint4 const* scratch,
                               std::size_t slot_stride,
                               int source_count,
                               std::size_t count)
{
  auto const stride = static_cast<std::size_t>(gridDim.x) * blockDim.x;
  for (auto index = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x; index < count;
       index += stride) {
    uint4 value = destination[index];
    for (int source = 0; source < source_count; ++source) {
      uint4 const slot = scratch[static_cast<std::size_t>(source) * slot_stride + index];
      value.x |= slot.x;
      value.y |= slot.y;
      value.z |= slot.z;
      value.w |= slot.w;
    }
    destination[index] = value;
  }
}

constexpr std::size_t kMinimumAutoChunkBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaximumAutoChunkBytes = 8U * 1024U * 1024U;

void cuda_try(cudaError_t status, char const* what)
{
  if (status != cudaSuccess) {
    throw std::runtime_error(std::string("[sirius_dynamic_bloom_publication_pipeline] ") + what +
                             " failed: " + cudaGetErrorString(status));
  }
}

/**
 * @brief Process-lifetime cache of timing-disabled events, one free list per device
 *
 * `cudaEventCreate` can block for milliseconds behind the driver lock while sibling tasks allocate,
 * so publications reuse a handful of events instead of creating them on the critical path. Events
 * are never destroyed: CUDA runtime teardown order makes destruction from a static destructor
 * unsafe, and the cache only ever holds the few events a publication needs per device.
 */
class device_event_cache {
 public:
  static device_event_cache& instance()
  {
    static auto* const cache = new device_event_cache{};
    return *cache;
  }

  /// @pre @p device_id is the current device
  [[nodiscard]] cudaEvent_t acquire(int device_id)
  {
    {
      std::scoped_lock lock(_mutex);
      auto& free_list = _free[device_id];
      if (!free_list.empty()) {
        auto const event = free_list.back();
        free_list.pop_back();
        return event;
      }
    }
    cudaEvent_t event = nullptr;
    cuda_try(cudaEventCreateWithFlags(&event, cudaEventDisableTiming), "cudaEventCreateWithFlags");
    return event;
  }

  void release(int device_id, cudaEvent_t event) noexcept
  {
    if (event == nullptr) { return; }
    try {
      std::scoped_lock lock(_mutex);
      _free[device_id].push_back(event);
    } catch (...) {
      // Out of host memory while returning a cached event: leaking one event is the safe choice.
    }
  }

 private:
  std::mutex _mutex;
  std::unordered_map<int, std::vector<cudaEvent_t>> _free;
};

// Borrowed cached event; returns itself to the cache on destruction.
class scoped_device_event {
 public:
  scoped_device_event() = default;
  /// @pre @p device_id is the current device
  explicit scoped_device_event(int device_id)
    : _device_id{device_id}, _event{device_event_cache::instance().acquire(device_id)}
  {
  }
  ~scoped_device_event() { device_event_cache::instance().release(_device_id, _event); }

  scoped_device_event(scoped_device_event const&)            = delete;
  scoped_device_event& operator=(scoped_device_event const&) = delete;
  scoped_device_event(scoped_device_event&& other) noexcept
    : _device_id{other._device_id}, _event{std::exchange(other._event, nullptr)}
  {
  }
  scoped_device_event& operator=(scoped_device_event&& other) noexcept
  {
    if (this != &other) {
      device_event_cache::instance().release(_device_id, _event);
      _device_id = other._device_id;
      _event     = std::exchange(other._event, nullptr);
    }
    return *this;
  }

  [[nodiscard]] cudaEvent_t get() const noexcept { return _event; }

 private:
  int _device_id     = -1;
  cudaEvent_t _event = nullptr;
};

template <class KeyT>
constexpr cudf::type_id key_type_id() noexcept
{
  static_assert(std::is_same_v<KeyT, std::int32_t> || std::is_same_v<KeyT, std::int64_t>);
  if constexpr (std::is_same_v<KeyT, std::int32_t>) {
    return cudf::type_id::INT32;
  } else {
    return cudf::type_id::INT64;
  }
}
}  // namespace

struct bloom_replica {
  int device_id = -1;
  bloom_storage bloom;

  template <class Filter>
  bloom_replica(int device_id, bloom_owner<Filter> owner)
    : device_id{device_id}, bloom{std::in_place_type<bloom_owner<Filter>>, std::move(owner)}
  {
  }

  ~bloom_replica() noexcept
  {
    if (device_id < 0) { return; }
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{device_id}};
    std::visit([](auto& owner) { owner.reset(); }, bloom);
  }

  [[nodiscard]] bool has_bloom() const noexcept
  {
    return std::visit([](auto const& owner) { return owner != nullptr; }, bloom);
  }
};

namespace {
template <class KeyT>
std::unique_ptr<bloom_replica> make_empty_bloom_replica(int device_id,
                                                        std::size_t num_blocks,
                                                        rmm::device_async_resource_ref mr,
                                                        cuda::stream_ref stream)
{
  return std::make_unique<bloom_replica>(device_id,
                                         make_bloom<sirius_bloom<KeyT>>(num_blocks, mr, stream));
}
}  // namespace

// Owns the source Bloom and every completed device-local replica.
struct sirius_dynamic_bloom_filter::impl {
  int source_device = -1;
  // Retained for null compaction in add().
  rmm::device_async_resource_ref mr;
  std::vector<std::unique_ptr<bloom_replica>> replicas;
  std::unique_ptr<rmm::device_buffer> reduction_scratch;

  explicit impl(rmm::device_async_resource_ref mr) : mr{mr} {}

  ~impl()
  {
    if (source_device < 0) { return; }
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{source_device}};
    reduction_scratch.reset();
  }

  [[nodiscard]] bloom_replica* find(int device_id) noexcept
  {
    auto const it =
      std::find_if(replicas.begin(), replicas.end(), [device_id](auto const& replica) {
        return replica->device_id == device_id;
      });
    return it == replicas.end() ? nullptr : it->get();
  }

  [[nodiscard]] bloom_replica const* find(int device_id) const noexcept
  {
    auto const it =
      std::find_if(replicas.begin(), replicas.end(), [device_id](auto const& replica) {
        return replica->device_id == device_id;
      });
    return it == replicas.end() ? nullptr : it->get();
  }
};

bool sirius_dynamic_bloom_filter::supports(cudf::data_type t) noexcept
{
  return t.id() == cudf::type_id::INT32 || t.id() == cudf::type_id::INT64;
}

std::size_t sirius_dynamic_bloom_filter::estimated_bytes(std::size_t num_keys) noexcept
{
  auto const blocks  = blocks_for(num_keys);
  auto const maximum = std::numeric_limits<std::size_t>::max();
  if (blocks > maximum / kBytesPerBlock) { return maximum; }
  auto const raw_bytes = blocks * kBytesPerBlock;
  if (raw_bytes > maximum - (rmm::CUDA_ALLOCATION_ALIGNMENT - 1)) { return maximum; }
  return detail::tracked_replica_allocation_bytes(raw_bytes);
}

// Sizing on the pre-compaction key count is conservative when nulls are dropped in add().
sirius_dynamic_bloom_filter::sirius_dynamic_bloom_filter(cudf::column_view const& keys,
                                                         rmm::cuda_stream_view stream,
                                                         rmm::device_async_resource_ref mr)
  : sirius_dynamic_bloom_filter(keys.type(), static_cast<std::size_t>(keys.size()), stream, mr)
{
  add(keys, stream);
}

sirius_dynamic_bloom_filter::sirius_dynamic_bloom_filter(cudf::data_type key_type,
                                                         std::size_t expected_num_keys,
                                                         rmm::cuda_stream_view stream,
                                                         rmm::device_async_resource_ref mr)
{
  if (!supports(key_type)) {
    throw std::invalid_argument(
      "[sirius_dynamic_bloom_filter] unsupported key type (INT32 or INT64).");
  }
  cuda::stream_ref const s{stream.value()};
  auto const num_blocks = blocks_for(expected_num_keys);
  auto const maximum    = std::numeric_limits<std::size_t>::max();
  if (num_blocks > maximum / kBytesPerBlock) {
    throw std::length_error("[sirius_dynamic_bloom_filter] requested geometry is too large.");
  }
  auto const raw_bytes = num_blocks * kBytesPerBlock;
  if (raw_bytes > maximum - (rmm::CUDA_ALLOCATION_ALIGNMENT - 1)) {
    throw std::length_error("[sirius_dynamic_bloom_filter] requested geometry is too large.");
  }
  _impl = std::make_unique<impl>(mr);
  if (cudaGetDevice(&_impl->source_device) != cudaSuccess) {
    throw std::runtime_error("[sirius_dynamic_bloom_filter] failed to identify source device.");
  }

  std::unique_ptr<bloom_replica> source;
  switch (key_type.id()) {
    case cudf::type_id::INT32:
      source = make_empty_bloom_replica<std::int32_t>(_impl->source_device, num_blocks, mr, s);
      break;
    case cudf::type_id::INT64:
      source = make_empty_bloom_replica<std::int64_t>(_impl->source_device, num_blocks, mr, s);
      break;
    default:
      throw std::logic_error(
        "[sirius_dynamic_bloom_filter] supported key type changed during construction.");
  }
  _impl->replicas.push_back(std::move(source));
}

void sirius_dynamic_bloom_filter::add(cudf::column_view const& keys, rmm::cuda_stream_view stream)
{
  if (!_impl || !supports(keys.type())) {
    throw std::invalid_argument("[sirius_dynamic_bloom_filter::add] unsupported key type.");
  }
  int device_id = -1;
  if (cudaGetDevice(&device_id) != cudaSuccess || device_id != _impl->source_device) {
    throw std::logic_error("[sirius_dynamic_bloom_filter::add] source device mismatch.");
  }
  auto* source = _impl->find(_impl->source_device);
  if (source == nullptr) {
    throw std::logic_error("[sirius_dynamic_bloom_filter::add] source replica is missing.");
  }
  // A null row's payload is garbage; inserting it would poison the filter. Keep compacted
  // storage alive until add_async is queued on stream.
  std::unique_ptr<cudf::table> compacted;
  cudf::column_view build_keys = keys;
  if (keys.null_count() > 0) {
    compacted  = cudf::drop_nulls(cudf::table_view{{keys}}, {0}, stream, _impl->mr);
    build_keys = compacted->view().column(0);
  }
  cuda::stream_ref const cuda_stream{stream.value()};
  std::visit(
    [&](auto& bloom) {
      using filter_type = typename std::decay_t<decltype(bloom)>::element_type;
      using key_type    = typename filter_type::key_type;
      if (build_keys.type().id() != key_type_id<key_type>()) {
        throw std::invalid_argument("[sirius_dynamic_bloom_filter::add] key type mismatch.");
      }
      auto const* data = build_keys.data<key_type>();
      bloom->add_async(data, data + build_keys.size(), cuda_stream);
    },
    source->bloom);
}

sirius_dynamic_bloom_filter::~sirius_dynamic_bloom_filter() = default;

void sirius_dynamic_bloom_filter::replicate_to_devices(
  std::span<dynamic_filter_replica_space const> spaces)
{
  std::string const nvtx_label =
    "dynfilter::bloom::replicate src=" + std::to_string(_impl ? _impl->source_device : -1) +
    " targets=" + std::to_string(spaces.size());
  nvtx3::scoped_range nvtx_range{nvtx_label};
  if (!_impl || _impl->replicas.empty()) { return; }
  auto const* source = _impl->find(_impl->source_device);
  if (!source) { return; }
  auto const source_target = std::find_if(spaces.begin(), spaces.end(), [this](auto const& target) {
    return target.get_gpu_space().get_device_id() == _impl->source_device;
  });
  if (source_target == spaces.end()) {
    SIRIUS_LOG_WARN(
      "[sirius_dynamic_bloom_filter] source GPU {} has no replica memory space; remote GPUs "
      "will skip this optional filter.",
      _impl->source_device);
    return;
  }
  auto const& source_space = source_target->get_gpu_space();

  // Keep copies and streams alive until all peer transfers are queued and synchronized.
  std::vector<std::pair<std::unique_ptr<bloom_replica>, rmm::cuda_stream_view>> pending;
  pending.reserve(spaces.size());
  _impl->replicas.reserve(_impl->replicas.size() + spaces.size());
  for (auto const& target : spaces) {
    auto const& target_space = target.get_gpu_space();
    auto const device_id     = target_space.get_device_id();
    if (device_id == _impl->source_device || _impl->find(device_id)) { continue; }
    std::string const nvtx_target_label =
      "dynfilter::bloom::replicate_target src=" + std::to_string(_impl->source_device) +
      " dst=" + std::to_string(device_id);
    nvtx3::scoped_range nvtx_target_range{nvtx_target_label};
    std::size_t bytes = 0;
    try {
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{device_id}};
      auto const stream = target_space.acquire_stream();

      auto replica = std::visit(
        [&](auto const& source_bloom) {
          if (!source_bloom) {
            throw std::logic_error(
              "[sirius_dynamic_bloom_filter] source replica has no Bloom filter.");
          }
          using owner_type  = std::decay_t<decltype(source_bloom)>;
          using filter_type = typename owner_type::element_type;

          bytes = source_bloom->block_extent() * filter_type::words_per_block *
                  sizeof(typename filter_type::word_type);
          auto reservation = detail::scoped_replica_reservation::try_acquire(
            target, detail::tracked_replica_allocation_bytes(bytes), stream);
          if (!reservation) { return std::unique_ptr<bloom_replica>{}; }

          auto destination_bloom = make_bloom<filter_type>(source_bloom->block_extent(),
                                                           reservation->allocator(),
                                                           cuda::stream_ref{stream.value()});
          auto result = std::make_unique<bloom_replica>(device_id, std::move(destination_bloom));
          auto& destination = *std::get<bloom_owner<filter_type>>(result->bloom);
          copy_filter_storage(*source_bloom,
                              source_space,
                              destination,
                              rmm::cuda_device_id{device_id},
                              stream,
                              target.get_host_staging_space(),
                              bytes);
          return result;
        },
        source->bloom);
      if (!replica) {
        SIRIUS_LOG_WARN(
          "[sirius_dynamic_bloom_filter] replica GPU {} -> GPU {} skipped: destination "
          "reservation for {} bytes unavailable.",
          _impl->source_device,
          device_id,
          bytes);
        continue;
      }
      pending.emplace_back(std::move(replica), stream);
    } catch (std::exception const& e) {
      SIRIUS_LOG_WARN(
        "[sirius_dynamic_bloom_filter] replica GPU {} -> GPU {} unavailable: {}. "
        "That GPU will skip this optional filter.",
        _impl->source_device,
        device_id,
        e.what());
      continue;
    }
    SIRIUS_LOG_DEBUG("[sirius_dynamic_bloom_filter] queued {}-byte replica GPU {} -> GPU {}.",
                     bytes,
                     _impl->source_device,
                     device_id);
  }

  for (auto& [replica, stream] : pending) {
    auto const device_id = replica->device_id;
    try {
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{device_id}};
      {
        std::string const nvtx_wait_label =
          "dynfilter::bloom::replicate_wait dst=" + std::to_string(device_id);
        nvtx3::scoped_range nvtx_wait_range{nvtx_wait_label};
        stream.synchronize();
      }
      _impl->replicas.push_back(std::move(replica));
    } catch (std::exception const& e) {
      SIRIUS_LOG_WARN(
        "[sirius_dynamic_bloom_filter] replica GPU {} -> GPU {} unavailable: {}. "
        "That GPU will skip this optional filter.",
        _impl->source_device,
        device_id,
        e.what());
    }
  }
}

void sirius_dynamic_bloom_filter::replicate_to_devices_strict(
  std::span<dynamic_filter_replica_space const> spaces)
{
  replicate_to_devices(spaces);
  for (auto const& space : spaces) {
    if (!is_available_on_device(space.get_gpu_space().get_device_id())) {
      throw std::runtime_error(
        "[sirius_dynamic_bloom_filter] required device replica is unavailable.");
    }
  }
}

void sirius_dynamic_bloom_filter::merge_from(sirius_dynamic_bloom_filter const& source_filter,
                                             dynamic_filter_replica_space const& source_space,
                                             dynamic_filter_replica_space const& root_space,
                                             rmm::cuda_stream_view root_stream)
{
  std::string const nvtx_label =
    "dynfilter::bloom::merge_from src=" +
    std::to_string(source_space.get_gpu_space().get_device_id()) +
    " root=" + std::to_string(root_space.get_gpu_space().get_device_id());
  nvtx3::scoped_range nvtx_range{nvtx_label};
  if (!_impl || !source_filter._impl) {
    throw std::logic_error("[sirius_dynamic_bloom_filter::merge_from] missing implementation.");
  }
  auto const root_device = root_space.get_gpu_space().get_device_id();
  if (_impl->source_device != root_device ||
      source_filter._impl->source_device != source_space.get_gpu_space().get_device_id()) {
    throw std::logic_error("[sirius_dynamic_bloom_filter::merge_from] plan/device mismatch.");
  }
  auto* destination  = _impl->find(root_device);
  auto const* source = source_filter._impl->find(source_filter._impl->source_device);
  if (destination == nullptr || source == nullptr ||
      destination->bloom.index() != source->bloom.index()) {
    throw std::logic_error("[sirius_dynamic_bloom_filter::merge_from] geometry mismatch.");
  }

  rmm::cuda_set_device_raii guard{rmm::cuda_device_id{root_device}};
  std::visit(
    [&](auto& destination_bloom) {
      using owner_type         = std::decay_t<decltype(destination_bloom)>;
      using filter_type        = typename owner_type::element_type;
      auto const& source_bloom = std::get<bloom_owner<filter_type>>(source->bloom);
      if (!destination_bloom || !source_bloom ||
          destination_bloom->block_extent() != source_bloom->block_extent()) {
        throw std::logic_error("[sirius_dynamic_bloom_filter::merge_from] geometry mismatch.");
      }
      using word_type       = typename filter_type::word_type;
      auto const word_count = destination_bloom->block_extent() * filter_type::words_per_block;
      auto const maximum_words_per_chunk =
        std::max<std::size_t>(kMaximumReductionScratchBytes / sizeof(word_type), 1);
      auto const scratch_words = std::min(word_count, maximum_words_per_chunk);
      auto const scratch_bytes = scratch_words * sizeof(word_type);
      if (!_impl->reduction_scratch || _impl->reduction_scratch->size() < scratch_bytes) {
        _impl->reduction_scratch = std::make_unique<rmm::device_buffer>(
          scratch_bytes, root_stream, root_space.get_gpu_space().get_default_allocator());
      }

      constexpr int threads = 256;
      for (std::size_t word_offset = 0; word_offset < word_count;
           word_offset += maximum_words_per_chunk) {
        auto const chunk_words = std::min(maximum_words_per_chunk, word_count - word_offset);
        auto const chunk_bytes = chunk_words * sizeof(word_type);
        nvtx3::mark("dynfilter::bloom::merge_chunk");
        detail::enqueue_replica_copy(_impl->reduction_scratch->data(),
                                     rmm::cuda_device_id{root_device},
                                     source_bloom->data() + word_offset,
                                     source_space.get_gpu_space(),
                                     chunk_bytes,
                                     root_stream,
                                     root_space.get_host_staging_space());
        auto const blocks = static_cast<int>((chunk_words + threads - 1) / threads);
        or_bloom_words<<<blocks, threads, 0, root_stream.value()>>>(
          destination_bloom->data() + word_offset,
          static_cast<word_type const*>(_impl->reduction_scratch->data()),
          chunk_words);
        auto const status = cudaPeekAtLastError();
        if (status != cudaSuccess) {
          throw std::runtime_error(
            std::string("[sirius_dynamic_bloom_filter::merge_from] OR launch failed: ") +
            cudaGetErrorString(status));
        }
      }
    },
    destination->bloom);
}

void sirius_dynamic_bloom_filter::release_reduction_scratch()
{
  if (!_impl || _impl->source_device < 0) { return; }
  rmm::cuda_set_device_raii guard{rmm::cuda_device_id{_impl->source_device}};
  _impl->reduction_scratch.reset();
}

bool sirius_dynamic_bloom_filter::is_available_on_device(int device_id) const noexcept
{
  return _impl && _impl->find(detail::resolve_dynamic_filter_device_id(device_id)) != nullptr;
}

std::size_t sirius_dynamic_bloom_filter::replica_count() const noexcept
{
  return _impl ? _impl->replicas.size() : 0;
}

std::unique_ptr<cudf::column> sirius_dynamic_bloom_filter::compute_mask(
  cudf::column_view const& probe,
  int device_id,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr) const
{
  return compute_mask_if(probe, /*stencil=*/nullptr, device_id, stream, mr);
}

std::unique_ptr<cudf::column> sirius_dynamic_bloom_filter::compute_mask_if(
  cudf::column_view const& probe,
  bool const* stencil,
  int device_id,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr) const
{
  nvtx3::scoped_range nvtx_range{"dynfilter::bloom::compute_mask"};
  auto const* replica =
    _impl ? _impl->find(detail::resolve_dynamic_filter_device_id(device_id)) : nullptr;
  if (!replica || !replica->has_bloom()) { return nullptr; }

  auto const compatible_probe = std::visit(
    [&](auto const& bloom) {
      using owner_type = std::decay_t<decltype(bloom)>;
      using key_type   = typename owner_type::element_type::key_type;
      return detail::probe_carrier_compatible(probe.type(),
                                              cudf::data_type{key_type_id<key_type>()});
    },
    replica->bloom);
  if (!compatible_probe) { return nullptr; }

  auto const n = probe.size();
  auto out     = cudf::make_numeric_column(
    cudf::data_type{cudf::type_id::BOOL8}, n, cudf::mask_state::UNALLOCATED, stream, mr);
  cuda::stream_ref const s{stream.value()};
  auto* const outp = out->mutable_view().data<bool>();

  std::visit(
    [&](auto const& bloom) {
      using owner_type = std::decay_t<decltype(bloom)>;
      using key_type   = typename owner_type::element_type::key_type;
      // A narrower carrier is widened per probe through the iterator; the key-typed probe passes
      // its raw pointer through unchanged.
      detail::with_probe_as_key<key_type>(probe, [&](auto first, auto last) {
        if (stencil) {
          bloom->contains_if_async(first, last, stencil, cuda::std::identity{}, outp, s);
        } else {
          bloom->contains_async(first, last, outp, s);
        }
      });
    },
    replica->bloom);

  if (probe.nullable() && probe.null_count() > 0) {
    out->set_null_mask(cudf::copy_bitmask(probe, stream, mr), probe.null_count());
  }
  return out;
}

// Owns every borrowed stream, event and scratch buffer of one pipelined publication. It never
// names the filter's private implementation: the friend methods of the outer class hand it the
// public `bloom_replica` handles and the replica vector to install into.
struct sirius_dynamic_bloom_publication_pipeline::impl {
  struct target_handle {
    dynamic_filter_replica_space const* space = nullptr;
    int device_id                             = -1;
    rmm::cuda_stream_view stream;
  };

  struct pending_key {
    sirius_dynamic_bloom_filter* filter                       = nullptr;
    std::vector<std::unique_ptr<bloom_replica>>* install_into = nullptr;
    std::vector<std::unique_ptr<bloom_replica>> replicas;  // parallel to `targets`
  };

  dynamic_filter_replica_space const& root_space;
  int const root_device;
  std::span<dynamic_filter_replica_space const> spaces;
  std::size_t const requested_chunk_bytes;
  rmm::cuda_stream_view copy_stream;
  rmm::cuda_stream_view or_stream;
  std::vector<target_handle> targets;
  std::array<scoped_device_event, 2> copies_done;
  std::array<scoped_device_event, 2> or_done;
  // Root scratch; every buffer stays alive until completion so a grown buffer never races the
  // OR kernels still reading its predecessor.
  std::vector<std::unique_ptr<rmm::device_buffer>> scratch;
  std::vector<pending_key> pending;
  // Counts chunks across keys: buffer (sequence & 1) is reused only after the OR of sequence - 2.
  std::size_t chunk_sequence = 0;
  // `sources * chunk` of the last key that had sources: the two scratch halves are
  // [0, slot_bytes) and [slot_bytes, 2 * slot_bytes), so the parity WAR wait above only covers
  // ORs issued under the same layout.
  std::size_t slot_bytes_in_use = 0;
  std::size_t ingress_bytes     = 0;
  std::size_t egress_bytes      = 0;
  bool completed                = false;

  impl(dynamic_filter_replica_space const& root_space,
       std::span<dynamic_filter_replica_space const> spaces,
       std::size_t requested_chunk_bytes)
    : root_space{root_space},
      root_device{root_space.get_gpu_space().get_device_id()},
      spaces{spaces},
      requested_chunk_bytes{requested_chunk_bytes}
  {
    auto const is_root = [this](dynamic_filter_replica_space const& space) {
      return space.get_gpu_space().get_device_id() == root_device;
    };
    if (std::none_of(spaces.begin(), spaces.end(), is_root)) {
      throw std::invalid_argument(
        "[sirius_dynamic_bloom_publication_pipeline] the root GPU is absent from the replica plan");
    }
    {
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{root_device}};
      auto const& gpu_space = root_space.get_gpu_space();
      copy_stream           = gpu_space.acquire_stream();
      or_stream             = gpu_space.acquire_stream();
      // Round-robin picks normally differ; a collision only costs the copy/OR overlap.
      for (int attempt = 0; attempt < 16 && or_stream.value() == copy_stream.value(); ++attempt) {
        or_stream = gpu_space.acquire_stream();
      }
      for (auto& event : copies_done) {
        event = scoped_device_event{root_device};
      }
      for (auto& event : or_done) {
        event = scoped_device_event{root_device};
      }
    }
    targets.reserve(spaces.size());
    for (auto const& space : spaces) {
      if (is_root(space)) { continue; }
      auto const device_id = space.get_gpu_space().get_device_id();
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{device_id}};
      targets.push_back({&space, device_id, space.get_gpu_space().acquire_stream()});
    }
  }

  ~impl()
  {
    if (!completed) { drain(); }
    pending.clear();  // replicas select their own device on destruction
    release_scratch();
  }

  impl(impl const&)            = delete;
  impl& operator=(impl const&) = delete;

  /// Best-effort completion of every borrowed stream before anything they read or write is freed.
  void drain() noexcept
  {
    auto const warn = [](char const* role, int device_id, char const* what) noexcept {
      try {
        SIRIUS_LOG_WARN(
          "[sirius_dynamic_bloom_publication_pipeline] {} stream failure drain also failed on GPU "
          "{}: {}",
          role,
          device_id,
          what);
      } catch (...) {  // a throwing log sink must not escape a failure drain
      }
    };
    auto const synchronize = [&warn](
                               rmm::cuda_stream_view stream, int device_id, char const* role) {
      try {
        rmm::cuda_set_device_raii guard{rmm::cuda_device_id{device_id}};
        stream.synchronize();
      } catch (std::exception const& error) {
        warn(role, device_id, error.what());
      } catch (...) {
        warn(role, device_id, "unknown error");
      }
    };
    synchronize(copy_stream, root_device, "copy");
    synchronize(or_stream, root_device, "OR");
    for (auto const& target : targets) {
      synchronize(target.stream, target.device_id, "target");
    }
  }

  void release_scratch() noexcept
  {
    if (scratch.empty()) { return; }
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{root_device}};
    scratch.clear();
  }

  [[nodiscard]] bool has_target(int device_id) const noexcept
  {
    return std::any_of(targets.begin(), targets.end(), [device_id](auto const& target) {
      return target.device_id == device_id;
    });
  }

  /// Returns scratch of at least @p bytes; the copy stream owns its lifetime.
  [[nodiscard]] std::byte* ensure_scratch(std::size_t bytes)
  {
    if (scratch.empty() || scratch.back()->size() < bytes) {
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{root_device}};
      scratch.push_back(std::make_unique<rmm::device_buffer>(
        bytes, copy_stream, root_space.get_gpu_space().get_default_allocator()));
    }
    return static_cast<std::byte*>(scratch.back()->data());
  }

  static void require_peer_dma(detail::replica_transfer_route route, char const* leg)
  {
    if (route == detail::replica_transfer_route::peer_dma ||
        route == detail::replica_transfer_route::none) {
      return;
    }
    throw std::runtime_error(
      std::string("[sirius_dynamic_bloom_publication_pipeline] ") + leg +
      " transfer did not take the direct peer-DMA route; the pipelined scheme cannot order a "
      "host-staged copy");
  }

  template <class Filter>
  void enqueue_typed(sirius_dynamic_bloom_filter& root_filter,
                     std::vector<std::unique_ptr<bloom_replica>>& install_into,
                     bloom_owner<Filter> const& destination_owner,
                     std::span<bloom_replica const* const> source_replicas,
                     std::span<source_partial const> sources)
  {
    using word_type = typename Filter::word_type;
    if (!destination_owner) {
      throw std::logic_error(
        "[sirius_dynamic_bloom_publication_pipeline] root replica has no Bloom filter.");
    }
    auto const block_extent = destination_owner->block_extent();
    auto const filter_bytes = block_extent * Filter::words_per_block * sizeof(word_type);

    std::vector<std::byte const*> source_words;
    source_words.reserve(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
      auto const* owner = std::get_if<bloom_owner<Filter>>(&source_replicas[i]->bloom);
      if (owner == nullptr || !*owner || (*owner)->block_extent() != block_extent) {
        throw std::logic_error(
          "[sirius_dynamic_bloom_publication_pipeline] source geometry mismatch.");
      }
      source_words.push_back(reinterpret_cast<std::byte const*>((*owner)->data()));
    }

    auto const chunk_bytes  = resolve_chunk_bytes(filter_bytes, requested_chunk_bytes);
    auto const source_count = sources.size();
    std::string const nvtx_label =
      "dynfilter::bloom::pipeline_enqueue root=" + std::to_string(root_device) +
      " bytes=" + std::to_string(filter_bytes) + " chunk=" + std::to_string(chunk_bytes) +
      " sources=" + std::to_string(source_count) + " targets=" + std::to_string(targets.size());
    nvtx3::scoped_range nvtx_range{nvtx_label};

    // Replicas first: a reservation denial aborts before this key issues any DMA. The cuco clear
    // runs on the target stream ahead of the copies and overlaps the pipeline fill.
    pending_key key{&root_filter, &install_into, {}};
    key.replicas.reserve(targets.size());
    for (auto const& target : targets) {
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{target.device_id}};
      auto reservation = detail::scoped_replica_reservation::try_acquire(
        *target.space, detail::tracked_replica_allocation_bytes(filter_bytes), target.stream);
      if (!reservation) {
        throw std::runtime_error(
          "[sirius_dynamic_bloom_publication_pipeline] replica GPU " + std::to_string(root_device) +
          " -> GPU " + std::to_string(target.device_id) + ": destination reservation for " +
          std::to_string(filter_bytes) + " bytes unavailable.");
      }
      auto destination_bloom = make_bloom<Filter>(
        block_extent, reservation->allocator(), cuda::stream_ref{target.stream.value()});
      key.replicas.push_back(
        std::make_unique<bloom_replica>(target.device_id, std::move(destination_bloom)));
    }

    std::byte* const scratch_base =
      source_count == 0 ? nullptr : ensure_scratch(2 * source_count * chunk_bytes);
    auto* const destination_bytes = reinterpret_cast<std::byte*>(destination_owner->data());
    auto const slot_stride        = chunk_bytes / sizeof(uint4);
    constexpr int threads         = 256;

    if (source_count > 0) {
      // A key with a different source count or chunk repartitions the scratch halves, so the
      // previous key's last OR (parity sequence - 1) may still be reading bytes this key's first
      // copies overwrite. Order those copies after every OR issued so far: one bubble per relayout,
      // never taken when consecutive keys share the layout (as one publication's keys do).
      auto const slot_bytes = source_count * chunk_bytes;
      if (chunk_sequence > 0 && slot_bytes != slot_bytes_in_use) {
        rmm::cuda_set_device_raii root_guard{rmm::cuda_device_id{root_device}};
        for (auto const& event : or_done) {
          cuda_try(cudaStreamWaitEvent(copy_stream.value(), event.get(), 0),
                   "cudaStreamWaitEvent(copy, or_done relayout)");
        }
      }
      slot_bytes_in_use = slot_bytes;
    }

    for (std::size_t offset = 0; offset < filter_bytes; offset += chunk_bytes, ++chunk_sequence) {
      auto const bytes  = std::min(chunk_bytes, filter_bytes - offset);
      auto const buffer = chunk_sequence & 1U;
      nvtx3::mark("dynfilter::bloom::pipeline_chunk");

      if (source_count > 0) {
        rmm::cuda_set_device_raii root_guard{rmm::cuda_device_id{root_device}};
        std::byte* const slot_base = scratch_base + buffer * source_count * chunk_bytes;
        if (chunk_sequence >= 2) {
          // WAR: the OR of chunk (sequence - 2) read this buffer.
          cuda_try(cudaStreamWaitEvent(copy_stream.value(), or_done[buffer].get(), 0),
                   "cudaStreamWaitEvent(copy, or_done)");
        }
        for (std::size_t source = 0; source < source_count; ++source) {
          auto const route = detail::enqueue_replica_copy(slot_base + source * chunk_bytes,
                                                          rmm::cuda_device_id{root_device},
                                                          source_words[source] + offset,
                                                          sources[source].space->get_gpu_space(),
                                                          bytes,
                                                          copy_stream,
                                                          root_space.get_host_staging_space());
          require_peer_dma(route, "ingress");
        }
        cuda_try(cudaEventRecord(copies_done[buffer].get(), copy_stream.value()),
                 "cudaEventRecord(copies_done)");
        cuda_try(cudaStreamWaitEvent(or_stream.value(), copies_done[buffer].get(), 0),
                 "cudaStreamWaitEvent(or, copies_done)");
        auto const count = bytes / sizeof(uint4);
        auto const blocks =
          static_cast<int>(std::min<std::size_t>((count + threads - 1) / threads, 4096));
        or_bloom_chunk<<<blocks, threads, 0, or_stream.value()>>>(
          reinterpret_cast<uint4*>(destination_bytes + offset),
          reinterpret_cast<uint4 const*>(slot_base),
          slot_stride,
          static_cast<int>(source_count),
          count);
        cuda_try(cudaPeekAtLastError(), "OR launch");
        cuda_try(cudaEventRecord(or_done[buffer].get(), or_stream.value()),
                 "cudaEventRecord(or_done)");
        ingress_bytes += source_count * bytes;
      }

      for (std::size_t t = 0; t < targets.size(); ++t) {
        auto const& target = targets[t];
        if (source_count > 0) {
          cuda_try(cudaStreamWaitEvent(target.stream.value(), or_done[buffer].get(), 0),
                   "cudaStreamWaitEvent(target, or_done)");
        }
        auto& replica_bloom = *std::get<bloom_owner<Filter>>(key.replicas[t]->bloom);
        auto const route =
          detail::enqueue_replica_copy(reinterpret_cast<std::byte*>(replica_bloom.data()) + offset,
                                       rmm::cuda_device_id{target.device_id},
                                       destination_bytes + offset,
                                       root_space.get_gpu_space(),
                                       bytes,
                                       target.stream,
                                       target.space->get_host_staging_space());
        require_peer_dma(route, "egress");
        egress_bytes += bytes;
      }
    }
    pending.push_back(std::move(key));
  }

  void complete()
  {
    if (completed) { return; }
    for (auto const& target : targets) {
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{target.device_id}};
      std::string const nvtx_wait_label =
        "dynfilter::bloom::pipeline_wait dst=" + std::to_string(target.device_id);
      nvtx3::scoped_range nvtx_wait_range{nvtx_wait_label};
      target.stream.synchronize();
    }
    if (targets.empty() && ingress_bytes > 0) {
      // Unreachable by construction (sources are targets), kept so the OR stream can never be
      // left running when the scratch is released below.
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{root_device}};
      or_stream.synchronize();
    }
    // Every target waited on the last OR event, so all scratch readers have finished.
    completed = true;
    release_scratch();
    auto const key_count = pending.size();
    for (auto& key : pending) {
      key.install_into->reserve(key.install_into->size() + key.replicas.size());
      for (auto& replica : key.replicas) {
        key.install_into->push_back(std::move(replica));
      }
      key.replicas.clear();
    }
    for (auto const& key : pending) {
      for (auto const& space : spaces) {
        if (!key.filter->is_available_on_device(space.get_gpu_space().get_device_id())) {
          throw std::runtime_error(
            "[sirius_dynamic_bloom_publication_pipeline] required device replica is unavailable.");
        }
      }
    }
    pending.clear();
    SIRIUS_LOG_DEBUG(
      "[sirius_dynamic_bloom_publication_pipeline] root GPU {} published {} key(s) over {} "
      "chunk(s): {} bytes in, {} bytes out, {} target(s).",
      root_device,
      key_count,
      chunk_sequence,
      ingress_bytes,
      egress_bytes,
      targets.size());
  }
};

bool sirius_dynamic_bloom_publication_pipeline::supports(
  dynamic_filter_replica_space const& root_space,
  std::span<dynamic_filter_replica_space const> spaces)
{
  auto const root_device = root_space.get_gpu_space().get_device_id();
  try {
    for (auto const& space : spaces) {
      auto const device_id = space.get_gpu_space().get_device_id();
      if (device_id == root_device) { continue; }
      if (!cucascade::memory::probe_peer_dma_works(device_id, root_device) ||
          !cucascade::memory::probe_peer_dma_works(root_device, device_id)) {
        return false;
      }
    }
  } catch (...) {
    return false;
  }
  return true;
}

std::size_t sirius_dynamic_bloom_publication_pipeline::resolve_chunk_bytes(
  std::size_t filter_bytes, std::size_t requested_chunk_bytes) noexcept
{
  auto chunk = requested_chunk_bytes == k_auto_chunk_bytes
                 ? std::clamp(filter_bytes / 8, kMinimumAutoChunkBytes, kMaximumAutoChunkBytes)
                 : requested_chunk_bytes;
  // Multiple of the 32-byte Bloom block (and of uint4); never larger than the filter.
  auto const remainder = chunk % kBytesPerBlock;
  if (remainder != 0 && chunk <= std::numeric_limits<std::size_t>::max() - kBytesPerBlock) {
    chunk += kBytesPerBlock - remainder;
  }
  chunk = std::min(chunk, filter_bytes);
  return std::max(chunk, kBytesPerBlock);
}

sirius_dynamic_bloom_publication_pipeline::sirius_dynamic_bloom_publication_pipeline(
  dynamic_filter_replica_space const& root_space,
  std::span<dynamic_filter_replica_space const> spaces,
  std::size_t chunk_bytes)
  : _impl{std::make_unique<impl>(root_space, spaces, chunk_bytes)}
{
}

sirius_dynamic_bloom_publication_pipeline::~sirius_dynamic_bloom_publication_pipeline() = default;

void sirius_dynamic_bloom_publication_pipeline::enqueue(sirius_dynamic_bloom_filter& root_filter,
                                                        std::span<source_partial const> sources)
{
  if (_impl->completed) {
    throw std::logic_error("[sirius_dynamic_bloom_publication_pipeline] enqueue after completion.");
  }
  if (!root_filter._impl) {
    throw std::logic_error(
      "[sirius_dynamic_bloom_publication_pipeline] missing root implementation.");
  }
  auto& root_impl = *root_filter._impl;
  if (root_impl.source_device != _impl->root_device) {
    throw std::logic_error("[sirius_dynamic_bloom_publication_pipeline] plan/device mismatch.");
  }
  if (root_impl.replicas.size() != 1) {
    throw std::logic_error(
      "[sirius_dynamic_bloom_publication_pipeline] the root filter was already replicated.");
  }
  auto const* destination = root_impl.find(_impl->root_device);
  if (destination == nullptr) {
    throw std::logic_error("[sirius_dynamic_bloom_publication_pipeline] root replica is missing.");
  }

  std::vector<bloom_replica const*> source_replicas;
  source_replicas.reserve(sources.size());
  for (auto const& source : sources) {
    if (source.filter == nullptr || source.space == nullptr || !source.filter->_impl) {
      throw std::logic_error("[sirius_dynamic_bloom_publication_pipeline] missing source.");
    }
    auto const device_id = source.space->get_gpu_space().get_device_id();
    if (source.filter->_impl->source_device != device_id || !_impl->has_target(device_id)) {
      throw std::logic_error(
        "[sirius_dynamic_bloom_publication_pipeline] a source is not on a non-root plan GPU.");
    }
    auto const* replica = source.filter->_impl->find(device_id);
    if (replica == nullptr || replica->bloom.index() != destination->bloom.index()) {
      throw std::logic_error(
        "[sirius_dynamic_bloom_publication_pipeline] source geometry mismatch.");
    }
    source_replicas.push_back(replica);
  }

  std::visit(
    [&](auto const& destination_owner) {
      _impl->enqueue_typed(
        root_filter, root_impl.replicas, destination_owner, source_replicas, sources);
    },
    destination->bloom);
}

void sirius_dynamic_bloom_publication_pipeline::complete() { _impl->complete(); }

}  // namespace sirius::op
