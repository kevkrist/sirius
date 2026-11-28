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
#include "op/dynamic_filter/dynamic_filter_replica_reservation.hpp"
#include "utils/cuda_status_fault.hpp"

#include <cucascade/error.hpp>
#include <cucascade/memory/fixed_size_host_memory_resource.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace {
namespace acc = sirius::test::accumulation;
using fault   = sirius::test::scoped_cuda_status_fault;

bool has_two_gpus()
{
  int count = 0;
  REQUIRE(cudaGetDeviceCount(&count) == cudaSuccess);
  if (count < 2) {
    WARN("accumulation publication test needs 2 GPUs, " << count << " visible; skipping");
    return false;
  }
  return true;
}

struct failure_point {
  fault::operation operation;
  std::size_t skip;
  bool staged;  // Every GPU in an island of its own: the publication crosses the host
};

/// The islands a failure point publishes with among `devices` GPUs.
std::span<std::uint8_t const> islands_of(failure_point const& point, std::size_t devices)
{
  return point.staged ? std::span{acc::separate_gpus}.first(devices)
                      : std::span<std::uint8_t const>{};
}
}  // namespace

TEST_CASE("accumulated publication faults drain all registered streams and retain the CUDA cause",
          "[dynamic_filter][accumulation_fault][multi_gpu][cuda_status_fault]")
{
  if (!has_two_gpus()) { return; }
  auto const point = GENERATE(failure_point{fault::copy_peer, 1, false},
                              failure_point{fault::copy_peer, 4, false},
                              failure_point{fault::copy_peer, 6, false},
                              failure_point{fault::copy_peer, 11, false},
                              failure_point{fault::record_event, 4, false},
                              failure_point{fault::synchronize_stream_unjoined, 0, false},
                              failure_point{fault::enqueue_copy_batch, 1, true},
                              failure_point{fault::enqueue_copy_batch, 3, true},
                              failure_point{fault::enqueue_copy_batch, 12, true},
                              failure_point{fault::enqueue_copy_batch, 23, true},
                              failure_point{fault::wait_event, 2, true},
                              failure_point{fault::synchronize_stream_unjoined, 0, true});
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
  auto const start       = std::chrono::steady_clock::now();
  {
    fault injection{
      point.operation, cudaErrorInvalidValue, point.skip, nullptr, std::chrono::milliseconds{80}};
    try {
      (void)acc::builder_access::finish(builder,
                                        rmm::cuda_device_id{0},
                                        fixture.publication_stream(0),
                                        fixture.gpu(0).get_default_allocator(),
                                        {.islands = islands_of(point, 2)});
      FAIL("the publication CUDA failure must escape");
    } catch (sirius::op::detail::accumulation_cuda_error const& error) {
      REQUIRE(error.code() == cudaErrorInvalidValue);
      REQUIRE_FALSE(error.transient_launch_failure());
    }
    REQUIRE(injection.consumed());
    REQUIRE(injection.observed_stream_count() > 0);
    REQUIRE_FALSE(injection.invalid_join());
    REQUIRE(injection.all_submitted_streams_joined());
  }
  REQUIRE(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds{70});
  REQUIRE(host->get_total_allocated_bytes() == host_before);
  REQUIRE(fixture.allocated_bytes() == before);
  for (int device = 0; device < 2; ++device) {
    auto const partial = builder.inspect_partial(0, rmm::cuda_device_id{device});
    REQUIRE(partial);
    REQUIRE(partial->stream_idle);
  }
}

TEST_CASE("accumulation preparation event failure preserves partials and retires scratch",
          "[dynamic_filter][accumulation_fault][multi_gpu][cuda_status_fault]")
{
  if (!has_two_gpus()) { return; }
  auto const skip = GENERATE(std::size_t{0}, std::size_t{7});
  acc::fixture fixture({.devices = 2});
  if (!acc::has_pool_access(fixture.spaces)) { return; }
  auto builder = acc::make_builder(fixture, acc::bloom_geometry(8000));
  auto batch   = fixture.make_batch(1, 0, 100);
  acc::add_to_builder(builder, fixture, batch);
  auto const before   = fixture.allocated_bytes();
  auto const expected = acc::partial_union(builder, 2, 2);
  {
    // The transfer creates eight events here; seven prior ones place the late failure at the last.
    fault injection{fault::create_event, cudaErrorInvalidValue, skip};
    REQUIRE_THROWS_AS(builder.finish(rmm::cuda_device_id{0},
                                     fixture.publication_stream(0),
                                     fixture.gpu(0).get_default_allocator()),
                      cucascade::cuda_error);
    REQUIRE(injection.consumed());
    REQUIRE(injection.observed_stream_count() == 0);
    REQUIRE_FALSE(injection.invalid_join());
    REQUIRE(injection.all_submitted_streams_joined());
  }
  REQUIRE(fixture.allocated_bytes() == before);
  REQUIRE(acc::partial_union(builder, 2, 2) == expected);
}

TEST_CASE(
  "delayed accumulation ingress merge and egress preserve every bitmap byte across halves and keys",
  "[dynamic_filter][accumulation_fault][multi_gpu][cuda_status_fault]")
{
  if (!has_two_gpus()) { return; }
  auto const point = GENERATE(failure_point{fault::copy_peer, 0, false},
                              failure_point{fault::copy_peer, 1, false},
                              failure_point{fault::copy_peer, 4, false},
                              failure_point{fault::copy_peer, 6, false},
                              failure_point{fault::record_event, 2, false},
                              failure_point{fault::enqueue_copy_batch, 1, true},
                              failure_point{fault::enqueue_copy_batch, 3, true});
  CAPTURE(point.operation, point.skip, point.staged);
  acc::fixture fixture({.devices = 2});
  if (!point.staged && !acc::has_pool_access(fixture.spaces)) { return; }
  auto builder = acc::make_builder(fixture, acc::bloom_geometry(2'097'153));
  auto first   = fixture.make_batch(0, 0, 100);
  auto second  = fixture.make_batch(1, 1000, 100);
  acc::add_to_builder(builder, fixture, first);
  acc::add_to_builder(builder, fixture, second);
  auto const expected = acc::partial_union(builder, 2, 2);
  std::optional<sirius::op::detail::accumulated_bloom_builder::publication> published;
  {
    fault injection{
      point.operation, cudaSuccess, point.skip, nullptr, std::chrono::milliseconds{80}};
    published = acc::builder_access::finish(builder,
                                            rmm::cuda_device_id{0},
                                            fixture.publication_stream(0),
                                            fixture.gpu(0).get_default_allocator(),
                                            {.islands = islands_of(point, 2)});
    REQUIRE(injection.consumed());
    REQUIRE_FALSE(injection.invalid_join());
  }
  REQUIRE(published);
  using route = sirius::op::detail::accumulated_bloom_builder::publication_route;
  REQUIRE(published->route == (point.staged ? route::HOST : route::PEER));
  REQUIRE_FALSE(published->degraded);
  acc::require_exact_union(published->filters, expected, 2);
}

TEST_CASE("a held upload does not stop the other leaders' uploads or the next downloads",
          "[dynamic_filter][accumulation_fault][multi_gpu][cuda_status_fault]")
{
  if (auto const visible = acc::visible_gpus(); visible < 3) {
    WARN("upload overlap test needs 3 GPUs, " << visible << " visible; skipping");
    return;
  }
  acc::fixture fixture({.devices = 3});
  // Three chunks per key: later chunks reuse the window slots of the held upload's chunk.
  auto const geometry = acc::bloom_geometry(2'097'153);
  REQUIRE(geometry.chunks_per_key() == 3);
  auto builder = acc::make_builder(fixture, geometry);
  std::vector<acc::batch_ptr> batches;
  for (int device = 0; device < 3; ++device) {
    batches.push_back(fixture.make_batch(device, std::int64_t{1000} * device, 100));
    acc::add_to_builder(builder, fixture, batches.back());
  }
  auto const expected = acc::partial_union(builder, 2, 3);
  auto* host          = fixture.spaces[0]
                 .get_host_staging_space()
                 .get_memory_resource_of<cucascade::memory::Tier::HOST>();
  REQUIRE(host);
  auto const host_before = host->get_total_allocated_bytes();
  auto const baseline    = fixture.allocated_bytes();
  sirius::op::detail::union_report workspace;
  std::optional<sirius::op::detail::accumulated_bloom_builder::publication> published;
  {
    // Three inputs download the first two chunks (six batches) before the first host OR, so the
    // delay holds the seventh batch: the first leader's upload of the first chunk.
    fault injection{
      fault::enqueue_copy_batch, cudaSuccess, 6, nullptr, std::chrono::milliseconds{200}};
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{0}};
    published = acc::builder_access::finish(
      builder,
      rmm::cuda_device_id{0},
      fixture.publication_stream(0),
      fixture.gpu(0).get_default_allocator(),
      {.islands = std::span{acc::separate_gpus}.first(3), .report = &workspace});
    REQUIRE(injection.consumed());
    REQUIRE_FALSE(injection.invalid_join());
    // While it was held: the other two leaders' uploads of that chunk and the three downloads of
    // the third chunk.
    REQUIRE(injection.submissions_during_delay() >= 5);
  }
  REQUIRE(published);
  using route = sirius::op::detail::accumulated_bloom_builder::publication_route;
  REQUIRE(published->route == route::HOST);
  acc::require_exact_union(published->filters, expected, 3);
  auto const block = host->get_block_size();
  // Two chunks in flight, each with three input slots and one output slot.
  REQUIRE(workspace.window_chunks == 2);
  REQUIRE(workspace.host_inputs == 3);
  REQUIRE(workspace.host_window_bytes ==
          2 * 4 * ((geometry.chunk_bytes + block - 1) / block) * block);
  REQUIRE(std::ranges::all_of(workspace.device_scratch_bytes,
                              [](std::size_t bytes) { return bytes == 0; }));
  REQUIRE(host->get_total_allocated_bytes() == host_before);
  REQUIRE(fixture.allocated_bytes() == baseline);
}

TEST_CASE(
  "a HOST pool with room for one chunk in flight publishes; with less it refuses and "
  "changes no partial",
  "[dynamic_filter][construction][accumulation_fault][multi_gpu][cuda_status_fault]")
{
  if (!has_two_gpus()) { return; }
  bool const one_chunk_fits = GENERATE(true, false);
  CAPTURE(one_chunk_fits);
  acc::fixture fixture({.devices = 2});
  auto const geometry = acc::bloom_geometry(8000);
  auto builder        = acc::make_builder(fixture, geometry);
  auto batch          = fixture.make_batch(1, 0, 100);
  acc::add_to_builder(builder, fixture, batch);
  auto const expected = acc::partial_union(builder, 2, 2);
  std::array<std::array<std::vector<std::byte>, 2>, 2> before;
  for (std::size_t key = 0; key < 2; ++key) {
    for (int device = 0; device < 2; ++device) {
      before[key][device] = builder.inspect_partial(key, rmm::cuda_device_id{device})->bytes;
    }
  }
  auto* host = fixture.spaces[0]
                 .get_host_staging_space()
                 .get_memory_resource_of<cucascade::memory::Tier::HOST>();
  auto const block = host->get_block_size();
  // One contributing GPU is passed through: one slot per chunk in flight, two chunks in all.
  auto const slot = ((geometry.chunk_bytes + block - 1) / block) * block;
  auto blocker    = host->reserve_upto(host->get_available_memory());
  REQUIRE(blocker);
  auto const keep        = one_chunk_fits ? slot : slot - block;
  auto remaining         = host->allocate_multiple_blocks(host->get_available_memory() - keep);
  auto const baseline    = fixture.allocated_bytes();
  auto const host_before = host->get_total_allocated_bytes();
  sirius::op::detail::union_report workspace;
  std::optional<sirius::op::detail::accumulated_bloom_builder::publication> result;
  {
    fault injection{fault::enqueue_copy_batch, cudaErrorInvalidValue, 1000};
    result = acc::builder_access::finish(
      builder,
      rmm::cuda_device_id{0},
      fixture.publication_stream(0),
      fixture.gpu(0).get_default_allocator(),
      {.islands = std::span{acc::separate_gpus}.first(2), .report = &workspace});
    REQUIRE_FALSE(injection.consumed());
    REQUIRE_FALSE(injection.invalid_join());
    if (!one_chunk_fits) { REQUIRE(injection.observed_stream_count() == 0); }
  }
  REQUIRE(host->get_total_allocated_bytes() == host_before);
  REQUIRE(fixture.allocated_bytes() == baseline);
  REQUIRE(cudaStreamQuery(fixture.publication_stream(0).get()) == cudaSuccess);
  if (one_chunk_fits) {
    REQUIRE(result);
    REQUIRE(result->degraded);
    REQUIRE(workspace.window_chunks == 1);
    REQUIRE(workspace.host_window_bytes == slot);
    acc::require_exact_union(result->filters, expected, 2);
    return;
  }
  REQUIRE_FALSE(result);
  REQUIRE(workspace.host_window_bytes == 0);
  for (std::size_t key = 0; key < 2; ++key) {
    for (int device = 0; device < 2; ++device) {
      auto const partial = builder.inspect_partial(key, rmm::cuda_device_id{device});
      REQUIRE(partial);
      REQUIRE(partial->stream_idle);
      REQUIRE(partial->bytes == before[key][device]);
    }
  }
}

namespace {
/// A failure of one transfer stage on a machine whose GPUs form several islands.
struct island_failure {
  fault::operation operation;
  std::size_t skip;
  char const* stage;
};
}  // namespace

TEST_CASE("a fault in any stage of a publication over several islands is joined and cleaned up",
          "[dynamic_filter][accumulation_fault][multi_gpu][cuda_status_fault]")
{
  auto const devices = acc::visible_gpus();
  if (devices < 3) {
    WARN("island fault test needs 3 GPUs, " << devices << " visible; skipping");
    return;
  }
  // The submission order is: merges of two chunks, downloads of the first chunk, then per chunk
  // the next download and merge, the host OR, the uploads and the fan-out.
  auto const point = GENERATE(island_failure{fault::copy_peer, 0, "root ingress"},
                              island_failure{fault::copy_peer, 1, "second ingress or fan-out"},
                              island_failure{fault::copy_peer, 9, "fan-out"},
                              island_failure{fault::wait_event, 0, "source ready"},
                              island_failure{fault::wait_event, 7, "merge"},
                              island_failure{fault::wait_event, 25, "later chunk"},
                              island_failure{fault::record_event, 0, "source ready"},
                              island_failure{fault::record_event, 9, "merged"},
                              island_failure{fault::record_event, 20, "later chunk"},
                              island_failure{fault::enqueue_copy_batch, 0, "download"},
                              island_failure{fault::enqueue_copy_batch, 4, "upload"},
                              island_failure{fault::enqueue_copy_batch, 9, "later chunk"},
                              island_failure{fault::synchronize_event, 0, "host OR, first chunk"},
                              island_failure{fault::synchronize_event, 5, "host OR, later chunk"},
                              island_failure{fault::synchronize_stream_unjoined, 0, "final wait"});
  CAPTURE(devices, point.operation, point.skip, point.stage);
  acc::fixture fixture({.devices = static_cast<std::size_t>(std::min(devices, 4))});
  auto const count = fixture.spaces.size();
  if (acc::pool_access_available(fixture.spaces)) {
    WARN("every fixture GPU reaches every other: no publication crosses the host; skipping");
    return;
  }
  auto builder = acc::make_builder(fixture, acc::bloom_geometry(2'097'153));
  std::vector<acc::batch_ptr> batches;
  for (std::size_t device = 0; device < count; ++device) {
    batches.push_back(
      fixture.make_batch(static_cast<int>(device), static_cast<std::int64_t>(1000 * device), 100));
    acc::add_to_builder(builder, fixture, batches.back());
  }
  auto const before = fixture.allocated_bytes();
  auto* host        = fixture.spaces[0]
                 .get_host_staging_space()
                 .get_memory_resource_of<cucascade::memory::Tier::HOST>();
  auto const host_before = host->get_total_allocated_bytes();
  {
    fault injection{point.operation, cudaErrorInvalidValue, point.skip};
    try {
      (void)builder.finish(rmm::cuda_device_id{0},
                           fixture.publication_stream(0),
                           fixture.gpu(0).get_default_allocator());
      FAIL("the publication CUDA failure must escape");
    } catch (sirius::op::detail::accumulation_cuda_error const& error) {
      REQUIRE(error.code() == cudaErrorInvalidValue);
    }
    REQUIRE(injection.consumed());
    REQUIRE_FALSE(injection.invalid_join());
    REQUIRE(injection.all_submitted_streams_joined());
  }
  REQUIRE(host->get_total_allocated_bytes() == host_before);
  REQUIRE(fixture.allocated_bytes() == before);
  for (std::size_t device = 0; device < count; ++device) {
    auto const partial = builder.inspect_partial(0, rmm::cuda_device_id{static_cast<int>(device)});
    REQUIRE(partial);
    REQUIRE(partial->stream_idle);
  }
}

TEST_CASE("a publication that moves nothing submits no copy and allocates nothing",
          "[dynamic_filter][accumulation_fault][multi_gpu][cuda_status_fault]")
{
  auto const devices = GENERATE(std::size_t{1}, std::size_t{2});
  if (static_cast<std::size_t>(acc::visible_gpus()) < devices) { return; }
  // Any copy of the armed kind would consume the fault and fail the publication.
  auto const copy        = GENERATE(fault::copy_peer, fault::enqueue_copy_batch);
  bool const contributes = devices == 1;
  CAPTURE(devices, contributes, copy);
  acc::fixture fixture({.devices = devices});
  auto builder = acc::make_builder(fixture, acc::bloom_geometry(8000));
  acc::batch_ptr batch;
  if (contributes) {
    batch = fixture.make_batch(0, 0, 100);
    acc::add_to_builder(builder, fixture, batch);
  }
  auto const expected = acc::partial_union(builder, 2, devices);
  auto const before   = fixture.allocated_bytes();
  sirius::op::detail::union_report workspace;
  std::optional<sirius::op::detail::accumulated_bloom_builder::publication> published;
  {
    fault injection{copy, cudaErrorInvalidValue};
    published = acc::builder_access::finish(builder,
                                            rmm::cuda_device_id{0},
                                            fixture.publication_stream(0),
                                            fixture.gpu(0).get_default_allocator(),
                                            {.report = &workspace});
    REQUIRE_FALSE(injection.consumed());
  }
  REQUIRE(published);
  using route = sirius::op::detail::accumulated_bloom_builder::publication_route;
  REQUIRE(published->route == route::NONE);
  REQUIRE(workspace.host_window_bytes == 0);
  REQUIRE(std::ranges::all_of(workspace.device_scratch_bytes,
                              [](std::size_t bytes) { return bytes == 0; }));
  REQUIRE(fixture.allocated_bytes() == before);
  acc::require_exact_union(published->filters, expected, devices);
}
