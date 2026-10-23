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

#include "memory/pool_peer_access.hpp"
#include "memory/sirius_memory_reservation_manager.hpp"
#include "op/dynamic_filter/bloom_sizing.hpp"
#include "op/dynamic_filter/detail/accumulated_bloom_builder.hpp"
#include "op/dynamic_filter/sirius_dynamic_filter.hpp"
#include "s4_experiment_common.hpp"

#include <cudf/column/column_factories.hpp>
#include <cudf/column/column_view.hpp>
#include <cudf/table/table.hpp>

#include <rmm/aligned.hpp>
#include <rmm/cuda_device.hpp>
#include <rmm/cuda_stream.hpp>
#include <rmm/device_buffer.hpp>

#include <cuda_runtime_api.h>

#include <cucascade/memory/fixed_size_host_memory_resource.hpp>
#include <cucascade/memory/reservation_manager_configurator.hpp>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace sirius::test::s4 {
namespace {

using builder  = op::detail::accumulated_bloom_builder;
using geometry = op::detail::accumulated_bloom_geometry;
using space    = cucascade::memory::memory_space;
using tier     = cucascade::memory::Tier;

void check(cudaError_t status, char const* operation)
{
  if (status != cudaSuccess) {
    throw std::runtime_error(std::string{operation} + ": " + cudaGetErrorString(status));
  }
}

void require(bool condition, char const* message)
{
  if (!condition) { throw std::runtime_error(message); }
}

std::uint64_t mix(std::uint64_t value)
{
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

/**
 * @brief Retires stream work before storage can unwind
 *
 * A failed cleanup barrier terminates this diagnostic process before any borrowed GPU memory can be
 * freed. Explicit joins report ordinary CUDA failures to the sample's error handler.
 */
class owned_stream {
 public:
  explicit owned_stream(int device) : device_{device}
  {
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{device_}};
    stream_ = std::make_unique<rmm::cuda_stream>(rmm::cuda_stream::flags::non_blocking);
  }
  ~owned_stream()
  {
    join_noexcept();
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{device_}};
    stream_.reset();
  }
  owned_stream(owned_stream const&)            = delete;
  owned_stream& operator=(owned_stream const&) = delete;
  int device() const { return device_; }
  cudaStream_t get() const { return stream_->value(); }
  ::cuda::stream_ref ref() const { return ::cuda::stream_ref{get()}; }
  void join() const
  {
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{device_}};
    check(cudaStreamSynchronize(get()), "stream completion");
  }
  void join_noexcept() const noexcept
  {
    int previous{};
    if (cudaGetDevice(&previous) != cudaSuccess || cudaSetDevice(device_) != cudaSuccess ||
        cudaStreamSynchronize(get()) != cudaSuccess || cudaSetDevice(previous) != cudaSuccess) {
      std::fputs("sirius_s4_experiment: cleanup could not retire GPU work; terminating\n", stderr);
      std::abort();
    }
  }

 private:
  int device_;
  std::unique_ptr<rmm::cuda_stream> stream_;
};

struct retire_before_release {
  owned_stream const& stream;
  ~retire_before_release() { stream.join_noexcept(); }
};

[[noreturn]] void unjoined_failure(record& sample, std::ostream& output)
{
  // The process owns the pools too; unwinding them would invalidate deliberately retained arrays.
  try {
    sample.set("status", "error");
    sample.set("skip_reason", "unjoined_gpu_work");
    sample.write(output);
  } catch (...) {
  }
  std::_Exit(1);
}

struct device_storage {
  owned_stream stream;
  std::unique_ptr<rmm::device_buffer> bytes;

  device_storage(space& owner, std::size_t size) : stream{owner.get_device_id()}
  {
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{stream.device()}};
    bytes = std::make_unique<rmm::device_buffer>(size, stream.ref(), owner.get_default_allocator());
    stream.join();
  }
  ~device_storage()
  {
    stream.join_noexcept();
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{stream.device()}};
    bytes.reset();
    stream.join_noexcept();
  }
};

std::unique_ptr<memory::sirius_memory_reservation_manager> make_manager(options const& r)
{
  auto const arrays  = r.mode == "builder" ? r.keys : 2 * (r.devices.size() - 1);
  auto const payload = checked_product(r.bytes, arrays);
  require(payload <= std::numeric_limits<std::size_t>::max() - (128ULL << 20), "capacity overflow");
  cucascade::memory::reservation_manager_configurator config;
  config.set_gpu_ids(r.devices)
    .set_gpu_usage_limit(std::max<std::size_t>(512ULL << 20, payload + (128ULL << 20)))
    .set_reservation_fraction_per_gpu(0.9)
    .set_per_numa_region_capacity(128ULL << 20)
    .set_reservation_fraction_per_numa_region(0.9)
    .use_gpu_id_as_host_id()
    .set_host_pool_features(1ULL << 20, 32, 1)
    .track_reservation_per_stream(false);
  return std::make_unique<memory::sirius_memory_reservation_manager>(config.build());
}

bool devices_available(options const& request, std::ostream& output)
{
  int count{};
  auto const status = cudaGetDeviceCount(&count);
  if (status != cudaErrorNoDevice) { check(status, "device enumeration"); }
  if (std::ranges::all_of(request.devices, [count](int device) { return device < count; })) {
    return true;
  }
  for (std::uint64_t i = 0; i < request.warmups + request.repetitions; ++i) {
    auto sample = sample_record(request, i);
    sample.set("status", "unavailable");
    sample.set("skip_reason", "requested_cuda_device_not_visible");
    sample.write(output);
  }
  return false;
}

space& gpu(memory::sirius_memory_reservation_manager& manager, int device)
{
  auto* result = manager.get_memory_space(tier::GPU, device);
  require(result != nullptr, "configured GPU space missing");
  return *result;
}

std::vector<std::byte> transport_input(std::size_t bytes, std::uint64_t seed, int source)
{
  std::vector<std::byte> result(bytes);
  for (std::size_t i = 0; i < bytes; ++i) {
    result[i] =
      static_cast<std::byte>(mix(seed ^ (static_cast<std::uint64_t>(source) << 32) ^ i) & 255);
  }
  return result;
}

void upload(device_storage& target, std::span<std::byte const> host)
{
  rmm::cuda_set_device_raii guard{rmm::cuda_device_id{target.stream.device()}};
  retire_before_release const retire{target.stream};
  check(
    cudaMemcpyAsync(
      target.bytes->data(), host.data(), host.size(), cudaMemcpyHostToDevice, target.stream.get()),
    "fixture upload");
  target.stream.join();
}

bool equal_bytes(device_storage& actual, std::span<std::byte const> expected)
{
  std::vector<std::byte> host(expected.size());
  rmm::cuda_set_device_raii guard{rmm::cuda_device_id{actual.stream.device()}};
  retire_before_release const retire{actual.stream};
  check(
    cudaMemcpyAsync(
      host.data(), actual.bytes->data(), host.size(), cudaMemcpyDeviceToHost, actual.stream.get()),
    "fixture verification download");
  actual.stream.join();
  return std::ranges::equal(host, expected);
}

template <typename T>
std::unique_ptr<cudf::column> key_column(space& owner,
                                         owned_stream const& stream,
                                         std::uint64_t seed,
                                         int contributor,
                                         std::uint64_t key,
                                         std::size_t rows)
{
  std::vector<T> values(rows);
  for (std::size_t i = 0; i < rows; ++i) {
    auto const bits = mix(seed ^ (static_cast<std::uint64_t>(contributor) << 32) ^ (key << 56) ^ i);
    if constexpr (sizeof(T) == 4) {
      values[i] = std::bit_cast<T>(static_cast<std::uint32_t>(bits));
    } else {
      values[i] = std::bit_cast<T>(bits);
    }
  }
  auto const type = cudf::data_type{sizeof(T) == 4 ? cudf::type_id::INT32 : cudf::type_id::INT64};
  auto column     = cudf::make_numeric_column(type,
                                          static_cast<cudf::size_type>(rows),
                                          cudf::mask_state::UNALLOCATED,
                                          stream.ref(),
                                          owner.get_default_allocator());
  retire_before_release const retire{stream};
  if (rows) {
    check(cudaMemcpyAsync(column->mutable_view().data<T>(),
                          values.data(),
                          values.size() * sizeof(T),
                          cudaMemcpyHostToDevice,
                          stream.get()),
          "key upload");
    stream.join();
  }
  return column;
}

std::unique_ptr<cudf::table> input_table(options const& r,
                                         space& owner,
                                         owned_stream const& stream,
                                         int contributor)
{
  rmm::cuda_set_device_raii guard{rmm::cuda_device_id{owner.get_device_id()}};
  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(key_column<std::int32_t>(owner, stream, r.seed, contributor, 0, r.rows));
  if (r.keys == 2) {
    columns.push_back(key_column<std::int64_t>(owner, stream, r.seed, contributor, 1, r.rows));
  }
  return std::make_unique<cudf::table>(std::move(columns));
}

void measure(record& sample, op::detail::publication_measurement const& m)
{
  sample.set("actual_transport", m.actual_transport);
  if (m.submitted_logical_edge_bytes != 0) {
    bool const host = std::string_view{m.actual_transport} == "host";
    sample.set("transport_api", host ? "cudf::detail::memcpy_batch_async" : "cudaMemcpyPeerAsync");
    sample.set("stream_owner", host ? "source_for_d2h_destination_for_h2d" : "destination");
  }
  sample.set("contributing_gpu_count", m.contributing_gpu_count);
  sample.set("input_bitmap_bytes", m.input_bitmap_bytes);
  sample.set("ready_replica_bytes", m.ready_replica_bytes);
  sample.set("published_bitmap_bytes", 0);
  sample.set("logical_edge_bytes", m.submitted_logical_edge_bytes);
  sample.set("d2h_bytes", m.submitted_d2h_bytes);
  sample.set("h2d_bytes", m.submitted_h2d_bytes);
  sample.set("planned_logical_edge_bytes", m.planned_logical_edge_bytes);
  sample.set("planned_d2h_bytes", m.planned_d2h_bytes);
  sample.set("planned_h2d_bytes", m.planned_h2d_bytes);
  sample.set("scratch_requested_bytes", m.requested_device_scratch_bytes);
  sample.set("scratch_aligned_bytes", m.aligned_device_scratch_bytes);
  sample.set("host_staging_physical_bytes", m.physical_host_bytes);
  sample.set("host_elapsed_ns", m.host_elapsed_ns);
  sample.set("submitted_bytes_complete", m.submitted_bytes_complete);
  sample.set("production_status", m.status);
  sample.set("production_refusal_reason", m.refusal_reason);
}

void verify_measurement(options const& r,
                        geometry const& shape,
                        space const& host_space,
                        op::detail::publication_measurement const& m)
{
  auto const key_bytes = checked_product(r.keys, r.bytes);
  auto const sources   = r.contributors.size() - 1;
  auto const targets   = r.devices.size() - 1;
  auto const logical   = checked_product(sources + targets, key_bytes);
  require(m.input_bitmap_bytes == checked_product(r.contributors.size(), key_bytes) &&
            m.ready_replica_bytes == checked_product(r.devices.size(), key_bytes),
          "builder bitmap accounting mismatch");
  require(m.submitted_bytes_complete && m.submitted_logical_edge_bytes == logical &&
            m.planned_logical_edge_bytes == logical,
          "builder logical edge accounting mismatch");
  bool const host = std::string_view{m.actual_transport} == "host";
  require(host || std::string_view{m.actual_transport} == "direct", "unknown completed transport");
  auto const scratch =
    host ? (sources ? shape.chunk_bytes : 0)
         : checked_product(std::min<std::size_t>(2, r.keys * shape.chunks_per_key()) * sources,
                           shape.chunk_bytes);
  require(
    m.requested_device_scratch_bytes == scratch &&
      m.aligned_device_scratch_bytes == rmm::align_up(scratch, rmm::CUDA_ALLOCATION_ALIGNMENT),
    "builder scratch is not bounded by production chunk geometry");
  auto const d2h = host && targets ? checked_product(sources + 1, key_bytes) : 0;
  auto const h2d = host ? logical : 0;
  require(m.submitted_d2h_bytes == d2h && m.submitted_h2d_bytes == h2d &&
            m.planned_d2h_bytes == d2h && m.planned_h2d_bytes == h2d,
          "builder HOST copy accounting mismatch");
  auto const* resource = host_space.get_memory_resource_of<tier::HOST>();
  require(resource != nullptr && resource->get_block_size() != 0, "HOST block geometry missing");
  auto const block = resource->get_block_size();
  auto const staging =
    host && targets ? checked_product((shape.chunk_bytes + block - 1) / block, block) : 0;
  require(m.physical_host_bytes == staging,
          "builder HOST charge differs from physical block rounding");
}

}  // namespace

void run_transport(options const& r, std::ostream& output)
{
  require(r.issuer == "destination_read" || r.issuer == "source_write",
          "unknown issuing direction");
  require(r.pattern == "pair" || r.pattern == "all_pairs" || r.pattern == "root_inbound" ||
            r.pattern == "root_outbound",
          "unknown transport pattern");
  std::vector<std::pair<int, int>> edges;
  for (int source : r.devices) {
    for (int target : r.devices) {
      if (source == target) { continue; }
      if (r.pattern == "all_pairs" ||
          (r.pattern == "pair" && source == r.source && target == r.destination) ||
          (r.pattern == "root_inbound" && target == r.root) ||
          (r.pattern == "root_outbound" && source == r.root)) {
        edges.emplace_back(source, target);
      }
    }
  }
  require(!edges.empty(), "transport needs distinct selected endpoints");
  if (!devices_available(r, output)) { return; }
  auto manager = make_manager(r);
  bool granted = true;
  for (auto [source, target] : edges) {
    auto const owner    = r.issuer == "destination_read" ? source : target;
    auto const accessor = r.issuer == "destination_read" ? target : source;
    granted             = memory::try_grant_pool_access(gpu(*manager, owner), accessor) && granted;
  }
  if (!granted) {
    for (std::uint64_t i = 0; i < r.warmups + r.repetitions; ++i) {
      auto sample = sample_record(r, i);
      sample.set("pattern", r.pattern);
      sample.set("issuer", r.issuer);
      sample.set("B", r.bytes);
      skipped(sample, "owning_pool_access_unavailable");
      sample.write(output);
    }
    return;
  }

  std::vector<std::unique_ptr<device_storage>> sources, destinations;
  std::vector<std::unique_ptr<owned_stream>> transfers;
  std::vector<int> source_ids, destination_ids, stream_devices;
  for (auto [source, target] : edges) {
    sources.push_back(std::make_unique<device_storage>(gpu(*manager, source), r.bytes));
    destinations.push_back(std::make_unique<device_storage>(gpu(*manager, target), r.bytes));
    auto const issuer = r.issuer == "destination_read" ? target : source;
    transfers.push_back(std::make_unique<owned_stream>(issuer));
    source_ids.push_back(source);
    destination_ids.push_back(target);
    stream_devices.push_back(issuer);
    upload(*sources.back(), transport_input(r.bytes, r.seed, source));
  }
  // Transfer streams unwind before either buffer collection, including a partially submitted batch.
  for (std::uint64_t i = 0; i < r.warmups + r.repetitions; ++i) {
    auto sample = sample_record(r, i);
    sample.set("pattern", r.pattern);
    sample.set("issuer", r.issuer);
    sample.set("source", r.source);
    sample.set("destination", r.destination);
    sample.set("B", r.bytes);
    sample.set("actual_transport", "direct");
    sample.set("timing_scope", "copy_submission_through_completion");
    sample.set("transport_api", "cudaMemcpyPeerAsync");
    sample.set("stream_owner", r.issuer == "destination_read" ? "destination" : "source");
    sample.array("edge_sources", std::span<int const>{source_ids});
    sample.array("edge_destinations", std::span<int const>{destination_ids});
    sample.array("stream_devices", std::span<int const>{stream_devices});
    sample.set("planned_logical_edge_bytes", checked_product(edges.size(), r.bytes));
    sample.set("planned_d2h_bytes", 0);
    sample.set("planned_h2d_bytes", 0);
    sample.set("d2h_bytes", 0);
    sample.set("h2d_bytes", 0);
    sample.set("host_staging_physical_bytes", 0);
    sample.set("submitted_bytes_complete", true);
    std::uint64_t submitted{};
    for (auto const& destination : destinations) {
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{destination->stream.device()}};
      check(cudaMemsetAsync(destination->bytes->data(), 0, r.bytes, destination->stream.get()),
            "destination reset");
      destination->stream.join();
    }
    auto const start = clock::now();
    try {
      for (std::size_t edge = 0; edge < edges.size(); ++edge) {
        auto const [source, target] = edges[edge];
        rmm::cuda_set_device_raii guard{rmm::cuda_device_id{transfers[edge]->device()}};
        check(cudaMemcpyPeerAsync(destinations[edge]->bytes->data(),
                                  target,
                                  sources[edge]->bytes->data(),
                                  source,
                                  r.bytes,
                                  transfers[edge]->get()),
              "pooled peer copy");
        submitted += r.bytes;
      }
      for (auto const& transfer : transfers) {
        transfer->join();
      }
      sample.set("host_elapsed_ns", elapsed_ns(start));
      sample.set("logical_edge_bytes", submitted);
      std::vector<std::uint64_t> transmitted(r.devices.size()), received(r.devices.size());
      for (auto [source, target] : edges) {
        transmitted[std::ranges::find(r.devices, source) - r.devices.begin()] += r.bytes;
        received[std::ranges::find(r.devices, target) - r.devices.begin()] += r.bytes;
      }
      sample.array("per_gpu_tx_bytes", std::span<std::uint64_t const>{transmitted});
      sample.array("per_gpu_rx_bytes", std::span<std::uint64_t const>{received});
      for (std::size_t edge = 0; edge < edges.size(); ++edge) {
        auto const expected = transport_input(r.bytes, r.seed, edges[edge].first);
        require(equal_bytes(*destinations[edge], expected), "transport destination byte mismatch");
        require(equal_bytes(*sources[edge], expected), "transport modified an immutable source");
      }
      passed(sample);
    } catch (std::exception const& error) {
      for (auto const& transfer : transfers) {
        transfer->join_noexcept();
      }
      sample.set("logical_edge_bytes", submitted);
      sample.set("submitted_bytes_complete", false);
      sample.set("skip_reason", error.what());
      sample.set("oracle_outcome", "failed");
      sample.set("oracle_passed", false);
      sample.write(output);
      throw;
    }
    sample.write(output);
  }
}

void run_builder(options const& r, std::ostream& output)
{
  require(r.bytes % std::lcm(op::bloom_bytes_per_block, rmm::CUDA_ALLOCATION_ALIGNMENT) == 0,
          "bitmap bytes must align to both a Bloom block and CUDA allocation");
  auto const sizing_rows = checked_product(
    r.bytes / op::bloom_bytes_per_block, op::bloom_bits_per_block / op::bloom_target_bits_per_key);
  auto const shape = geometry::try_create(sizing_rows, r.keys, r.cap_bytes);
  op::detail::publication_transport transport;
  if (r.transport == "automatic") {
    transport = op::detail::publication_transport::automatic;
  } else if (r.transport == "force_host") {
    transport = op::detail::publication_transport::force_host;
  } else if (r.transport == "require_direct") {
    transport = op::detail::publication_transport::require_direct;
  } else {
    throw std::invalid_argument("unknown builder transport");
  }
  if (!shape) {
    for (std::uint64_t i = 0; i < r.warmups + r.repetitions; ++i) {
      auto sample = sample_record(r, i);
      sample.set("K", r.keys);
      sample.set("B", r.bytes);
      skipped(sample, "bitmap_cap_refused");
      sample.write(output);
    }
    return;
  }
  require(shape->raw_bytes == r.bytes, "production sizing cannot represent requested bitmap bytes");
  if (!devices_available(r, output)) { return; }
  auto manager     = make_manager(r);
  auto const hosts = manager->get_memory_spaces_for_tier(tier::HOST);
  require(!hosts.empty(), "no HOST staging space");
  std::vector<op::dynamic_filter_replica_space> spaces;
  for (int device : r.devices) {
    auto const host = std::ranges::find_if(
      hosts, [device](auto* entry) { return entry->get_device_id() == device; });
    spaces.emplace_back(gpu(*manager, device), **(host == hosts.end() ? hosts.begin() : host));
  }
  auto contribution_order = r.contributors;
  std::erase(contribution_order, r.root);
  contribution_order.push_back(r.root);
  for (std::uint64_t i = 0; i < r.warmups + r.repetitions; ++i) {
    auto sample = sample_record(r, i);
    sample.set("K", r.keys);
    sample.set("B", shape->raw_bytes);
    sample.set("C", shape->chunk_bytes);
    sample.set("rows_per_contributor", r.rows);
    sample.set("requested_transport", r.transport);
    sample.set("timing_scope", "publication_prepare_through_completion");
    sample.array("contribution_order", std::span<int const>{contribution_order});
    std::vector<builder::key> keys{{0, cudf::data_type{cudf::type_id::INT32}}};
    if (r.keys == 2) { keys.push_back({1, cudf::data_type{cudf::type_id::INT64}}); }
    auto instance = builder::try_create(std::move(keys), *shape, spaces);
    if (!instance) {
      skipped(sample, "partial_array_resource_refused");
      sample.write(output);
      continue;
    }
    for (int contributor : contribution_order) {
      auto& owner = gpu(*manager, contributor);
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{contributor}};
      owned_stream stream{contributor};
      auto input = input_table(r, owner, stream, contributor);
      try {
        require(
          instance->enqueue_add(input->view(), rmm::cuda_device_id{contributor}, stream.ref()),
          "builder refused a configured contributor");
        stream.join();
      } catch (unjoined_gpu_work const&) {
        unjoined_failure(sample, output);
      } catch (...) {
        stream.join_noexcept();
        throw;
      }
    }
    std::vector<std::vector<std::byte>> canonical(r.keys, std::vector<std::byte>(r.bytes));
    for (std::size_t key = 0; key < r.keys; ++key) {
      for (int device : r.devices) {
        auto partial = instance->inspect_partial(key, rmm::cuda_device_id{device});
        require(partial && partial->bytes.size() == r.bytes,
                "partial snapshot missing or wrong size");
        for (std::size_t byte = 0; byte < r.bytes; ++byte) {
          canonical[key][byte] |= partial->bytes[byte];
        }
      }
    }
    op::detail::publication_measurement measurement{};
    owned_stream publication{r.root};
    try {
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{r.root}};
      auto const filters =
        instance->finish_for_experiment(rmm::cuda_device_id{r.root},
                                        publication.ref(),
                                        gpu(*manager, r.root).get_default_allocator(),
                                        transport,
                                        &measurement);
      measure(sample, measurement);
      if (!filters) {
        skipped(sample, measurement.refusal_reason);
        sample.write(output);
        continue;
      }
      require(filters->size() == r.keys, "builder returned wrong filter count");
      auto const root_space = std::ranges::find_if(
        spaces, [&](auto const& entry) { return entry.get_gpu_space().get_device_id() == r.root; });
      verify_measurement(r, *shape, root_space->get_host_staging_space(), measurement);
      for (std::size_t key = 0; key < r.keys; ++key) {
        for (int device : r.devices) {
          auto replica = builder::inspect_replica(*(*filters)[key], rmm::cuda_device_id{device});
          require(replica && replica->stream_idle, "replica not ready at return");
          require(replica->bytes == canonical[key], "replica differs from canonical every-bit OR");
        }
      }
      for (int device : r.devices) {
        rmm::cuda_set_device_raii probe_guard{rmm::cuda_device_id{device}};
        owned_stream probe_stream{device};
        for (int contributor : contribution_order) {
          auto probe = input_table(r, gpu(*manager, device), probe_stream, contributor);
          retire_before_release const retire_probe{probe_stream};
          for (std::size_t key = 0; key < r.keys && r.rows != 0; ++key) {
            std::vector<std::uint8_t> host(r.rows);
            auto mask =
              (*filters)[key]->compute_mask(probe->view().column(key),
                                            nullptr,
                                            device,
                                            probe_stream.ref(),
                                            gpu(*manager, device).get_default_allocator());
            retire_before_release const retire{probe_stream};
            require(mask && mask->type().id() == cudf::type_id::BOOL8 && !mask->nullable(),
                    "membership probe returned no valid mask");
            check(cudaMemcpyAsync(host.data(),
                                  mask->view().data<bool>(),
                                  host.size(),
                                  cudaMemcpyDeviceToHost,
                                  probe_stream.get()),
                  "membership download");
            probe_stream.join();
            require(std::ranges::all_of(host, [](auto value) { return value != 0; }),
                    "published Bloom has a false negative");
          }
        }
      }
      sample.set("canonical_bitmap_equal", true);
      sample.set("no_false_negatives", true);
      sample.set("replicas_checked", checked_product(r.keys, r.devices.size()));
      passed(sample);
    } catch (unjoined_gpu_work const&) {
      measure(sample, measurement);
      unjoined_failure(sample, output);
    } catch (std::exception const& error) {
      publication.join_noexcept();
      measure(sample, measurement);
      sample.set("skip_reason", error.what());
      sample.set("oracle_outcome", "failed");
      sample.set("oracle_passed", false);
      sample.write(output);
      throw;
    }
    sample.write(output);
  }
}

}  // namespace sirius::test::s4
