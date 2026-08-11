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

#include "op/dynamic_filter/dynamic_filter_stats.hpp"
#include "op/dynamic_filter/partition_dynamic_filter_bank.hpp"
#include "op/dynamic_filter/sirius_dynamic_filter.hpp"
#include "operator_test_utils.hpp"

#include <cudf/column/column_factories.hpp>
#include <cudf/filling.hpp>
#include <cudf/scalar/scalar.hpp>
#include <cudf/table/table_view.hpp>

#include <rmm/cuda_device.hpp>

#include <cuda_runtime_api.h>

#include <catch.hpp>
#include <cucascade/memory/common.hpp>
#include <cucascade/memory/memory_space.hpp>

#include <barrier>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr int kDeviceId = 0;
constexpr auto kInt32   = cudf::data_type{cudf::type_id::INT32};
constexpr auto kInt64   = cudf::data_type{cudf::type_id::INT64};

using sirius::op::dynamic_filter_publication_policy;
using sirius::op::dynamic_filter_publish_plan;
using sirius::op::dynamic_filter_stats;
using sirius::op::partition_dynamic_filter_bank;
using sirius::op::sirius_dynamic_filter_set;

struct bank_fixture {
  rmm::cuda_set_device_raii device{rmm::cuda_device_id{kDeviceId}};
  decltype(sirius::test::operator_utils::initialize_memory_manager(1)) memory_manager =
    sirius::test::operator_utils::initialize_memory_manager(1);
  dynamic_filter_stats stats;

  [[nodiscard]] cucascade::memory::memory_space& gpu_space() const
  {
    auto* space = memory_manager->get_memory_space(cucascade::memory::Tier::GPU, kDeviceId);
    REQUIRE(space != nullptr);
    return *space;
  }
};

dynamic_filter_publish_plan make_plan(dynamic_filter_publication_policy policy = {},
                                      std::size_t domain_cardinality           = 0,
                                      bool build_key_proven_unique             = false,
                                      cudf::data_type probe_type               = kInt64,
                                      std::size_t key_count                    = 1)
{
  using admitted_key = dynamic_filter_publish_plan::admitted_key;
  std::vector<admitted_key> keys;
  keys.reserve(key_count);
  for (std::size_t ordinal = 0; ordinal < key_count; ++ordinal) {
    auto const key_ordinal = static_cast<cudf::size_type>(ordinal);
    keys.push_back({.planner_condition_index      = ordinal,
                    .build_key_ordinal            = key_ordinal,
                    .probe_key_ordinal            = key_ordinal,
                    .storage_type                 = kInt64,
                    .probe_storage_type           = probe_type,
                    .key_shape                    = {},
                    .build_key_domain_cardinality = domain_cardinality,
                    .build_key_proven_unique      = build_key_proven_unique});
  }
  return dynamic_filter_publish_plan{std::move(keys), {}, {}, policy};
}

std::unique_ptr<cudf::column> make_sequence(cucascade::memory::memory_space& space,
                                            rmm::cuda_stream_view stream,
                                            std::size_t rows,
                                            std::int64_t first)
{
  return cudf::sequence(static_cast<cudf::size_type>(rows),
                        cudf::numeric_scalar<std::int64_t>(first, true, stream),
                        cudf::numeric_scalar<std::int64_t>(1, true, stream),
                        stream,
                        space.get_default_allocator());
}

std::unique_ptr<cudf::column> make_empty_int64(cucascade::memory::memory_space& space,
                                               rmm::cuda_stream_view stream)
{
  return cudf::make_numeric_column(
    kInt64, 0, cudf::mask_state::UNALLOCATED, stream, space.get_default_allocator());
}

cudf::table_view one_column_view(cudf::column const& column)
{
  return cudf::table_view{std::vector<cudf::column_view>{column.view()}};
}

std::vector<std::uint8_t> membership_mask(sirius_dynamic_filter_set const& filters,
                                          cudf::column_view const& probe,
                                          cucascade::memory::memory_space& space,
                                          rmm::cuda_stream_view stream)
{
  auto const published = filters.filters_for_column(0);
  REQUIRE(published.size() == 1);
  auto const* applicable =
    dynamic_cast<sirius::op::sirius_mask_applicable const*>(published.front().get());
  REQUIRE(applicable != nullptr);
  auto mask = applicable->compute_mask(probe, kDeviceId, stream, space.get_default_allocator());
  REQUIRE(mask != nullptr);

  std::vector<std::uint8_t> host(static_cast<std::size_t>(mask->size()));
  auto const error = cudaMemcpyAsync(host.data(),
                                     mask->view().data<bool>(),
                                     host.size() * sizeof(bool),
                                     cudaMemcpyDeviceToHost,
                                     stream.value());
  REQUIRE(error == cudaSuccess);
  stream.synchronize();
  return host;
}

}  // namespace

TEST_CASE("partition Bloom bank unions every fragment before exposing a skewed partition",
          "[dynamic_filter][partition_dynamic_filter_bank]")
{
  bank_fixture fixture;
  auto& space       = fixture.gpu_space();
  auto const stream = space.acquire_stream();
  auto plan         = make_plan();
  partition_dynamic_filter_bank bank{plan, &fixture.stats};

  REQUIRE(bank.current_state() == partition_dynamic_filter_bank::state::pending_arm);
  REQUIRE(bank.may_apply_to_probe());
  REQUIRE(bank.select(0, kDeviceId).result ==
          partition_dynamic_filter_bank::selection::status::not_ready);
  REQUIRE_FALSE(bank.probe_may_proceed());
  bank.record_readiness_wait();

  REQUIRE(bank.arm(6, 2, 1));
  REQUIRE(bank.may_apply_to_probe());
  REQUIRE(bank.rows_per_partition_geometry() == 3);
  auto first  = make_sequence(space, stream, 3, 0);
  auto second = make_sequence(space, stream, 3, 3);
  bank.contribute(0, one_column_view(*first), kDeviceId, space, stream);
  bank.contribute(0, one_column_view(*second), kDeviceId, space, stream);

  auto const before_seal = bank.select(0, kDeviceId);
  REQUIRE(before_seal.result == partition_dynamic_filter_bank::selection::status::not_ready);
  REQUIRE(before_seal.filters == nullptr);
  bank.seal();

  REQUIRE(bank.current_state() == partition_dynamic_filter_bank::state::sealed);
  REQUIRE(bank.may_apply_to_probe());
  REQUIRE(bank.probe_may_proceed());
  auto const selected = bank.select(0, kDeviceId);
  REQUIRE(selected.result == partition_dynamic_filter_bank::selection::status::available);
  REQUIRE(selected.filters != nullptr);
  auto probe = make_sequence(space, stream, 6, 0);
  REQUIRE(membership_mask(*selected.filters, probe->view(), space, stream) ==
          std::vector<std::uint8_t>(6, 1));
  REQUIRE(bank.select(1, kDeviceId).result ==
          partition_dynamic_filter_bank::selection::status::missing);

  auto const stats = fixture.stats.snapshot();
  REQUIRE(stats.partition_dynamic_filter_readiness_waits == 1);
  REQUIRE(stats.partition_dynamic_filter_build_fragments == 2);
  REQUIRE(stats.partition_dynamic_filter_build_rows == 6);
  REQUIRE(stats.partition_dynamic_filter_partitions_built == 1);
  REQUIRE(stats.partition_dynamic_filter_filters_built == 1);
  REQUIRE(stats.partition_dynamic_filter_skewed_partitions == 1);
  REQUIRE(stats.filters_pushed == 0);
}

TEST_CASE("partition Bloom bank selects only the exact partition and actual device",
          "[dynamic_filter][partition_dynamic_filter_bank][routing]")
{
  bank_fixture fixture;
  auto& space       = fixture.gpu_space();
  auto const stream = space.acquire_stream();
  auto plan         = make_plan();
  partition_dynamic_filter_bank bank{plan, &fixture.stats};
  REQUIRE(bank.arm(4, 2, 1));

  auto partition_zero = make_sequence(space, stream, 2, 0);
  auto partition_one  = make_sequence(space, stream, 2, 100);
  bank.contribute(0, one_column_view(*partition_zero), kDeviceId, space, stream);
  bank.contribute(1, one_column_view(*partition_one), kDeviceId, space, stream);
  bank.seal();

  auto const selected_zero = bank.select(0, kDeviceId);
  auto const selected_one  = bank.select(1, kDeviceId);
  REQUIRE(selected_zero.result == partition_dynamic_filter_bank::selection::status::available);
  REQUIRE(selected_one.result == partition_dynamic_filter_bank::selection::status::available);
  REQUIRE(selected_zero.filters != selected_one.filters);
  REQUIRE(membership_mask(*selected_zero.filters, partition_zero->view(), space, stream) ==
          std::vector<std::uint8_t>(2, 1));
  REQUIRE(membership_mask(*selected_one.filters, partition_one->view(), space, stream) ==
          std::vector<std::uint8_t>(2, 1));

  auto const mismatch = bank.select(0, kDeviceId + 1);
  REQUIRE(mismatch.result == partition_dynamic_filter_bank::selection::status::device_mismatch);
  REQUIRE(mismatch.filters == nullptr);
  REQUIRE(fixture.stats.snapshot().partition_dynamic_filter_device_mismatches == 1);
}

TEST_CASE("partition Bloom bank policy gates fail open before allocation",
          "[dynamic_filter][partition_dynamic_filter_bank][policy]")
{
  SECTION("aggregate Bloom cap")
  {
    dynamic_filter_stats stats;
    auto plan = make_plan({.max_bloom_bytes_per_gpu = 0});
    partition_dynamic_filter_bank bank{plan, &stats};

    REQUIRE_FALSE(bank.arm(4, 2, 1));
    REQUIRE(bank.current_state() == partition_dynamic_filter_bank::state::disabled);
    REQUIRE_FALSE(bank.may_apply_to_probe());
    REQUIRE(bank.probe_may_proceed());
    REQUIRE(bank.select(0, kDeviceId).result ==
            partition_dynamic_filter_bank::selection::status::missing);
    auto const snapshot = stats.snapshot();
    REQUIRE(snapshot.partition_dynamic_filter_keys_considered == 1);
    REQUIRE(snapshot.partition_dynamic_filter_budget_skips == 1);
  }

  SECTION("aggregate cap admits equality and rejects one byte below")
  {
    constexpr std::size_t kBuildRows  = 100;
    constexpr std::size_t kPartitions = 5;
    constexpr std::size_t kActiveGpus = 2;
    auto const bloom_bytes = sirius::op::sirius_dynamic_bloom_filter::estimated_bytes(20);
    auto const exact_cap   = bloom_bytes * 2 * 3;

    dynamic_filter_stats equality_stats;
    auto equality_plan = make_plan({.max_bloom_bytes_per_gpu = exact_cap}, 0, false, kInt64, 2);
    partition_dynamic_filter_bank equality_bank{equality_plan, &equality_stats};
    REQUIRE(equality_bank.arm(kBuildRows, kPartitions, kActiveGpus));
    REQUIRE(equality_bank.rows_per_partition_geometry() == 20);
    REQUIRE(equality_stats.snapshot().partition_dynamic_filter_budget_skips == 0);
    equality_bank.disable();

    dynamic_filter_stats below_stats;
    auto below_plan = make_plan({.max_bloom_bytes_per_gpu = exact_cap - 1}, 0, false, kInt64, 2);
    partition_dynamic_filter_bank below_bank{below_plan, &below_stats};
    REQUIRE_FALSE(below_bank.arm(kBuildRows, kPartitions, kActiveGpus));
    REQUIRE(below_bank.current_state() == partition_dynamic_filter_bank::state::disabled);
    REQUIRE(below_stats.snapshot().partition_dynamic_filter_budget_skips == 2);
  }

  SECTION("global domain coverage")
  {
    dynamic_filter_stats stats;
    auto plan = make_plan({}, 4, true);
    partition_dynamic_filter_bank bank{plan, &stats};

    REQUIRE_FALSE(bank.arm(4, 2, 1));
    REQUIRE(bank.current_state() == partition_dynamic_filter_bank::state::disabled);
    auto const snapshot = stats.snapshot();
    REQUIRE(snapshot.partition_dynamic_filter_keys_considered == 1);
    REQUIRE(snapshot.partition_dynamic_filter_keys_with_known_domain == 1);
    REQUIRE(snapshot.partition_dynamic_filter_keys_skipped_domain_gate == 1);
    REQUIRE(snapshot.partition_dynamic_filter_budget_skips == 0);
  }

  SECTION("build and probe storage types must match exactly")
  {
    dynamic_filter_stats stats;
    auto plan = make_plan({}, 0, false, kInt32);
    partition_dynamic_filter_bank bank{plan, &stats};

    REQUIRE_FALSE(bank.arm(4, 2, 1));
    REQUIRE(bank.current_state() == partition_dynamic_filter_bank::state::disabled);
    REQUIRE(stats.snapshot().partition_dynamic_filter_keys_considered == 0);
  }
}

TEST_CASE("abandoned or empty partitions never expose incomplete filters",
          "[dynamic_filter][partition_dynamic_filter_bank][failure]")
{
  bank_fixture fixture;
  auto& space       = fixture.gpu_space();
  auto const stream = space.acquire_stream();
  auto plan         = make_plan();
  partition_dynamic_filter_bank bank{plan, &fixture.stats};
  REQUIRE(bank.arm(4, 2, 1));

  auto discarded_fragment = make_sequence(space, stream, 2, 0);
  auto complete_partition = make_sequence(space, stream, 2, 100);
  bank.contribute(0, one_column_view(*discarded_fragment), kDeviceId, space, stream);
  bank.abandon_partition(0, "later build fragment unavailable");
  bank.contribute(1, one_column_view(*complete_partition), kDeviceId, space, stream);
  bank.seal();

  REQUIRE(bank.select(0, kDeviceId).result ==
          partition_dynamic_filter_bank::selection::status::failed);
  REQUIRE(bank.select(1, kDeviceId).result ==
          partition_dynamic_filter_bank::selection::status::available);
  auto const snapshot = fixture.stats.snapshot();
  REQUIRE(snapshot.partition_dynamic_filter_failures == 1);
  REQUIRE(snapshot.partition_dynamic_filter_partitions_built == 1);

  dynamic_filter_stats empty_stats;
  auto empty_plan = make_plan();
  partition_dynamic_filter_bank empty_bank{empty_plan, &empty_stats};
  REQUIRE(empty_bank.arm(1, 2, 1));
  auto empty = make_empty_int64(space, stream);
  empty_bank.contribute(0, one_column_view(*empty), kDeviceId, space, stream);
  empty_bank.seal();
  REQUIRE(empty_bank.select(0, kDeviceId).result ==
          partition_dynamic_filter_bank::selection::status::missing);
}

TEST_CASE("pre-arm partition abandonment releases probes fail open",
          "[dynamic_filter][partition_dynamic_filter_bank][failure]")
{
  dynamic_filter_stats stats;
  auto plan = make_plan();
  partition_dynamic_filter_bank bank{plan, &stats};

  bank.abandon_partition(0, "build sizing failed");
  REQUIRE(bank.current_state() == partition_dynamic_filter_bank::state::failed);
  REQUIRE_FALSE(bank.may_apply_to_probe());
  REQUIRE(bank.probe_may_proceed());
  REQUIRE_FALSE(bank.arm(4, 2, 1));
  REQUIRE(bank.select(0, kDeviceId).result ==
          partition_dynamic_filter_bank::selection::status::failed);
  REQUIRE(stats.snapshot().partition_dynamic_filter_failures == 1);
}

TEST_CASE("partition Bloom bank serializes same-partition contributions without blocking others",
          "[dynamic_filter][partition_dynamic_filter_bank][concurrency]")
{
  bank_fixture fixture;
  auto& space              = fixture.gpu_space();
  auto const first_stream  = space.acquire_stream();
  auto const second_stream = space.acquire_stream();
  auto const other_stream  = space.acquire_stream();
  auto plan                = make_plan();
  partition_dynamic_filter_bank bank{plan, &fixture.stats};
  REQUIRE(bank.arm(9, 2, 1));
  std::barrier start{3};

  auto first_fragment  = make_sequence(space, first_stream, 3, 0);
  auto second_fragment = make_sequence(space, second_stream, 3, 3);
  auto other_partition = make_sequence(space, other_stream, 3, 100);
  first_stream.synchronize();
  second_stream.synchronize();
  other_stream.synchronize();

  auto contribute =
    [&](std::size_t partition_idx, cudf::column const& column, rmm::cuda_stream_view stream) {
      rmm::cuda_set_device_raii device{rmm::cuda_device_id{kDeviceId}};
      start.arrive_and_wait();
      bank.contribute(partition_idx, one_column_view(column), kDeviceId, space, stream);
    };
  auto first =
    std::async(std::launch::async, [&] { contribute(0, *first_fragment, first_stream); });
  auto second =
    std::async(std::launch::async, [&] { contribute(0, *second_fragment, second_stream); });
  auto other =
    std::async(std::launch::async, [&] { contribute(1, *other_partition, other_stream); });
  first.get();
  second.get();
  other.get();
  bank.seal();

  auto const selected_zero = bank.select(0, kDeviceId);
  auto const selected_one  = bank.select(1, kDeviceId);
  REQUIRE(selected_zero.result == partition_dynamic_filter_bank::selection::status::available);
  REQUIRE(selected_one.result == partition_dynamic_filter_bank::selection::status::available);
  auto const probe_stream = space.acquire_stream();
  auto zero_probe         = make_sequence(space, probe_stream, 6, 0);
  auto one_probe          = make_sequence(space, probe_stream, 3, 100);
  REQUIRE(membership_mask(*selected_zero.filters, zero_probe->view(), space, probe_stream) ==
          std::vector<std::uint8_t>(6, 1));
  REQUIRE(membership_mask(*selected_one.filters, one_probe->view(), space, probe_stream) ==
          std::vector<std::uint8_t>(3, 1));
  REQUIRE(fixture.stats.snapshot().partition_dynamic_filter_build_fragments == 3);
  REQUIRE(fixture.stats.snapshot().partition_dynamic_filter_build_rows == 9);
}
