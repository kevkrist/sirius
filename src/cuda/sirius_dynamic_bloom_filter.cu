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

#include "memory/host_or.hpp"
#include "memory/peer_islands.hpp"
#include "memory/runtime_stream_pool.hpp"

#include <cudf/table/table_view.hpp>
#include <cudf/utilities/error.hpp>

#include <rmm/cuda_device.hpp>
#include <rmm/cuda_stream.hpp>
#include <rmm/device_buffer.hpp>
#include <rmm/error.hpp>
#include <rmm/mr/per_device_resource.hpp>
#include <rmm/resource_ref.hpp>

#include <cuco/bloom_filter.cuh>
#include <cuco/bloom_filter_policy.cuh>
#include <cuco/bloom_filter_ref.cuh>
#include <cuda/accumulation_partial.cuh>
#include <cuda/dynamic_filter_fused_probe.cuh>
#include <cuda/sirius_rmm_cuco_allocator.cuh>
#include <cuda/std/cstddef>
#include <cuda/stream>
#include <cuda/utility>
#include <thrust/iterator/counting_iterator.h>

#include <cucascade/cuda/event.hpp>
#include <cucascade/memory/common.hpp>
#include <cucascade/memory/fixed_size_host_memory_resource.hpp>
#include <cucascade/memory/memory_space.hpp>
#include <cucascade/memory/reservation_aware_resource_adaptor.hpp>
#include <log/logging.hpp>
#include <op/dynamic_filter/bloom_sizing.hpp>
#include <op/dynamic_filter/detail/accumulated_bloom_builder.hpp>
#include <op/dynamic_filter/detail/union_transfer.hpp>
#include <op/dynamic_filter/dynamic_filter_device.hpp>
#include <op/dynamic_filter/dynamic_filter_replica_reservation.hpp>
#include <op/dynamic_filter/dynamic_filter_replica_transfer.hpp>
#include <op/dynamic_filter/sirius_dynamic_filter.hpp>
#include <sirius/exception.hpp>
#include <telemetry/nvtx.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace sirius::op {

namespace {
using detail::bloom_alloc;
using detail::sirius_bloom;

template <class Filter>
using bloom_owner = std::unique_ptr<Filter>;

// One alternative per membership_key_rep.
using bloom_storage = std::variant<bloom_owner<sirius_bloom<std::int32_t>>,
                                   bloom_owner<sirius_bloom<std::int64_t>>,
                                   bloom_owner<sirius_bloom<std::uint32_t>>,
                                   bloom_owner<sirius_bloom<std::uint64_t>>>;

template <class Filter>
bloom_owner<Filter> make_bloom(std::size_t num_blocks,
                               rmm::device_async_resource_ref mr,
                               ::cuda::stream_ref stream)
{
  return bloom_owner<Filter>(
    new Filter{cuco::extent<std::size_t>{num_blocks}, {}, {}, bloom_alloc{mr}, stream});
}

template <class Filter>
void copy_filter_storage(Filter const& source,
                         cucascade::memory::memory_space const& source_space,
                         Filter& destination,
                         rmm::cuda_device_id destination_device,
                         ::cuda::stream_ref stream,
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

template <class Filter>
bloom_owner<Filter> build_bloom(membership_key_domain const& domain,
                                cudf::column_view const& keys,
                                std::size_t num_blocks,
                                rmm::device_async_resource_ref mr,
                                ::cuda::stream_ref stream)
{
  using key_type = typename Filter::key_type;
  auto result    = make_bloom<Filter>(num_blocks, mr, stream);
  if (keys.size() > 0) {
    // The build column may sit at a same-family carrier other than the rep; the iterator converts
    // per element instead of materializing a rep-typed copy.
    bool const added = detail::with_build_key_iterator<key_type>(
      domain, keys, stream, mr, [&](auto first, auto last) {
        result->add_async(first, last, stream);
      });
    if (!added) {
      throw std::logic_error("[sirius_dynamic_bloom_filter] build carrier does not fit its rep.");
    }
  }
  return result;
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
std::unique_ptr<bloom_replica> build_prepared_bloom_replica(int device_id,
                                                            cudf::column_view const& prepared,
                                                            std::size_t num_blocks,
                                                            rmm::device_async_resource_ref mr,
                                                            ::cuda::stream_ref stream)
{
  auto filter = make_bloom<sirius_bloom<KeyT>>(num_blocks, mr, stream);
  if (prepared.size() > 0) {
    auto const* first = prepared.data<KeyT>();
    filter->add_async(first, first + prepared.size(), stream);
  }
  return std::make_unique<bloom_replica>(device_id, std::move(filter));
}

template <class KeyT>
std::unique_ptr<bloom_replica> build_bloom_replica(int device_id,
                                                   membership_key_domain const& domain,
                                                   cudf::column_view const& keys,
                                                   std::size_t num_blocks,
                                                   rmm::device_async_resource_ref mr,
                                                   ::cuda::stream_ref stream)
{
  return std::make_unique<bloom_replica>(
    device_id, build_bloom<sirius_bloom<KeyT>>(domain, keys, num_blocks, mr, stream));
}
}  // namespace

namespace {

template <class Key>
using raw_bloom_ref = cuco::bloom_filter_ref<Key,
                                             cuco::extent<std::size_t>,
                                             ::cuda::thread_scope_device,
                                             cuco::bloom_filter_policy<Key>>;

using detail::check_cuda;
using detail::cuda_event;
using detail::make_event_on;

/**
 * @brief One published replica of an accumulated key: the array and the partial stream that frees
 * it.
 *
 * The replica and the builder's partial share the stream, and whichever releases it last destroys
 * it, so both destructors make their own GPU current first.
 */
struct accumulated_bloom_replica {
  int device_id;
  std::size_t blocks;
  std::shared_ptr<rmm::cuda_stream>
    stream;  // Declared before bits: outlives the stream-ordered free.
  std::unique_ptr<rmm::device_buffer> bits;

  accumulated_bloom_replica(int device,
                            std::size_t num_blocks,
                            std::shared_ptr<rmm::cuda_stream> owner_stream,
                            std::unique_ptr<rmm::device_buffer> storage) noexcept
    : device_id{device},
      blocks{num_blocks},
      stream{std::move(owner_stream)},
      bits{std::move(storage)}
  {
  }

  ~accumulated_bloom_replica() noexcept
  {
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{device_id}};
    bits.reset();
    stream.reset();
  }

  accumulated_bloom_replica(accumulated_bloom_replica const&)            = delete;
  accumulated_bloom_replica& operator=(accumulated_bloom_replica const&) = delete;
  accumulated_bloom_replica(accumulated_bloom_replica&&)                 = delete;
  accumulated_bloom_replica& operator=(accumulated_bloom_replica&&)      = delete;
};

template <class Key>
[[nodiscard]] raw_bloom_ref<Key> accumulated_ref(void* bits, std::size_t blocks)
{
  using block = typename raw_bloom_ref<Key>::filter_block_type;
  static_assert(sizeof(block) == bloom_bytes_per_block);
  return raw_bloom_ref<Key>{static_cast<block*>(bits), cuco::extent<std::size_t>{blocks}, {}, {}};
}

/**
 * @brief Adds the valid rows of @p keys, a carrier of @p domain, to one accumulated array.
 *
 * Values convert per element to the rep (`with_build_key_iterator`); STRING keys are fingerprinted
 * in-kernel, so nothing is allocated and nothing waits on the host.
 *
 * @throw detail::accumulation_invariant_error if @p keys is not a carrier of @p domain
 */
template <class Key>
void add_accumulated_keys(rmm::device_buffer& bits,
                          std::size_t blocks,
                          membership_key_domain const& domain,
                          cudf::column_view const& keys,
                          ::cuda::stream_ref stream)
{
  if (keys.size() == 0) { return; }
  // Null build keys match nothing under the join's null_equality::UNEQUAL, so they are not added.
  auto const add = [&](auto first) {
    // A pending error belongs to earlier work; it must not be read below as this launch's.
    check_cuda(cudaGetLastError(), "pending CUDA error before Bloom add_if_async");
    accumulated_ref<Key>(bits.data(), blocks)
      .add_if_async(first,
                    first + keys.size(),
                    thrust::counting_iterator<cudf::size_type>{0},
                    detail::probe_validity_of(keys),
                    stream);
    check_cuda(cudaGetLastError(), "Bloom add_if_async", true);
  };
  if (domain.family == membership_key_family::string_hash) {
    if constexpr (std::is_same_v<Key, std::uint64_t>) {
      if (keys.type().id() == cudf::type_id::STRING) {
        detail::with_string_fingerprint_iterator(keys, stream, add);
        return;
      }
    }
    throw detail::accumulation_invariant_error{
      "[accumulated_bloom_builder] a STRING key's contribution is not a STRING column"};
  }
  // The rep conversion needs no memory resource outside the string family.
  bool const added = detail::with_build_key_iterator<Key>(
    domain, keys, stream, rmm::mr::get_current_device_resource_ref(), [&](auto first, auto) {
      add(first);
    });
  if (!added) {
    throw detail::accumulation_invariant_error{
      "[accumulated_bloom_builder] a contribution's key column is not a carrier of its key"};
  }
}

}  // namespace

//===----------------------------------------------------------------------===//
// sirius_dynamic_bloom_filter::impl
//===----------------------------------------------------------------------===//
struct sirius_dynamic_bloom_filter::impl {
  int source_device = -1;
  std::vector<std::unique_ptr<bloom_replica>> replicas;
  std::vector<std::unique_ptr<accumulated_bloom_replica>> accumulated_replicas;

  [[nodiscard]] accumulated_bloom_replica const* find_accumulated(int device_id) const noexcept
  {
    auto const it = std::find_if(
      accumulated_replicas.begin(), accumulated_replicas.end(), [device_id](auto const& replica) {
        return replica->device_id == device_id;
      });
    return it == accumulated_replicas.end() ? nullptr : it->get();
  }

  [[nodiscard]] bloom_replica const* find(int device_id) const noexcept
  {
    auto const it =
      std::find_if(replicas.begin(), replicas.end(), [device_id](auto const& replica) {
        return replica->device_id == device_id;
      });
    return it == replicas.end() ? nullptr : it->get();
  }

  /// The `Store` interface of detail::compute_membership_mask; an accumulated replica wins.
  template <class Fn>
  void with_lookup(membership_key_domain const& domain, int device_id, Fn&& fn) const
  {
    if (auto const* accumulated = find_accumulated(device_id)) {
      detail::dispatch_key_rep(domain.rep, [&]<class Key>(Key) {
        auto ref = accumulated_ref<Key>(accumulated->bits->data(), accumulated->blocks);
        static_assert(std::is_same_v<decltype(ref), detail::bloom_ref<Key>>);
        fn(detail::bloom_lookup<decltype(ref)>{ref});
      });
      return;
    }
    auto const* replica = find(device_id);
    if (!replica || !replica->has_bloom()) { return; }
    std::visit(
      [&](auto const& bloom) {
        auto ref = bloom->ref();
        fn(detail::bloom_lookup<decltype(ref)>{ref});
      },
      replica->bloom);
  }
};

//===----------------------------------------------------------------------===//
// sirius_dynamic_bloom_filter
//===----------------------------------------------------------------------===//
bool sirius_dynamic_bloom_filter::supports(cudf::data_type t) noexcept
{
  return membership_key_supported(t);
}

std::size_t sirius_dynamic_bloom_filter::estimated_bytes(std::size_t num_keys) noexcept
{
  return bloom_blocks_for(num_keys) * bloom_bytes_per_block;
}

sirius_dynamic_bloom_filter::sirius_dynamic_bloom_filter(cudf::column_view const& keys,
                                                         ::cuda::stream_ref stream,
                                                         rmm::device_async_resource_ref mr)
  : sirius_dynamic_bloom_filter(keys, 0, stream, mr)
{
}

sirius_dynamic_bloom_filter::sirius_dynamic_bloom_filter(cudf::column_view const& keys,
                                                         std::size_t capacity_keys,
                                                         ::cuda::stream_ref stream,
                                                         rmm::device_async_resource_ref mr)
{
  // Classifies, checks the DECIMAL128 fit, compacts null build keys out, and names the source
  // device; `build.compacted` stays alive until add_async is queued on `stream`.
  auto const build = prepare_membership_build("[sirius_dynamic_bloom_filter]", keys, stream, mr);
  _domain          = build.domain;
  auto const n     = build.keys.size();
  ::cuda::stream_ref const s{stream.get()};
  auto const num_blocks = bloom_blocks_for(capacity_keys > 0 ? capacity_keys : n);
  _impl                 = std::make_unique<impl>();
  _impl->source_device  = build.source_device;

  auto source = detail::dispatch_key_rep(_domain.rep, [&](auto key_tag) {
    using key_type = decltype(key_tag);
    return build_bloom_replica<key_type>(
      _impl->source_device, _domain, build.keys, num_blocks, mr, s);
  });
  _impl->replicas.push_back(std::move(source));
}

sirius_dynamic_bloom_filter::sirius_dynamic_bloom_filter(membership_key_domain const& domain,
                                                         cudf::column_view const& prepared,
                                                         ::cuda::stream_ref stream,
                                                         rmm::device_async_resource_ref mr)
  : _domain{domain}
{
  detail::require_prepared("[sirius_dynamic_bloom_filter]", domain, prepared);
  // Sized like the column constructor, on the valid keys, so both set the same bits.
  auto const num_blocks = bloom_blocks_for(static_cast<std::size_t>(prepared.size()));
  _impl                 = std::make_unique<impl>();
  _impl->source_device  = detail::current_device("[sirius_dynamic_bloom_filter]");
  auto source           = detail::dispatch_key_rep(_domain.rep, [&](auto key_tag) {
    using key_type = decltype(key_tag);
    return build_prepared_bloom_replica<key_type>(
      _impl->source_device, prepared, num_blocks, mr, ::cuda::stream_ref{stream.get()});
  });
  _impl->replicas.push_back(std::move(source));
}

sirius_dynamic_bloom_filter::~sirius_dynamic_bloom_filter() = default;

sirius_dynamic_bloom_filter::sirius_dynamic_bloom_filter(membership_key_domain domain,
                                                         std::unique_ptr<impl> completed)
  : _domain{domain}, _impl{std::move(completed)}
{
}

std::shared_ptr<sirius_dynamic_bloom_filter> sirius_dynamic_bloom_filter::make_accumulated(
  membership_key_domain domain, std::unique_ptr<impl> completed)
{
  // The constructor is private, so std::make_shared cannot reach it; ownership is taken at once.
  return std::shared_ptr<sirius_dynamic_bloom_filter>{
    new sirius_dynamic_bloom_filter{domain, std::move(completed)}};
}

namespace {

__global__ void or_words(std::uint32_t* into, std::uint32_t const* from, std::size_t count)
{
  auto const i = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (i < count) { into[i] |= from[i]; }
}

}  // namespace

void const* sirius_dynamic_bloom_filter::storage() const
{
  if (auto const* accumulated = _impl->find_accumulated(_impl->source_device)) {
    return accumulated->bits->data();
  }
  auto const* source = _impl->find(_impl->source_device);
  return std::visit([](auto const& bloom) -> void const* { return bloom->data(); }, source->bloom);
}

std::size_t sirius_dynamic_bloom_filter::storage_bytes() const
{
  if (auto const* accumulated = _impl->find_accumulated(_impl->source_device)) {
    return accumulated->blocks * bloom_bytes_per_block;
  }
  auto const* source = _impl->find(_impl->source_device);
  return std::visit(
    [](auto const& bloom) {
      using filter_type = typename std::decay_t<decltype(bloom)>::element_type;
      return bloom->block_extent() * filter_type::words_per_block *
             sizeof(typename filter_type::word_type);
    },
    source->bloom);
}

void sirius_dynamic_bloom_filter::merge_from(void const* words,
                                             std::size_t bytes,
                                             ::cuda::stream_ref stream)
{
  if (bytes != storage_bytes() || bytes % sizeof(std::uint32_t) != 0) {
    throw std::invalid_argument("[sirius_dynamic_bloom_filter] merging a filter of another size");
  }
  // Distributed ranks run on one GPU each, so a merged filter has no other replica to update.
  if (replica_count() > 1) {
    throw std::logic_error("[sirius_dynamic_bloom_filter] merge_from on a filter on several GPUs");
  }
  cudaPointerAttributes attributes{};
  CUDF_CUDA_TRY(cudaPointerGetAttributes(&attributes, words));
  if (attributes.type == cudaMemoryTypeDevice && attributes.device != _impl->source_device) {
    throw std::invalid_argument("[sirius_dynamic_bloom_filter] merging a filter on another GPU");
  }
  rmm::cuda_set_device_raii guard{rmm::cuda_device_id{_impl->source_device}};
  auto* into                 = static_cast<std::uint32_t*>(const_cast<void*>(storage()));
  auto const count           = bytes / sizeof(std::uint32_t);
  constexpr unsigned threads = 256;
  auto const blocks          = static_cast<unsigned>(::cuda::ceil_div(count, std::size_t{threads}));
  or_words<<<blocks, threads, 0, stream.get()>>>(
    into, static_cast<std::uint32_t const*>(words), count);
  CUDF_CUDA_TRY(cudaGetLastError());
}

void sirius_dynamic_bloom_filter::replicate_to_devices(
  std::span<dynamic_filter_replica_space const> spaces)
{
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
  std::vector<std::pair<std::unique_ptr<bloom_replica>, sirius::memory::runtime_stream_pool::lease>>
    pending;
  pending.reserve(spaces.size());
  _impl->replicas.reserve(_impl->replicas.size() + spaces.size());
  detail::replica_fanout_failures failures;
  for (auto const& target : spaces) {
    auto const& target_space = target.get_gpu_space();
    auto const device_id     = target_space.get_device_id();
    if (device_id == _impl->source_device || _impl->find(device_id)) { continue; }
    std::size_t bytes = 0;
    try {
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{device_id}};
      auto stream_lease = sirius::memory::runtime_stream_pool::acquire(target_space);
      auto const stream = stream_lease.get();

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
                                                           ::cuda::stream_ref{stream.get()});
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
      pending.emplace_back(std::move(replica), std::move(stream_lease));
    } catch (...) {
      if (failures.skip_if_recoverable(
            "[sirius_dynamic_bloom_filter]", _impl->source_device, device_id)) {
        continue;
      }
      // Earlier destinations still own asynchronous copies; drain them before propagating.
      break;
    }
    SIRIUS_LOG_DEBUG("[sirius_dynamic_bloom_filter] queued {}-byte replica GPU {} -> GPU {}.",
                     bytes,
                     _impl->source_device,
                     device_id);
  }

  for (auto& [replica, stream_lease] : pending) {
    auto const stream    = stream_lease.get();
    auto const device_id = replica->device_id;
    try {
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{device_id}};
      CUCASCADE_CUDA_TRY(cudaStreamSynchronize(stream.get()));
    } catch (...) {
      failures.record_fatal("[sirius_dynamic_bloom_filter]", _impl->source_device, device_id);
    }
  }
  failures.rethrow_if_fatal();
  for (auto& [replica, stream_lease] : pending) {
    _impl->replicas.push_back(std::move(replica));
  }
}

bool sirius_dynamic_bloom_filter::is_available_on_device(int device_id) const noexcept
{
  auto const resolved = detail::resolve_dynamic_filter_device_id(device_id);
  return _impl &&
         (_impl->find(resolved) != nullptr || _impl->find_accumulated(resolved) != nullptr);
}

std::size_t sirius_dynamic_bloom_filter::replica_count() const noexcept
{
  return _impl ? _impl->replicas.size() + _impl->accumulated_replicas.size() : 0;
}

std::unique_ptr<cudf::column> sirius_dynamic_bloom_filter::compute_mask(
  cudf::column_view const& probe,
  std::uint32_t const* prior_mask_words,
  int device_id,
  ::cuda::stream_ref stream,
  rmm::device_async_resource_ref mr) const
{
  return compute_mask_impl(probe, prior_mask_words, nullptr, device_id, stream, mr);
}

std::unique_ptr<cudf::column> sirius_dynamic_bloom_filter::compute_mask_if(
  cudf::column_view const& probe,
  cudf::column_view const& prior_keep,
  int device_id,
  ::cuda::stream_ref stream,
  rmm::device_async_resource_ref mr) const
{
  return compute_mask_impl(
    probe, nullptr, detail::checked_prior_keep(probe, prior_keep), device_id, stream, mr);
}

std::unique_ptr<cudf::column> sirius_dynamic_bloom_filter::compute_mask_impl(
  cudf::column_view const& probe,
  std::uint32_t const* prior_mask_words,
  bool const* prior_keep,
  int device_id,
  ::cuda::stream_ref stream,
  rmm::device_async_resource_ref mr) const
{
  return detail::compute_membership_mask(_impl.get(),
                                         _domain,
                                         probe,
                                         prior_mask_words,
                                         prior_keep,
                                         device_id,
                                         stream,
                                         mr,
                                         preserves_nulls());
}

bool sirius_dynamic_bloom_filter::prepare_fused_probe(cudf::column_view const& probe,
                                                      int device_id,
                                                      detail::fused_probe& out) const
{
  return detail::prepare_fused_membership_probe(
    _impl.get(), _domain, probe, device_id, out, preserves_nulls());
}

namespace detail {

//===----------------------------------------------------------------------===//
// accumulated_bloom_builder::impl
//===----------------------------------------------------------------------===//
struct accumulated_bloom_builder::impl {
  using partial = accumulation_partial;

  /**
   * @brief One key's array: its Bloom blocks, its bytes, and its transfer chunk.
   */
  struct shape {
    std::size_t blocks;
    std::size_t raw_bytes;
    std::size_t chunk_bytes;

    [[nodiscard]] std::size_t chunks() const noexcept
    {
      return ::cuda::ceil_div(raw_bytes, chunk_bytes);
    }
  };

  impl(std::vector<key> active_keys,
       std::vector<shape> key_shapes,
       std::vector<membership_key_domain> key_domains)
    : keys{std::move(active_keys)}, shapes{std::move(key_shapes)}, domains{std::move(key_domains)}
  {
  }

  /// The largest chunk of any key: the size of one transfer slot.
  [[nodiscard]] std::size_t max_chunk_bytes() const noexcept
  {
    return std::ranges::max(shapes, {}, &shape::chunk_bytes).chunk_bytes;
  }

  [[nodiscard]] partial* find(int device_id) const noexcept
  {
    return find_partial(partials, device_id);
  }

  std::vector<key> keys;
  std::vector<shape> shapes;                   // Per key
  std::vector<membership_key_domain> domains;  // Per key, classified from its type
  std::atomic<bool> leak_storage{false};
  std::vector<std::unique_ptr<partial>> partials;  // Destroyed before leak_storage.
  std::vector<std::shared_ptr<sirius_dynamic_bloom_filter>> filters;
  std::vector<std::uint8_t> islands;  // Per partial: its group of GPUs with mutual pool access
};

//===----------------------------------------------------------------------===//
// accumulated_bloom_builder
//===----------------------------------------------------------------------===//
accumulated_bloom_builder::accumulated_bloom_builder(std::unique_ptr<impl> state) noexcept
  : _impl{std::move(state)}
{
}

accumulated_bloom_builder::~accumulated_bloom_builder() = default;
accumulated_bloom_builder::accumulated_bloom_builder(accumulated_bloom_builder&&) noexcept =
  default;
accumulated_bloom_builder& accumulated_bloom_builder::operator=(
  accumulated_bloom_builder&&) noexcept = default;

namespace {

/**
 * @brief Classifies every key's type into the domain its filter is published with.
 *
 * @throw accumulation_invariant_error for a type no membership filter supports
 */
std::vector<membership_key_domain> classify_keys(
  std::span<accumulated_bloom_builder::key const> keys)
{
  std::vector<membership_key_domain> domains;
  domains.reserve(keys.size());
  for (auto const& active : keys) {
    auto const domain = classify_membership_key(active.type);
    if (!domain) {
      throw accumulation_invariant_error(
        "[accumulated_bloom_builder] unsupported accumulated key type");
    }
    domains.push_back(*domain);
  }
  return domains;
}

}  // namespace

std::optional<accumulated_bloom_builder> accumulated_bloom_builder::try_create(
  std::vector<key> keys,
  accumulated_bloom_geometry geometry,
  std::span<dynamic_filter_replica_space const> targets)
{
  auto constexpr maximum = std::numeric_limits<std::size_t>::max();
  // clang-format off
  if (keys.empty() ||
      geometry.raw_bytes == 0 ||
      geometry.blocks != geometry.raw_bytes / bloom_bytes_per_block ||
      geometry.raw_bytes % bloom_bytes_per_block != 0 ||
      geometry.raw_bytes > maximum - (rmm::CUDA_ALLOCATION_ALIGNMENT - 1) ||
      geometry.chunk_bytes == 0 ||
      geometry.chunk_bytes % rmm::CUDA_ALLOCATION_ALIGNMENT != 0 ||
      geometry.chunk_bytes > tracked_replica_allocation_bytes(geometry.raw_bytes) ||
      geometry.chunk_bytes > accumulated_bloom_geometry::k_max_transfer_chunk_bytes ||
      keys.size() > maximum / tracked_replica_allocation_bytes(geometry.raw_bytes) ||
      geometry.arrays_bytes != keys.size() * tracked_replica_allocation_bytes(geometry.raw_bytes)) {
    throw accumulation_invariant_error{"invalid accumulated Bloom geometry"};
  }
  // clang-format on
  std::vector<impl::shape> shapes(keys.size(),
                                  {geometry.blocks, geometry.raw_bytes, geometry.chunk_bytes});
  for (auto& active : keys) {
    active.blocks = geometry.blocks;
  }
  auto domains = classify_keys(keys);
  return create(std::make_unique<impl>(std::move(keys), std::move(shapes), std::move(domains)),
                targets);
}

std::optional<accumulated_bloom_builder> accumulated_bloom_builder::try_create(
  std::vector<key> keys, std::span<dynamic_filter_replica_space const> targets)
{
  auto constexpr maximum = std::numeric_limits<std::size_t>::max();
  if (keys.empty()) { throw accumulation_invariant_error{"accumulation without keys"}; }
  std::vector<impl::shape> shapes;
  shapes.reserve(keys.size());
  std::size_t lease_bytes = 0;
  for (auto const& active : keys) {
    if (active.blocks == 0 || active.blocks > maximum / bloom_bytes_per_block) {
      throw accumulation_invariant_error{"invalid accumulated Bloom key size"};
    }
    auto const raw_bytes = active.blocks * bloom_bytes_per_block;
    if (raw_bytes > maximum - (rmm::CUDA_ALLOCATION_ALIGNMENT - 1) ||
        tracked_replica_allocation_bytes(raw_bytes) > maximum - lease_bytes) {
      throw accumulation_invariant_error{"invalid accumulated Bloom key size"};
    }
    lease_bytes += tracked_replica_allocation_bytes(raw_bytes);
    shapes.push_back({active.blocks, raw_bytes, accumulated_bloom_chunk_bytes(raw_bytes)});
  }
  auto domains = classify_keys(keys);
  return create(std::make_unique<impl>(std::move(keys), std::move(shapes), std::move(domains)),
                targets);
}

std::optional<accumulated_bloom_builder> accumulated_bloom_builder::create(
  std::unique_ptr<impl> state, std::span<dynamic_filter_replica_space const> targets)
{
  std::vector<std::size_t> array_bytes;
  array_bytes.reserve(state->shapes.size());
  for (auto const& shape : state->shapes) {
    array_bytes.push_back(shape.raw_bytes);
  }
  auto partials = lease_partials(targets, array_bytes, /*zero_fill=*/true, state->leak_storage);
  if (!partials) { return std::nullopt; }
  state->partials = std::move(*partials);
  std::vector<cucascade::memory::memory_space const*> spaces;
  spaces.reserve(state->partials.size());
  for (auto const& entry : state->partials) {
    spaces.push_back(entry->space);
  }
  state->islands = sirius::memory::peer_islands(spaces);
  return accumulated_bloom_builder{std::move(state)};
}

namespace {

/**
 * @brief Throws `accumulation_invariant_error` with @p mismatch unless @p stream belongs to
 *        `device`.
 */
void require_stream_on(::cuda::stream_ref stream, rmm::cuda_device_id device, char const* mismatch)
{
  int stream_device = -1;
  check_cuda(cudaStreamGetDevice(stream.get(), &stream_device), "cudaStreamGetDevice");
  if (stream_device != device.value()) { throw accumulation_invariant_error(mismatch); }
}

}  // namespace

bool accumulated_bloom_builder::enqueue_add(cudf::table_view const& input,
                                            rmm::cuda_device_id device,
                                            ::cuda::stream_ref stream)
{
  nvtx_scoped_range range{"dynfilter::accum::contribute"};
  check_cuda(cudaGetLastError(), "pending CUDA error before accumulated contribution");
  auto* target = _impl->find(device.value());
  if (target == nullptr) { return false; }
  require_stream_on(
    stream,
    device,
    "[accumulated_bloom_builder::enqueue_add] the contribution stream belongs to another GPU");
  rmm::cuda_set_device_raii guard{device};
  cuda_event done;
  std::array const joined{stream};
  try {
    join_on_failure(joined, [&] {
      check_cuda(cudaStreamWaitEvent(stream.get(), target->init->get(), 0),
                 "cudaStreamWaitEvent(init)");
      for (std::size_t index = 0; index < _impl->keys.size(); ++index) {
        auto const& domain = _impl->domains[index];
        detail::dispatch_key_rep(domain.rep, [&](auto key_tag) {
          add_accumulated_keys<decltype(key_tag)>(*target->arrays[index],
                                                  _impl->shapes[index].blocks,
                                                  domain,
                                                  input.column(_impl->keys[index].ordinal),
                                                  stream);
        });
      }
      check_cuda(cudaEventRecord(done.get(), stream.get()), "cudaEventRecord(contribution)");
      // Fold the contribution into the partial stream: every later egress or free follows it.
      check_cuda(cudaStreamWaitEvent(target->stream->value(), done.get(), 0),
                 "cudaStreamWaitEvent(fold contribution)");
    });
  } catch (unjoined_gpu_work const&) {
    _impl->leak_storage.store(true);
    throw;
  }
  target->contributed.store(true, std::memory_order_relaxed);
  return true;
}

std::optional<accumulated_bloom_builder::publication> accumulated_bloom_builder::finish(
  rmm::cuda_device_id root_device,
  ::cuda::stream_ref stream,
  rmm::device_async_resource_ref scratch_mr)
{
  auto const workers = sirius::memory::host_or_workers::shared();
  return finish_impl(root_device, stream, scratch_mr, {}, union_or_runner::of(*workers), nullptr);
}

std::optional<accumulated_bloom_builder::publication> accumulated_bloom_builder::finish_impl(
  rmm::cuda_device_id root_device,
  ::cuda::stream_ref stream,
  rmm::device_async_resource_ref scratch_mr,
  std::span<std::uint8_t const> islands,
  union_or_runner or_runner,
  union_report* report)
{
  nvtx_scoped_range range{"dynfilter::accum::finish"};
  if (report) { *report = {}; }
  check_cuda(cudaGetLastError(), "pending CUDA error before accumulated publication");
  int current_device = -1;
  check_cuda(cudaGetDevice(&current_device), "cudaGetDevice(publication)");
  if (current_device != root_device.value()) {
    throw accumulation_invariant_error(
      "[accumulated_bloom_builder::finish] the root GPU is not the current device");
  }
  require_stream_on(
    stream,
    root_device,
    "[accumulated_bloom_builder::finish] the publication stream belongs to another GPU");
  auto const* const root = _impl->find(root_device.value());
  if (root == nullptr) { return std::nullopt; }
  auto const& partials = _impl->partials;
  if (islands.empty()) {
    islands = _impl->islands;
  } else {
    // Peer copies are legal only inside the real islands, so a caller may only divide them.
    bool divides = islands.size() == partials.size();
    for (std::size_t left = 0; divides && left < islands.size(); ++left) {
      for (std::size_t right = left + 1; right < islands.size(); ++right) {
        divides = divides && (islands[left] != islands[right] ||
                              _impl->islands[left] == _impl->islands[right]);
      }
    }
    if (!divides) {
      throw accumulation_invariant_error(
        "[accumulated_bloom_builder::finish] the islands join GPUs without mutual pool access");
    }
  }
  std::vector<union_shape> shapes;
  std::vector<std::byte*> arrays;
  std::vector<union_member> members;
  shapes.reserve(_impl->shapes.size());
  arrays.reserve(_impl->shapes.size() * partials.size());
  members.reserve(partials.size());
  std::size_t all_bytes = 0;
  for (auto const& shape : _impl->shapes) {
    shapes.push_back({shape.raw_bytes, shape.chunk_bytes});
    all_bytes += shape.raw_bytes;
  }
  std::size_t root_index = 0;
  for (std::size_t index = 0; index < partials.size(); ++index) {
    auto const& entry = *partials[index];
    if (&entry == root) { root_index = index; }
    for (auto const& array : entry.arrays) {
      arrays.push_back(array ? static_cast<std::byte*>(array->data()) : nullptr);
    }
    members.push_back({entry.space,
                       entry.stream_ref(),
                       std::span{arrays}.last(entry.arrays.size()),
                       entry.contributed.load(std::memory_order_relaxed)});
  }
  std::optional<union_report> united;
  auto const started = std::chrono::steady_clock::now();
  try {
    united = unite({members,
                    islands,
                    shapes,
                    root_index,
                    stream,
                    scratch_mr,
                    root->host_space->get_memory_resource_of<cucascade::memory::Tier::HOST>()},
                   or_runner);
  } catch (unjoined_gpu_work const&) {
    // As in `enqueue_add`: never free storage that unfinished GPU work may still touch.
    _impl->leak_storage.store(true);
    throw;
  }
  if (!united) { return std::nullopt; }
  auto const route =
    united->host_inputs == 0
      ? (united->peer_copies ? publication_route::PEER : publication_route::NONE)
      : (united->peer_copies ? publication_route::PEER_AND_HOST : publication_route::HOST);
  SIRIUS_LOG_DEBUG(
    "[accumulated_bloom_builder::finish] root GPU {} united {} key(s), {} bytes per GPU, on {} "
    "GPU(s) in {:.3f} ms: {} island(s), {} through the host ({:.3f} ms in host ORs, {:.3f} ms "
    "waiting for their inputs), {} chunk(s) in flight, {} pinned bytes, {} device scratch bytes{}.",
    root->device_id,
    _impl->keys.size(),
    all_bytes,
    partials.size(),
    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
    united->islands,
    united->host_inputs,
    united->host_or_ms,
    united->host_wait_ms,
    united->window_chunks,
    united->host_window_bytes,
    std::reduce(united->device_scratch_bytes.begin(), united->device_scratch_bytes.end()),
    united->degraded ? ", degraded" : "");
  auto const degraded = united->degraded;
  if (report) { *report = std::move(*united); }
  make_filters(root->device_id);
  return publication{{_impl->filters}, route, degraded};
}

void accumulated_bloom_builder::make_filters(int root_device)
{
  std::vector<std::shared_ptr<sirius_dynamic_bloom_filter>> filters;
  filters.reserve(_impl->keys.size());
  for (std::size_t key_index = 0; key_index < _impl->keys.size(); ++key_index) {
    auto filter           = std::make_unique<sirius_dynamic_bloom_filter::impl>();
    filter->source_device = root_device;
    filter->accumulated_replicas.reserve(_impl->partials.size());
    for (auto const& entry : _impl->partials) {
      filter->accumulated_replicas.push_back(std::make_unique<accumulated_bloom_replica>(
        entry->device_id, _impl->shapes[key_index].blocks, entry->stream, nullptr));
    }
    filters.push_back(
      sirius_dynamic_bloom_filter::make_accumulated(_impl->domains[key_index], std::move(filter)));
  }
  // Allocate every host shell before transferring leased arrays. From this point, only nonthrowing
  // ownership moves run, and the builder keeps an owner even when no channel accepts a key.
  _impl->filters = std::move(filters);
  for (std::size_t key_index = 0; key_index < _impl->keys.size(); ++key_index) {
    auto& replicas = _impl->filters[key_index]->_impl->accumulated_replicas;
    for (std::size_t gpu_index = 0; gpu_index < _impl->partials.size(); ++gpu_index) {
      replicas[gpu_index]->bits = std::move(_impl->partials[gpu_index]->arrays[key_index]);
    }
  }
}

std::optional<accumulated_bloom_builder::replica_contents>
accumulated_bloom_builder::inspect_partial(std::size_t key_index, rmm::cuda_device_id device) const
{
  auto const* partial = _impl ? _impl->find(device.value()) : nullptr;
  if (partial == nullptr || key_index >= partial->arrays.size() || !partial->arrays[key_index]) {
    return std::nullopt;
  }
  rmm::cuda_set_device_raii guard{device};
  auto const pending = cudaStreamQuery(partial->stream->value());
  if (pending != cudaErrorNotReady) { check_cuda(pending, "cudaStreamQuery(partial)"); }
  auto const& bits = *partial->arrays[key_index];
  replica_contents contents{std::vector<std::byte>(bits.size()), pending == cudaSuccess};
  check_cuda(cudaMemcpyAsync(contents.bytes.data(),
                             bits.data(),
                             bits.size(),
                             cudaMemcpyDeviceToHost,
                             partial->stream->value()),
             "cudaMemcpyAsync(partial snapshot)");
  check_cuda(cudaStreamSynchronize(partial->stream->value()),
             "cudaStreamSynchronize(partial snapshot)");
  return contents;
}

std::optional<accumulated_bloom_builder::replica_contents>
accumulated_bloom_builder::inspect_replica(sirius_dynamic_bloom_filter const& filter,
                                           rmm::cuda_device_id device)
{
  auto const* replica = filter._impl ? filter._impl->find_accumulated(device.value()) : nullptr;
  if (replica == nullptr || !replica->bits) { return std::nullopt; }
  rmm::cuda_set_device_raii guard{device};
  auto const pending = cudaStreamQuery(replica->stream->value());
  if (pending != cudaErrorNotReady) { check_cuda(pending, "cudaStreamQuery(replica)"); }
  replica_contents contents{std::vector<std::byte>(replica->bits->size()), pending == cudaSuccess};
  check_cuda(cudaMemcpyAsync(contents.bytes.data(),
                             replica->bits->data(),
                             contents.bytes.size(),
                             cudaMemcpyDeviceToHost,
                             replica->stream->value()),
             "cudaMemcpyAsync(replica)");
  check_cuda(cudaStreamSynchronize(replica->stream->value()), "cudaStreamSynchronize(replica)");
  return contents;
}

std::optional<accumulated_bloom_builder::replica_contents>
accumulated_bloom_builder::inspect_replica(sirius_dynamic_bloom_filter const& filter,
                                           rmm::cuda_device_id device,
                                           ::cuda::stream_ref stream)
{
  if (filter._impl && filter._impl->find_accumulated(device.value()) != nullptr) {
    return inspect_replica(filter, device);
  }
  auto const* built = filter._impl ? filter._impl->find(device.value()) : nullptr;
  if (built == nullptr || !built->has_bloom()) { return std::nullopt; }
  rmm::cuda_set_device_raii guard{device};
  auto const pending = cudaStreamQuery(stream.get());
  if (pending != cudaErrorNotReady) { check_cuda(pending, "cudaStreamQuery(built Bloom)"); }
  return std::visit(
    [&](auto const& owner) -> std::optional<replica_contents> {
      using filter_type = typename std::decay_t<decltype(owner)>::element_type;
      replica_contents contents{
        std::vector<std::byte>(owner->block_extent() * filter_type::words_per_block *
                               sizeof(typename filter_type::word_type)),
        pending == cudaSuccess};
      check_cuda(cudaMemcpyAsync(contents.bytes.data(),
                                 owner->data(),
                                 contents.bytes.size(),
                                 cudaMemcpyDeviceToHost,
                                 stream.get()),
                 "cudaMemcpyAsync(built Bloom)");
      check_cuda(cudaStreamSynchronize(stream.get()), "cudaStreamSynchronize(built Bloom)");
      return contents;
    },
    built->bloom);
}

bool accumulated_bloom_builder::releasable_here() const noexcept
{
  if (!_impl) { return true; }
  try {
    return std::ranges::none_of(_impl->partials, [](auto const& entry) {
      auto const* allocator =
        entry->space->template get_memory_resource_of<cucascade::memory::Tier::GPU>();
      return allocator != nullptr && allocator->is_stream_tracked(entry->stream_ref());
    });
  } catch (...) {
    return false;
  }
}

}  // namespace detail

}  // namespace sirius::op
