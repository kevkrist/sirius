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

#include "dynamic_filter_accumulation_test_utils.hpp"
#include "utils/cuda_status_fault.hpp"

#include <cucascade/error.hpp>
#include <cucascade/memory/fixed_size_host_memory_resource.hpp>

#include <chrono>
#include <string_view>

namespace {
namespace acc = sirius::test::accumulation;
using fault   = sirius::test::scoped_publication_fault;

bool has_two_gpus()
{
  int count = 0;
  REQUIRE(cudaGetDeviceCount(&count) == cudaSuccess);
  if (count < 2) {
    WARN("S4 publication test needs 2 GPUs, " << count << " visible; skipping");
    return false;
  }
  return true;
}

struct failure_point {
  fault::operation operation;
  std::size_t skip;
  bool staged;
};
}  // namespace

TEST_CASE("S4 publication faults drain all registered streams and retain the CUDA cause",
          "[dynamic_filter][s4_publication_fault][multi_gpu]")
{
  if (!has_two_gpus()) { return; }
  auto const point = GENERATE(failure_point{fault::peer_copy, 1, false},
                              failure_point{fault::peer_copy, 4, false},
                              failure_point{fault::peer_copy, 6, false},
                              failure_point{fault::peer_copy, 11, false},
                              failure_point{fault::record_event, 4, false},
                              failure_point{fault::synchronize_stream, 0, false},
                              failure_point{fault::batch_copy, 1, true},
                              failure_point{fault::batch_copy, 3, true},
                              failure_point{fault::batch_copy, 12, true},
                              failure_point{fault::synchronize_stream, 5, true});
  CAPTURE(point.operation, point.skip, point.staged);
  acc::fixture fixture({.devices = 2});
  if (!point.staged && !acc::has_pool_access(fixture.spaces)) { return; }
  auto const geometry = acc::bloom_geometry(2'097'153);
  auto builder        = acc::make_builder(fixture, geometry);
  auto first          = fixture.make_batch(0, 0, 100);
  auto second         = fixture.make_batch(1, 1000, 100);
  acc::add_to_builder(builder, fixture, first);
  acc::add_to_builder(builder, fixture, second);
  auto const before = fixture.allocated_bytes();
  auto* host        = fixture.spaces[0]
                 .get_host_staging_space()
                 .get_memory_resource_of<cucascade::memory::Tier::HOST>();
  auto const host_before = host->get_total_allocated_bytes();
  sirius::op::detail::publication_measurement measured;
  auto const start = std::chrono::steady_clock::now();
  {
    fault injection{
      point.operation, point.skip, cudaErrorInvalidValue, std::chrono::milliseconds{80}};
    try {
      (void)builder.finish_for_experiment(
        rmm::cuda_device_id{0},
        fixture.publication_stream(0),
        fixture.gpu(0).get_default_allocator(),
        point.staged ? sirius::op::detail::publication_transport::force_host
                     : sirius::op::detail::publication_transport::require_direct,
        &measured);
      FAIL("the publication CUDA failure must escape");
    } catch (sirius::op::detail::accumulation_cuda_error const& error) {
      REQUIRE(error.code() == cudaErrorInvalidValue);
      REQUIRE_FALSE(error.transient_launch_failure());
    }
    REQUIRE(injection.consumed());
    REQUIRE_FALSE(injection.invalid_join());
    REQUIRE(injection.all_submitted_streams_joined());
  }
  REQUIRE(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds{70});
  REQUIRE(std::string_view{measured.status} == "failed");
  REQUIRE(measured.ready_replica_bytes == 0);
  REQUIRE(measured.submitted_logical_edge_bytes <= measured.planned_logical_edge_bytes);
  REQUIRE(std::string_view{measured.actual_transport} == (point.staged ? "host" : "direct"));
  REQUIRE(host->get_total_allocated_bytes() == host_before);
  REQUIRE(fixture.allocated_bytes() == before);
  for (int device = 0; device < 2; ++device) {
    auto const partial = builder.inspect_partial(0, rmm::cuda_device_id{device});
    REQUIRE(partial);
    REQUIRE(partial->stream_idle);
  }
}

TEST_CASE("S4 preparation event failure preserves partials and retires scratch",
          "[dynamic_filter][s4_publication_fault][multi_gpu]")
{
  if (!has_two_gpus()) { return; }
  auto const skip = GENERATE(std::size_t{0}, std::size_t{6});
  acc::fixture fixture({.devices = 2});
  if (!acc::has_pool_access(fixture.spaces)) { return; }
  auto builder = acc::make_builder(fixture, acc::bloom_geometry(8000));
  auto batch   = fixture.make_batch(1, 0, 100);
  acc::add_to_builder(builder, fixture, batch);
  auto const before   = fixture.allocated_bytes();
  auto const expected = acc::partial_union(builder, 2, 2);
  sirius::op::detail::publication_measurement measured;
  {
    // Six prior events place the late failure at the last target-completion event.
    fault injection{fault::create_event, skip, cudaErrorInvalidValue};
    REQUIRE_THROWS_AS(
      builder.finish_for_experiment(rmm::cuda_device_id{0},
                                    fixture.publication_stream(0),
                                    fixture.gpu(0).get_default_allocator(),
                                    sirius::op::detail::publication_transport::require_direct,
                                    &measured),
      cucascade::cuda_error);
    REQUIRE(injection.consumed());
    REQUIRE_FALSE(injection.invalid_join());
    REQUIRE(injection.all_submitted_streams_joined());
  }
  REQUIRE(measured.submitted_logical_edge_bytes == 0);
  REQUIRE(measured.ready_replica_bytes == 0);
  REQUIRE(fixture.allocated_bytes() == before);
  REQUIRE(acc::partial_union(builder, 2, 2) == expected);
}

TEST_CASE("S4 delayed ingress merge and egress preserve every bitmap byte across halves and keys",
          "[dynamic_filter][s4_publication_fault][multi_gpu]")
{
  if (!has_two_gpus()) { return; }
  auto const point = GENERATE(failure_point{fault::peer_copy, 0, false},
                              failure_point{fault::peer_copy, 1, false},
                              failure_point{fault::peer_copy, 4, false},
                              failure_point{fault::peer_copy, 6, false},
                              failure_point{fault::record_event, 2, false},
                              failure_point{fault::batch_copy, 1, true},
                              failure_point{fault::batch_copy, 3, true});
  CAPTURE(point.operation, point.skip, point.staged);
  acc::fixture fixture({.devices = 2});
  if (!point.staged && !acc::has_pool_access(fixture.spaces)) { return; }
  auto builder = acc::make_builder(fixture, acc::bloom_geometry(2'097'153));
  auto first   = fixture.make_batch(0, 0, 100);
  auto second  = fixture.make_batch(1, 1000, 100);
  acc::add_to_builder(builder, fixture, first);
  acc::add_to_builder(builder, fixture, second);
  auto const expected = acc::partial_union(builder, 2, 2);
  sirius::op::detail::publication_measurement measured;
  std::optional<std::span<std::shared_ptr<sirius::op::sirius_dynamic_bloom_filter> const>> filters;
  {
    fault injection{point.operation, point.skip, cudaSuccess, std::chrono::milliseconds{80}};
    filters = builder.finish_for_experiment(
      rmm::cuda_device_id{0},
      fixture.publication_stream(0),
      fixture.gpu(0).get_default_allocator(),
      point.staged ? sirius::op::detail::publication_transport::force_host
                   : sirius::op::detail::publication_transport::require_direct,
      &measured);
    REQUIRE(injection.consumed());
    REQUIRE_FALSE(injection.invalid_join());
  }
  REQUIRE(filters);
  acc::require_exact_union(*filters, expected, 2);
  REQUIRE(measured.submitted_logical_edge_bytes == measured.planned_logical_edge_bytes);
  REQUIRE(measured.submitted_bytes_complete);
}
