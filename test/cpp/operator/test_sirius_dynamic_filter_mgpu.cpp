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

// Focused regressions for device-local dynamic-filter replicas. Each case builds the producer-side
// filter on logical GPU 0, materializes device-local replicas, then consumes them from remote probe
// GPUs. Before replica support, the corresponding cross-device dereference is the
// cudaErrorIllegalAddress reported by TPC-H Q2.

#include "op/dynamic_filter/dynamic_filter_replica_transfer.hpp"
#include "op/dynamic_filter/sirius_dynamic_filter.hpp"
#include "operator_test_utils.hpp"

#include <cudf/ast/expressions.hpp>
#include <cudf/column/column_factories.hpp>
#include <cudf/filling.hpp>
#include <cudf/scalar/scalar.hpp>
#include <cudf/table/table_view.hpp>
#include <cudf/transform.hpp>
#include <cudf/utilities/default_stream.hpp>
#include <cudf/utilities/memory_resource.hpp>

#include <rmm/aligned.hpp>
#include <rmm/cuda_device.hpp>
#include <rmm/device_buffer.hpp>

#include <cuda_runtime.h>

#include <catch.hpp>
#include <cucascade/memory/common.hpp>
#include <cucascade/memory/fixed_size_host_memory_resource.hpp>
#include <cucascade/memory/reservation_aware_resource_adaptor.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

constexpr int kBuildDevice = 0;
constexpr int kProbeDevice = 1;
constexpr std::array<int, 2> kReplicaDevices{kBuildDevice, kProbeDevice};

bool require_two_gpus()
{
  int count      = 0;
  auto const err = cudaGetDeviceCount(&count);
  if (err != cudaSuccess || count < 2) {
    WARN("dynamic-filter replica test requires at least two visible GPUs; skipping");
    return false;
  }
  return true;
}

bool require_three_gpus()
{
  int count      = 0;
  auto const err = cudaGetDeviceCount(&count);
  if (err != cudaSuccess || count < 3) {
    WARN("dynamic-filter overlap test requires at least three visible GPUs; skipping");
    return false;
  }
  return true;
}

template <typename T>
std::unique_ptr<cudf::column> make_values(std::vector<T> const& values,
                                          cudf::data_type type,
                                          rmm::cuda_stream_view stream)
{
  auto col       = cudf::make_numeric_column(type,
                                       static_cast<cudf::size_type>(values.size()),
                                       cudf::mask_state::UNALLOCATED,
                                       stream,
                                       cudf::get_current_device_resource_ref());
  auto const err = cudaMemcpyAsync(col->mutable_view().data<T>(),
                                   values.data(),
                                   values.size() * sizeof(T),
                                   cudaMemcpyHostToDevice,
                                   stream.value());
  REQUIRE(err == cudaSuccess);
  // Callers commonly pass a temporary initializer vector. Complete the pageable-host transfer
  // before that vector is destroyed; these focused tests do not benchmark ingestion.
  stream.synchronize();
  return col;
}

std::vector<std::uint8_t> mask_to_host(cudf::column_view const& mask, rmm::cuda_stream_view stream)
{
  REQUIRE(mask.type().id() == cudf::type_id::BOOL8);
  std::vector<std::uint8_t> host(static_cast<std::size_t>(mask.size()));
  auto const err = cudaMemcpyAsync(host.data(),
                                   mask.data<bool>(),
                                   host.size() * sizeof(bool),
                                   cudaMemcpyDeviceToHost,
                                   stream.value());
  REQUIRE(err == cudaSuccess);
  stream.synchronize();
  return host;
}

template <typename Filter>
void replicate_to_both_devices(
  Filter& filter, std::span<sirius::op::dynamic_filter_replica_space const> replica_spaces)
{
  filter.replicate_to_devices(replica_spaces);
  REQUIRE(filter.is_available_on_device(kBuildDevice));
  REQUIRE(filter.is_available_on_device(kProbeDevice));
}

template <typename MemoryManager>
std::vector<sirius::op::dynamic_filter_replica_space> get_replica_spaces(
  MemoryManager& memory_manager, std::size_t expected_device_count = kReplicaDevices.size())
{
  auto const gpu_spaces  = memory_manager.get_memory_spaces_for_tier(cucascade::memory::Tier::GPU);
  auto const host_spaces = memory_manager.get_memory_spaces_for_tier(cucascade::memory::Tier::HOST);
  REQUIRE(gpu_spaces.size() == expected_device_count);
  REQUIRE_FALSE(host_spaces.empty());

  std::vector<sirius::op::dynamic_filter_replica_space> result;
  result.reserve(gpu_spaces.size());
  for (auto const* gpu_space_view : gpu_spaces) {
    auto* gpu_space = memory_manager.get_memory_space(cucascade::memory::Tier::GPU,
                                                      gpu_space_view->get_device_id());
    REQUIRE(gpu_space != nullptr);
    auto const local_host =
      std::find_if(host_spaces.begin(), host_spaces.end(), [gpu_space](auto const* host_space) {
        return host_space->get_device_id() == gpu_space->get_device_id();
      });
    auto const* host_space = local_host == host_spaces.end() ? host_spaces.front() : *local_host;
    result.emplace_back(*gpu_space, *host_space);
  }
  std::sort(result.begin(), result.end(), [](auto const& lhs, auto const& rhs) {
    return lhs.get_gpu_space().get_device_id() < rhs.get_gpu_space().get_device_id();
  });
  for (std::size_t i = 0; i < result.size(); ++i) {
    REQUIRE(result[i].get_gpu_space().get_device_id() == static_cast<int>(i));
  }
  return result;
}

}  // namespace

TEST_CASE("IN-list replica built on GPU 0 computes an exact mask on GPU 1",
          "[dynamic_filter][mgpu][replica][in_list]")
{
  if (!require_two_gpus()) { return; }

  auto memory_manager = sirius::test::operator_utils::initialize_memory_manager(2);
  auto replica_spaces = get_replica_spaces(*memory_manager);
  auto& target_space  = replica_spaces.back().get_gpu_space();
  auto* target_mr =
    target_space.get_memory_resource_as<cucascade::memory::reservation_aware_resource_adaptor>();
  REQUIRE(target_mr != nullptr);
  auto const target_reserved_before  = target_space.get_total_reserved_memory();
  auto const target_active_before    = target_space.get_active_reservation_count();
  auto const target_allocated_before = target_mr->get_total_allocated_bytes();
  std::unique_ptr<sirius::op::sirius_dynamic_in_list_filter> filter;
  {
    rmm::cuda_set_device_raii const build_device{rmm::cuda_device_id{kBuildDevice}};
    auto const& build_space = replica_spaces.front().get_gpu_space();
    auto const stream       = build_space.acquire_stream();
    auto keys = make_values<std::int64_t>({2, 4, 6}, cudf::data_type{cudf::type_id::INT64}, stream);
    filter    = std::make_unique<sirius::op::sirius_dynamic_in_list_filter>(
      keys->view(), stream, build_space.get_default_allocator());
    stream.synchronize();
    keys.reset();  // replication must not depend on the constructor's borrowed column_view

    REQUIRE(filter->replica_count() == 1);
    REQUIRE(filter->is_available_on_device(kBuildDevice));
    REQUIRE_FALSE(filter->is_available_on_device(kProbeDevice));

    // Exercise the low-level availability contract independently of production scheduling: before
    // explicit replication, a GPU 1 caller skips the optional filter and never touches GPU 0's set.
    {
      rmm::cuda_set_device_raii const probe_device{rmm::cuda_device_id{kProbeDevice}};
      auto const probe_stream = cudf::get_default_stream();
      auto early_probe =
        make_values<std::int64_t>({2, 7}, cudf::data_type{cudf::type_id::INT64}, probe_stream);
      auto early_mask = filter->compute_mask(
        early_probe->view(), kProbeDevice, probe_stream, cudf::get_current_device_resource_ref());
      REQUIRE(early_mask == nullptr);
    }

    replicate_to_both_devices(*filter, replica_spaces);
    REQUIRE(filter->replica_count() == kReplicaDevices.size());
    REQUIRE(target_space.get_total_reserved_memory() == target_reserved_before);
    REQUIRE(target_space.get_active_reservation_count() == target_active_before);
    REQUIRE(target_mr->get_total_allocated_bytes() > target_allocated_before);
  }

  {
    rmm::cuda_set_device_raii const probe_device{rmm::cuda_device_id{kProbeDevice}};
    auto const stream = cudf::get_default_stream();
    auto probe =
      make_values<std::int64_t>({1, 2, 3, 4, 6, 9}, cudf::data_type{cudf::type_id::INT64}, stream);
    auto mask = filter->compute_mask(
      probe->view(), kProbeDevice, stream, cudf::get_current_device_resource_ref());
    REQUIRE(mask != nullptr);
    REQUIRE(mask_to_host(mask->view(), stream) == std::vector<std::uint8_t>{0, 1, 0, 1, 1, 0});
  }

  rmm::cuda_set_device_raii const build_device{rmm::cuda_device_id{kBuildDevice}};
  filter.reset();
  REQUIRE(target_mr->get_total_allocated_bytes() == target_allocated_before);
}

TEST_CASE("dynamic-filter replicas require destination reservation admission",
          "[dynamic_filter][mgpu][replica][reservation]")
{
  if (!require_two_gpus()) { return; }

  auto memory_manager = sirius::test::operator_utils::initialize_memory_manager(2);
  auto replica_spaces = get_replica_spaces(*memory_manager);
  auto& target_space  = replica_spaces.back().get_gpu_space();
  auto* target_mr =
    target_space.get_memory_resource_as<cucascade::memory::reservation_aware_resource_adaptor>();
  REQUIRE(target_mr != nullptr);

  auto const allocated_before = target_mr->get_total_allocated_bytes();
  REQUIRE(allocated_before < target_space.get_max_memory());
  auto reservation_pressure =
    target_space.make_reservation_or_null(target_space.get_max_memory() - allocated_before);
  REQUIRE(reservation_pressure != nullptr);
  REQUIRE(target_space.get_available_memory() > (1U << 20));
  REQUIRE(target_space.make_reservation_or_null(rmm::CUDA_ALLOCATION_ALIGNMENT) == nullptr);

  auto require_omitted = [&](auto& filter) {
    auto const target_allocated = target_mr->get_total_allocated_bytes();
    filter.replicate_to_devices(replica_spaces);
    REQUIRE(filter.is_available_on_device(kBuildDevice));
    REQUIRE_FALSE(filter.is_available_on_device(kProbeDevice));
    REQUIRE(target_mr->get_total_allocated_bytes() == target_allocated);
  };

  rmm::cuda_set_device_raii const build_device{rmm::cuda_device_id{kBuildDevice}};
  auto& build_space = replica_spaces.front().get_gpu_space();
  auto const stream = build_space.acquire_stream();

  SECTION("IN-list")
  {
    auto keys = make_values<std::int64_t>({2, 4, 6}, cudf::data_type{cudf::type_id::INT64}, stream);
    sirius::op::sirius_dynamic_in_list_filter filter(
      keys->view(), stream, build_space.get_default_allocator());
    stream.synchronize();
    require_omitted(filter);
  }

  SECTION("Small IN-list")
  {
    auto keys = make_values<std::int64_t>({2, 4, 6}, cudf::data_type{cudf::type_id::INT64}, stream);
    sirius::op::sirius_dynamic_small_in_list_filter filter(
      keys->view(), stream, build_space.get_default_allocator());
    stream.synchronize();
    require_omitted(filter);
  }

  SECTION("Bloom")
  {
    auto keys = cudf::sequence(1024,
                               cudf::numeric_scalar<std::int64_t>(0, true, stream),
                               cudf::numeric_scalar<std::int64_t>(1, true, stream),
                               stream,
                               build_space.get_default_allocator());
    sirius::op::sirius_dynamic_bloom_filter filter(
      keys->view(), stream, build_space.get_default_allocator());
    stream.synchronize();
    require_omitted(filter);
  }
}

TEST_CASE("IN-list peer copies fan out to three GPUs before publication",
          "[dynamic_filter][mgpu][replica][peer_overlap]")
{
  if (!require_three_gpus()) { return; }

  constexpr std::size_t device_count = 3;
  auto memory_manager = sirius::test::operator_utils::initialize_memory_manager(device_count);
  auto replica_spaces = get_replica_spaces(*memory_manager, device_count);
  if (!cucascade::memory::probe_peer_dma_works(0, 1) ||
      !cucascade::memory::probe_peer_dma_works(0, 2)) {
    WARN(
      "dynamic-filter overlap test requires direct peer DMA from GPU 0 to GPUs 1 and 2; "
      "skipping");
    return;
  }
  std::unique_ptr<sirius::op::sirius_dynamic_in_list_filter> filter;

  {
    rmm::cuda_set_device_raii const build_device{rmm::cuda_device_id{kBuildDevice}};
    auto const& build_space = replica_spaces.front().get_gpu_space();
    auto const stream       = build_space.acquire_stream();
    auto keys = make_values<std::int64_t>({2, 4, 6}, cudf::data_type{cudf::type_id::INT64}, stream);
    filter    = std::make_unique<sirius::op::sirius_dynamic_in_list_filter>(
      keys->view(), stream, build_space.get_default_allocator());
    stream.synchronize();

    filter->replicate_to_devices(replica_spaces);
    REQUIRE(filter->replica_count() == device_count);
  }

  for (int device_id = 1; device_id < static_cast<int>(device_count); ++device_id) {
    rmm::cuda_set_device_raii const probe_device{rmm::cuda_device_id{device_id}};
    auto const& probe_space = replica_spaces[static_cast<std::size_t>(device_id)].get_gpu_space();
    auto const stream       = probe_space.acquire_stream();
    REQUIRE(filter->is_available_on_device(device_id));
    auto probe =
      make_values<std::int64_t>({1, 2, 4, 9}, cudf::data_type{cudf::type_id::INT64}, stream);
    auto mask =
      filter->compute_mask(probe->view(), device_id, stream, probe_space.get_default_allocator());
    REQUIRE(mask != nullptr);
    REQUIRE(mask_to_host(mask->view(), stream) == std::vector<std::uint8_t>{0, 1, 1, 0});
  }

  rmm::cuda_set_device_raii const build_device{rmm::cuda_device_id{kBuildDevice}};
  filter.reset();
}

TEST_CASE("dynamic-filter replica transfer borrows fixed blocks from a Sirius HOST space",
          "[dynamic_filter][mgpu][replica][transfer]")
{
  if (!require_two_gpus()) { return; }

  auto memory_manager      = sirius::test::operator_utils::initialize_memory_manager(2);
  auto replica_spaces      = get_replica_spaces(*memory_manager);
  auto const& source_space = replica_spaces.front().get_gpu_space();
  auto const& target       = replica_spaces.back();
  auto const& target_space = target.get_gpu_space();
  auto* const host_resource =
    target.get_host_staging_space()
      .get_memory_resource_as<cucascade::memory::fixed_size_host_memory_resource>();
  REQUIRE(host_resource != nullptr);

  // Exercise a real batch over three noncontiguous fixed blocks. Make each block's contents
  // distinct so a duplicated or reordered chunk cannot satisfy the exact-payload assertion.
  auto const block_size = host_resource->get_block_size();
  auto const bytes      = 2 * block_size + 4096;
  std::vector<std::byte> expected(bytes);
  for (std::size_t i = 0; i < expected.size(); ++i) {
    auto const block_index  = i / block_size;
    auto const block_offset = i % block_size;
    expected[i] = static_cast<std::byte>((block_offset * 131U + block_index * 17U) & 0xffU);
  }

  std::unique_ptr<rmm::device_buffer> source;
  {
    rmm::cuda_set_device_raii const build_device{rmm::cuda_device_id{kBuildDevice}};
    auto const stream = source_space.acquire_stream();
    source =
      std::make_unique<rmm::device_buffer>(bytes, stream, source_space.get_default_allocator());
    REQUIRE(cudaMemcpyAsync(
              source->data(), expected.data(), bytes, cudaMemcpyHostToDevice, stream.value()) ==
            cudaSuccess);
    stream.synchronize();
  }

  {
    rmm::cuda_set_device_raii const probe_device{rmm::cuda_device_id{kProbeDevice}};
    auto const stream = target_space.acquire_stream();
    rmm::device_buffer destination(bytes, stream, target_space.get_default_allocator());
    auto const allocated_before = host_resource->get_total_allocated_bytes();
    auto const route            = sirius::op::detail::enqueue_replica_copy(
      destination.data(),
      rmm::cuda_device_id{kProbeDevice},
      source->data(),
      source_space,
      bytes,
      stream,
      target.get_host_staging_space(),
      sirius::op::detail::replica_transfer_policy::force_host_staging);

    REQUIRE(route == sirius::op::detail::replica_transfer_route::host_staging);
    REQUIRE(host_resource->get_total_allocated_bytes() == allocated_before);

    int current_device = -1;
    REQUIRE(cudaGetDevice(&current_device) == cudaSuccess);
    REQUIRE(current_device == kProbeDevice);

    std::vector<std::byte> actual(bytes);
    auto const err = cudaMemcpyAsync(
      actual.data(), destination.data(), bytes, cudaMemcpyDeviceToHost, stream.value());
    REQUIRE(err == cudaSuccess);
    stream.synchronize();
    REQUIRE(actual == expected);
  }

  rmm::cuda_set_device_raii const build_device{rmm::cuda_device_id{kBuildDevice}};
  source.reset();
}

TEST_CASE("Bloom replica built on GPU 0 has no false negatives on GPU 1",
          "[dynamic_filter][mgpu][replica][bloom]")
{
  if (!require_two_gpus()) { return; }

  auto memory_manager = sirius::test::operator_utils::initialize_memory_manager(2);
  auto replica_spaces = get_replica_spaces(*memory_manager);
  std::unique_ptr<sirius::op::sirius_dynamic_bloom_filter> filter;
  {
    rmm::cuda_set_device_raii const build_device{rmm::cuda_device_id{kBuildDevice}};
    auto const& build_space = replica_spaces.front().get_gpu_space();
    auto const stream       = build_space.acquire_stream();
    auto keys               = cudf::sequence(1024,
                               cudf::numeric_scalar<std::int64_t>(0, true, stream),
                               cudf::numeric_scalar<std::int64_t>(1, true, stream),
                               stream,
                               build_space.get_default_allocator());
    filter                  = std::make_unique<sirius::op::sirius_dynamic_bloom_filter>(
      keys->view(), stream, build_space.get_default_allocator());
    stream.synchronize();
    keys.reset();  // replication must use filter-owned source material

    REQUIRE(filter->replica_count() == 1);
    REQUIRE(filter->is_available_on_device(kBuildDevice));
    REQUIRE_FALSE(filter->is_available_on_device(kProbeDevice));
    replicate_to_both_devices(*filter, replica_spaces);
    REQUIRE(filter->replica_count() == kReplicaDevices.size());
  }

  {
    rmm::cuda_set_device_raii const probe_device{rmm::cuda_device_id{kProbeDevice}};
    auto const stream = cudf::get_default_stream();
    // Positions 0, 2, and 4 are build keys. Misses may be Bloom false positives, so only the
    // no-false-negative contract is asserted for them.
    auto probe = make_values<std::int64_t>(
      {0, 2048, 511, 4096, 1023}, cudf::data_type{cudf::type_id::INT64}, stream);
    auto mask = filter->compute_mask(
      probe->view(), kProbeDevice, stream, cudf::get_current_device_resource_ref());
    REQUIRE(mask != nullptr);
    auto const host_mask = mask_to_host(mask->view(), stream);
    REQUIRE(host_mask.size() == 5);
    REQUIRE(host_mask[0] != 0);
    REQUIRE(host_mask[2] != 0);
    REQUIRE(host_mask[4] != 0);
  }

  rmm::cuda_set_device_raii const build_device{rmm::cuda_device_id{kBuildDevice}};
  filter.reset();
}

TEST_CASE("zone-map replica built on GPU 0 lowers and evaluates its AST on GPU 1",
          "[dynamic_filter][mgpu][replica][zone_map]")
{
  if (!require_two_gpus()) { return; }

  auto memory_manager = sirius::test::operator_utils::initialize_memory_manager(2);
  auto replica_spaces = get_replica_spaces(*memory_manager);
  std::unique_ptr<sirius::op::sirius_dynamic_zone_map_filter> filter;
  {
    rmm::cuda_set_device_raii const build_device{rmm::cuda_device_id{kBuildDevice}};
    auto const& build_space = replica_spaces.front().get_gpu_space();
    auto const stream       = build_space.acquire_stream();
    std::vector<sirius::op::zone_map_entry> zones;
    zones.push_back({std::make_unique<cudf::numeric_scalar<std::int64_t>>(
                       3, true, stream, build_space.get_default_allocator()),
                     std::make_unique<cudf::numeric_scalar<std::int64_t>>(
                       6, true, stream, build_space.get_default_allocator())});
    filter = std::make_unique<sirius::op::sirius_dynamic_zone_map_filter>(std::move(zones),
                                                                          /*inclusive_min=*/true,
                                                                          /*inclusive_max=*/true);
    stream.synchronize();

    REQUIRE(filter->is_available_on_device(kBuildDevice));
    REQUIRE_FALSE(filter->is_available_on_device(kProbeDevice));
    replicate_to_both_devices(*filter, replica_spaces);
  }

  {
    rmm::cuda_set_device_raii const probe_device{rmm::cuda_device_id{kProbeDevice}};
    auto const stream = cudf::get_default_stream();
    auto probe        = cudf::sequence(10,
                                cudf::numeric_scalar<std::int64_t>(0, true, stream),
                                cudf::numeric_scalar<std::int64_t>(1, true, stream),
                                stream,
                                cudf::get_current_device_resource_ref());
    std::vector<cudf::column_view> columns{probe->view()};
    cudf::table_view input{columns};

    cudf::ast::tree tree;
    auto const& col_ref = tree.emplace<cudf::ast::column_reference>(0);
    auto const& root    = filter->to_ast(tree, col_ref, kProbeDevice);
    auto mask = cudf::compute_column(input, root, stream, cudf::get_current_device_resource_ref());

    REQUIRE(mask_to_host(mask->view(), stream) ==
            std::vector<std::uint8_t>{0, 0, 0, 1, 1, 1, 1, 0, 0, 0});
  }

  rmm::cuda_set_device_raii const build_device{rmm::cuda_device_id{kBuildDevice}};
  filter.reset();
}

TEST_CASE("small IN-list replica built on GPU 0 computes an exact mask on GPU 1",
          "[dynamic_filter][mgpu][replica][small_in_list]")
{
  if (!require_two_gpus()) { return; }

  auto memory_manager = sirius::test::operator_utils::initialize_memory_manager(2);
  auto replica_spaces = get_replica_spaces(*memory_manager);
  auto& target_space  = replica_spaces.back().get_gpu_space();
  auto* target_mr =
    target_space.get_memory_resource_as<cucascade::memory::reservation_aware_resource_adaptor>();
  REQUIRE(target_mr != nullptr);
  auto const target_reserved_before  = target_space.get_total_reserved_memory();
  auto const target_active_before    = target_space.get_active_reservation_count();
  auto const target_allocated_before = target_mr->get_total_allocated_bytes();
  std::unique_ptr<sirius::op::sirius_dynamic_small_in_list_filter> filter;
  {
    rmm::cuda_set_device_raii const build_device{rmm::cuda_device_id{kBuildDevice}};
    auto const& build_space = replica_spaces.front().get_gpu_space();
    auto const stream       = build_space.acquire_stream();
    auto keys = make_values<std::int64_t>({2, 4, 6}, cudf::data_type{cudf::type_id::INT64}, stream);
    filter    = std::make_unique<sirius::op::sirius_dynamic_small_in_list_filter>(
      keys->view(), stream, build_space.get_default_allocator());
    stream.synchronize();
    keys.reset();  // replication must use the filter-owned needle snapshot

    REQUIRE(filter->replica_count() == 1);
    REQUIRE(filter->is_available_on_device(kBuildDevice));
    REQUIRE_FALSE(filter->is_available_on_device(kProbeDevice));

    // A remote caller must skip the optional filter until its device-local needle snapshot is
    // published; it must never dereference the source GPU's buffer.
    {
      rmm::cuda_set_device_raii const probe_device{rmm::cuda_device_id{kProbeDevice}};
      auto const probe_stream = cudf::get_default_stream();
      auto early_probe =
        make_values<std::int64_t>({2, 7}, cudf::data_type{cudf::type_id::INT64}, probe_stream);
      auto early_mask = filter->compute_mask(
        early_probe->view(), kProbeDevice, probe_stream, cudf::get_current_device_resource_ref());
      REQUIRE(early_mask == nullptr);
    }

    replicate_to_both_devices(*filter, replica_spaces);
    REQUIRE(filter->replica_count() == kReplicaDevices.size());
    REQUIRE(target_space.get_total_reserved_memory() == target_reserved_before);
    REQUIRE(target_space.get_active_reservation_count() == target_active_before);
    REQUIRE(target_mr->get_total_allocated_bytes() > target_allocated_before);
  }

  {
    rmm::cuda_set_device_raii const probe_device{rmm::cuda_device_id{kProbeDevice}};
    auto const stream = cudf::get_default_stream();
    auto probe =
      make_values<std::int64_t>({1, 2, 3, 4, 6, 9}, cudf::data_type{cudf::type_id::INT64}, stream);
    auto mask = filter->compute_mask(
      probe->view(), kProbeDevice, stream, cudf::get_current_device_resource_ref());
    REQUIRE(mask != nullptr);
    REQUIRE(mask_to_host(mask->view(), stream) == std::vector<std::uint8_t>{0, 1, 0, 1, 1, 0});
  }

  rmm::cuda_set_device_raii const build_device{rmm::cuda_device_id{kBuildDevice}};
  filter.reset();
  REQUIRE(target_space.get_total_reserved_memory() == target_reserved_before);
  REQUIRE(target_space.get_active_reservation_count() == target_active_before);
  REQUIRE(target_mr->get_total_allocated_bytes() == target_allocated_before);
}

// Pool-level peer access is what SiriusContext::initialize() grants for every probe-verified
// pair so that cudaMemcpyPeerAsync between two cudaMallocAsync pools is a direct DMA instead of
// the driver's host-staged copy. The grant is exercised here on a bare memory manager: the probe
// GPU's pool must report ProtReadWrite for the build GPU afterwards and a peer pull issued from
// the build GPU must read the probe GPU's pool memory intact.
TEST_CASE("pool peer access grant makes the probe GPU's pool reachable from the build GPU",
          "[dynamic_filter][mgpu][pool_peer_access]")
{
  if (!require_two_gpus()) { return; }

  auto memory_manager =
    sirius::test::operator_utils::initialize_memory_manager(kReplicaDevices.size());
  auto replica_spaces = get_replica_spaces(*memory_manager);
  // The grant requires the probe to pass in both directions (the peer may pull from and push into
  // the pool), so an asymmetric-broken pair must skip rather than fail.
  if (!cucascade::memory::probe_peer_dma_works(kProbeDevice, kBuildDevice) ||
      !cucascade::memory::probe_peer_dma_works(kBuildDevice, kProbeDevice)) {
    WARN("pool peer access test requires direct peer DMA between GPU 0 and GPU 1; skipping");
    return;
  }

  auto const& build_space = replica_spaces.front().get_gpu_space();
  auto const& probe_space = replica_spaces.back().get_gpu_space();
  auto const* probe_mr =
    probe_space.get_memory_resource_as<cucascade::memory::reservation_aware_resource_adaptor>();
  REQUIRE(probe_mr != nullptr);
  cudaMemPool_t const probe_pool = probe_mr->pool_handle();
  REQUIRE(probe_pool != nullptr);

  using cucascade::memory::grant_pool_peer_access;
  using cucascade::memory::pool_peer_access_status;

  auto const granted = grant_pool_peer_access(probe_pool, kProbeDevice, kBuildDevice);
  REQUIRE(granted.status == pool_peer_access_status::granted);
  REQUIRE(granted.error == cudaSuccess);

  cudaMemAccessFlags flags{};
  cudaMemLocation location{};
  location.type = cudaMemLocationTypeDevice;
  location.id   = kBuildDevice;
  REQUIRE(cudaMemPoolGetAccess(&flags, probe_pool, &location) == cudaSuccess);
  CHECK(flags == cudaMemAccessFlagsProtReadWrite);

  // Idempotent, and a device trivially has access to its own pool.
  CHECK(grant_pool_peer_access(probe_pool, kProbeDevice, kBuildDevice).status ==
        pool_peer_access_status::granted);
  CHECK(grant_pool_peer_access(probe_pool, kProbeDevice, kProbeDevice).status ==
        pool_peer_access_status::granted);

  constexpr std::size_t kBytes = 1u << 20;
  constexpr int kPattern       = 0x5A;
  std::vector<std::uint8_t> host(kBytes);
  {
    rmm::cuda_set_device_raii const probe_device{rmm::cuda_device_id{kProbeDevice}};
    auto const probe_stream = probe_space.acquire_stream();
    rmm::device_buffer source(kBytes, probe_stream, probe_space.get_default_allocator());
    REQUIRE(cudaMemsetAsync(source.data(), kPattern, kBytes, probe_stream.value()) == cudaSuccess);
    probe_stream.synchronize();

    rmm::cuda_set_device_raii const build_device{rmm::cuda_device_id{kBuildDevice}};
    auto const build_stream = build_space.acquire_stream();
    rmm::device_buffer destination(kBytes, build_stream, build_space.get_default_allocator());
    REQUIRE(cudaMemcpyPeerAsync(destination.data(),
                                kBuildDevice,
                                source.data(),
                                kProbeDevice,
                                kBytes,
                                build_stream.value()) == cudaSuccess);
    REQUIRE(
      cudaMemcpyAsync(
        host.data(), destination.data(), kBytes, cudaMemcpyDeviceToHost, build_stream.value()) ==
      cudaSuccess);
    build_stream.synchronize();
    // Reverse destruction order frees `destination` with the build GPU current and, once the
    // inner guard has restored it, `source` with the probe GPU current.
  }
  CHECK(std::ranges::all_of(host, [](std::uint8_t byte) { return byte == kPattern; }));
}

namespace {

constexpr auto kInt64Type = cudf::data_type{cudf::type_id::INT64};

// Visible GPUs usable as one replica plan (at most four), or 0 when fewer than two are visible.
std::size_t pipeline_device_count()
{
  int count = 0;
  if (cudaGetDeviceCount(&count) != cudaSuccess || count < 2) {
    WARN("dynamic-filter pipeline test requires at least two visible GPUs; skipping");
    return 0;
  }
  return static_cast<std::size_t>(std::min(count, 4));
}

bool pipeline_peer_dma_available(std::size_t device_count, int root_device)
{
  for (int device = 0; device < static_cast<int>(device_count); ++device) {
    if (device == root_device) { continue; }
    if (!cucascade::memory::probe_peer_dma_works(device, root_device) ||
        !cucascade::memory::probe_peer_dma_works(root_device, device)) {
      WARN("dynamic-filter pipeline test requires direct peer DMA with the root GPU; skipping");
      return false;
    }
  }
  return true;
}

// Distinct pseudo-random key sets per GPU (xorshift64), so a skipped, duplicated or misplaced
// chunk changes the OR result.
std::vector<std::int64_t> pipeline_keys(std::uint64_t seed, std::size_t count)
{
  std::vector<std::int64_t> keys(count);
  std::uint64_t state = 0x9E3779B97F4A7C15ULL * (seed + 1);
  for (auto& key : keys) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    key = static_cast<std::int64_t>(state >> 1);
  }
  return keys;
}

// Mask of `probe` on `device_id` through `filter`, on a pooled stream of that GPU's space.
std::vector<std::uint8_t> pipeline_mask(sirius::op::sirius_dynamic_bloom_filter const& filter,
                                        std::vector<std::int64_t> const& probe,
                                        int device_id,
                                        cucascade::memory::memory_space const& space)
{
  rmm::cuda_set_device_raii const device{rmm::cuda_device_id{device_id}};
  auto const stream = space.acquire_stream();
  auto column       = make_values<std::int64_t>(probe, kInt64Type, stream);
  auto mask = filter.compute_mask(column->view(), device_id, stream, space.get_default_allocator());
  REQUIRE(mask != nullptr);
  return mask_to_host(mask->view(), stream);
}

}  // namespace

TEST_CASE("pipelined publication ORs every partial into bit-identical replicas on every GPU",
          "[dynamic_filter][mgpu][bloom][publication_pipeline]")
{
  auto const device_count = pipeline_device_count();
  if (device_count == 0) { return; }
  // The final contributor roots production publications; use the last GPU so a root-is-GPU-0
  // assumption cannot pass by accident.
  auto const root_device = static_cast<int>(device_count - 1);
  if (!pipeline_peer_dma_available(device_count, root_device)) { return; }

  // Auto (2 MiB for this footprint), many small chunks (exercises double-buffer reuse), and one
  // chunk larger than the filter.
  auto const requested_chunk_bytes =
    GENERATE(std::size_t{0}, std::size_t{64} << 10, std::size_t{1} << 30);
  CAPTURE(device_count, root_device, requested_chunk_bytes);

  auto memory_manager = sirius::test::operator_utils::initialize_memory_manager(device_count);
  auto replica_spaces = get_replica_spaces(*memory_manager, device_count);

  constexpr std::size_t keys_per_device = 1 << 19;  // 4 MB Bloom per filter
  auto const total_rows                 = keys_per_device * device_count;
  std::vector<std::vector<std::int64_t>> keys(device_count);
  std::vector<std::unique_ptr<sirius::op::sirius_dynamic_bloom_filter>> partials(device_count);
  for (std::size_t device = 0; device < device_count; ++device) {
    keys[device] = pipeline_keys(device, keys_per_device);
    rmm::cuda_set_device_raii const guard{rmm::cuda_device_id{static_cast<int>(device)}};
    auto const& space = replica_spaces[device].get_gpu_space();
    auto const stream = space.acquire_stream();
    partials[device]  = std::make_unique<sirius::op::sirius_dynamic_bloom_filter>(
      kInt64Type, total_rows, stream, space.get_default_allocator());
    auto column = make_values<std::int64_t>(keys[device], kInt64Type, stream);
    partials[device]->add(column->view(), stream);
    stream.synchronize();
  }

  // Reference: every key inserted into one filter of the same geometry on the root. Bloom
  // insertion ORs bits, so the reduced filter must be bit-identical to it.
  std::unique_ptr<sirius::op::sirius_dynamic_bloom_filter> reference;
  {
    rmm::cuda_set_device_raii const guard{rmm::cuda_device_id{root_device}};
    auto const& space = replica_spaces[root_device].get_gpu_space();
    auto const stream = space.acquire_stream();
    reference         = std::make_unique<sirius::op::sirius_dynamic_bloom_filter>(
      kInt64Type, total_rows, stream, space.get_default_allocator());
    for (auto const& device_keys : keys) {
      auto column = make_values<std::int64_t>(device_keys, kInt64Type, stream);
      reference->add(column->view(), stream);
      stream.synchronize();
    }
  }

  auto& root = *partials[root_device];
  {
    std::vector<sirius::op::sirius_dynamic_bloom_publication_pipeline::source_partial> sources;
    for (std::size_t device = 0; device < device_count; ++device) {
      if (static_cast<int>(device) == root_device) { continue; }
      sources.push_back({partials[device].get(), &replica_spaces[device]});
    }
    rmm::cuda_set_device_raii const guard{rmm::cuda_device_id{root_device}};
    sirius::op::sirius_dynamic_bloom_publication_pipeline pipeline{
      replica_spaces[root_device], replica_spaces, requested_chunk_bytes};
    pipeline.enqueue(root, sources);
    // Nothing is visible before completion.
    for (std::size_t device = 0; device < device_count; ++device) {
      CHECK(root.is_available_on_device(static_cast<int>(device)) ==
            (static_cast<int>(device) == root_device));
    }
    pipeline.complete();
  }
  REQUIRE(root.replica_count() == device_count);

  // Probe every inserted key (no false negatives anywhere) plus keys nobody inserted; the masks
  // on every GPU must equal the reference mask exactly.
  std::vector<std::int64_t> probe;
  probe.reserve(total_rows + keys_per_device);
  for (auto const& device_keys : keys) {
    probe.insert(probe.end(), device_keys.begin(), device_keys.end());
  }
  auto const absent = pipeline_keys(device_count + 7, keys_per_device);
  probe.insert(probe.end(), absent.begin(), absent.end());
  auto const expected =
    pipeline_mask(*reference, probe, root_device, replica_spaces[root_device].get_gpu_space());
  REQUIRE(std::all_of(expected.begin(),
                      expected.begin() + static_cast<std::ptrdiff_t>(total_rows),
                      [](std::uint8_t keep) { return keep != 0; }));
  // A Bloom this size at 16 bits per key rejects most absent keys; a mask of all ones would
  // make the equality below vacuous.
  REQUIRE(std::count(expected.begin() + static_cast<std::ptrdiff_t>(total_rows),
                     expected.end(),
                     0) > static_cast<std::ptrdiff_t>(keys_per_device / 2));
  for (std::size_t device = 0; device < device_count; ++device) {
    CAPTURE(device);
    REQUIRE(root.is_available_on_device(static_cast<int>(device)));
    CHECK(pipeline_mask(
            root, probe, static_cast<int>(device), replica_spaces[device].get_gpu_space()) ==
          expected);
  }

  for (std::size_t device = device_count; device-- > 0;) {
    rmm::cuda_set_device_raii const guard{rmm::cuda_device_id{static_cast<int>(device)}};
    if (static_cast<int>(device) == root_device) { reference.reset(); }
    partials[device].reset();
  }
}

TEST_CASE("pipelined publication with no remote partial still replicates the root filter",
          "[dynamic_filter][mgpu][bloom][publication_pipeline]")
{
  if (!require_two_gpus()) { return; }
  if (!pipeline_peer_dma_available(2, kBuildDevice)) { return; }

  auto memory_manager = sirius::test::operator_utils::initialize_memory_manager(2);
  auto replica_spaces = get_replica_spaces(*memory_manager);
  auto const keys     = pipeline_keys(3, 4096);
  std::unique_ptr<sirius::op::sirius_dynamic_bloom_filter> filter;
  {
    rmm::cuda_set_device_raii const guard{rmm::cuda_device_id{kBuildDevice}};
    auto const& space = replica_spaces.front().get_gpu_space();
    auto const stream = space.acquire_stream();
    filter            = std::make_unique<sirius::op::sirius_dynamic_bloom_filter>(
      kInt64Type, keys.size(), stream, space.get_default_allocator());
    auto column = make_values<std::int64_t>(keys, kInt64Type, stream);
    filter->add(column->view(), stream);
    stream.synchronize();

    sirius::op::sirius_dynamic_bloom_publication_pipeline pipeline{replica_spaces.front(),
                                                                   replica_spaces};
    pipeline.enqueue(*filter, {});
    pipeline.complete();
  }
  REQUIRE(filter->replica_count() == 2);
  REQUIRE(filter->is_available_on_device(kProbeDevice));
  auto const on_probe =
    pipeline_mask(*filter, keys, kProbeDevice, replica_spaces.back().get_gpu_space());
  CHECK(std::all_of(on_probe.begin(), on_probe.end(), [](std::uint8_t keep) { return keep != 0; }));

  rmm::cuda_set_device_raii const guard{rmm::cuda_device_id{kBuildDevice}};
  filter.reset();
}

TEST_CASE("pipelined publication fails closed before any DMA when a target reservation is denied",
          "[dynamic_filter][mgpu][bloom][publication_pipeline][reservation]")
{
  if (!require_two_gpus()) { return; }
  if (!pipeline_peer_dma_available(2, kBuildDevice)) { return; }

  auto memory_manager = sirius::test::operator_utils::initialize_memory_manager(2);
  auto replica_spaces = get_replica_spaces(*memory_manager);
  auto& target_space  = replica_spaces.back().get_gpu_space();
  auto* target_mr =
    target_space.get_memory_resource_as<cucascade::memory::reservation_aware_resource_adaptor>();
  REQUIRE(target_mr != nullptr);

  auto const keys = pipeline_keys(5, 4096);
  std::unique_ptr<sirius::op::sirius_dynamic_bloom_filter> root;
  std::unique_ptr<sirius::op::sirius_dynamic_bloom_filter> partial;
  {
    // The partial lives on the target GPU (as a contribution would leave it), before the target
    // is put under reservation pressure.
    rmm::cuda_set_device_raii const guard{rmm::cuda_device_id{kProbeDevice}};
    auto const stream = target_space.acquire_stream();
    partial           = std::make_unique<sirius::op::sirius_dynamic_bloom_filter>(
      kInt64Type, keys.size(), stream, target_space.get_default_allocator());
    auto column = make_values<std::int64_t>(keys, kInt64Type, stream);
    partial->add(column->view(), stream);
    stream.synchronize();
  }
  auto const allocated_with_partial = target_mr->get_total_allocated_bytes();
  REQUIRE(allocated_with_partial < target_space.get_max_memory());
  auto reservation_pressure =
    target_space.make_reservation_or_null(target_space.get_max_memory() - allocated_with_partial);
  REQUIRE(reservation_pressure != nullptr);
  REQUIRE(target_space.make_reservation_or_null(rmm::CUDA_ALLOCATION_ALIGNMENT) == nullptr);
  // The pressure arena itself is accounted; nothing may be added on top of it.
  auto const allocated_under_pressure = target_mr->get_total_allocated_bytes();
  {
    rmm::cuda_set_device_raii const guard{rmm::cuda_device_id{kBuildDevice}};
    auto const& space = replica_spaces.front().get_gpu_space();
    auto const stream = space.acquire_stream();
    root              = std::make_unique<sirius::op::sirius_dynamic_bloom_filter>(
      kInt64Type, keys.size(), stream, space.get_default_allocator());
    stream.synchronize();

    sirius::op::sirius_dynamic_bloom_publication_pipeline pipeline{replica_spaces.front(),
                                                                   replica_spaces};
    std::array<sirius::op::sirius_dynamic_bloom_publication_pipeline::source_partial, 1> const
      sources{{{partial.get(), &replica_spaces.back()}}};
    REQUIRE_THROWS_AS(pipeline.enqueue(*root, sources), std::runtime_error);
    // Destroying an incomplete pipeline drains its streams; nothing was enqueued here.
  }
  CHECK(root->replica_count() == 1);
  CHECK_FALSE(root->is_available_on_device(kProbeDevice));
  CHECK(target_mr->get_total_allocated_bytes() == allocated_under_pressure);

  {
    rmm::cuda_set_device_raii const guard{rmm::cuda_device_id{kProbeDevice}};
    partial.reset();
  }
  rmm::cuda_set_device_raii const guard{rmm::cuda_device_id{kBuildDevice}};
  root.reset();
}

TEST_CASE("pipelined publication chunks are block-aligned and bounded by the filter",
          "[dynamic_filter][bloom][publication_pipeline][chunk]")
{
  using pipeline            = sirius::op::sirius_dynamic_bloom_publication_pipeline;
  constexpr std::size_t mib = 1 << 20;
  // Automatic: clamp(bytes / 8, 2 MiB, 8 MiB).
  CHECK(pipeline::resolve_chunk_bytes(146'000'000, pipeline::k_auto_chunk_bytes) == 8 * mib);
  CHECK(pipeline::resolve_chunk_bytes(5'900'000, pipeline::k_auto_chunk_bytes) == 2 * mib);
  auto const mid = pipeline::resolve_chunk_bytes(29'400'000, pipeline::k_auto_chunk_bytes);
  CHECK(mid >= 29'400'000 / 8);
  CHECK(mid < 29'400'000 / 8 + 32);
  CHECK(mid % 32 == 0);
  // Never larger than the filter; a tiny filter is one chunk.
  CHECK(pipeline::resolve_chunk_bytes(1024, pipeline::k_auto_chunk_bytes) == 1024);
  // Explicit requests round up to the 32-byte Bloom block.
  CHECK(pipeline::resolve_chunk_bytes(4096, 100) == 128);
  CHECK(pipeline::resolve_chunk_bytes(4096, 4096) == 4096);
  CHECK(pipeline::resolve_chunk_bytes(4096, 1 << 30) == 4096);
  CHECK(pipeline::resolve_chunk_bytes(32, 1) == 32);
}
