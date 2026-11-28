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
#include "memory/peer_islands.hpp"
#include "op/dynamic_filter/detail/accumulated_bloom_builder.hpp"
#include "op/dynamic_filter/detail/union_plan.hpp"
#include "op/dynamic_filter/detail/union_transfer.hpp"
#include "op/dynamic_filter/dynamic_filter_replica_reservation.hpp"

#include <rmm/cuda_device.hpp>
#include <rmm/cuda_stream.hpp>

#include <cuda/utility>

#include <catch.hpp>
#include <cucascade/memory/fixed_size_host_memory_resource.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace acc      = sirius::test::accumulation;
namespace op       = sirius::op;
using builder_type = op::detail::accumulated_bloom_builder;
using route        = builder_type::publication_route;

/// The islands of the fixture's GPUs as the builder finds them.
std::vector<std::uint8_t> real_islands(acc::fixture const& fixture)
{
  std::vector<cucascade::memory::memory_space const*> spaces;
  for (auto const& space : fixture.spaces) {
    spaces.push_back(&space.get_gpu_space());
  }
  return sirius::memory::peer_islands(spaces);
}

/// `islands` with every member of the islands selected by `divide` moved to an island of its own.
template <class Divide>
std::vector<std::uint8_t> divided(std::vector<std::uint8_t> islands, Divide&& divide)
{
  auto next = static_cast<std::uint8_t>(*std::ranges::max_element(islands) + 1);
  for (auto& island : islands) {
    if (divide(island)) { island = next++; }
  }
  return islands;
}

/// The groupings one fixture is published with: the real one and three divisions of it.
std::vector<std::vector<std::uint8_t>> topologies(acc::fixture const& fixture, std::size_t root)
{
  auto const real = real_islands(fixture);
  auto const home = real[root];
  std::vector<std::vector<std::uint8_t>> result{
    real,
    divided(real, [](std::uint8_t) { return true; }),
    divided(real, [&](std::uint8_t island) { return island == home; }),
    divided(real, [&](std::uint8_t island) { return island != home; })};
  std::ranges::sort(result);
  result.erase(std::ranges::unique(result).begin(), result.end());
  return result;
}

std::string describe(std::span<std::uint8_t const> islands)
{
  std::string text;
  for (auto const island : islands) {
    text += std::to_string(island);
  }
  return text;
}

/// A builder over arrays of `blocks[i]` Bloom blocks, with the keys of the fixture's batches.
builder_type make_builder(acc::fixture const& fixture, std::span<std::size_t const> blocks)
{
  std::vector<builder_type::key> keys{{0, cudf::data_type{cudf::type_id::INT32}, blocks[0]}};
  if (blocks.size() == 2) { keys.push_back({1, cudf::data_type{cudf::type_id::INT64}, blocks[1]}); }
  auto result = builder_type::try_create(std::move(keys), fixture.spaces);
  REQUIRE(result);
  return std::move(*result);
}

/// What `unite` must report for a publication, from the plan's roles and the array shapes.
struct expected_workspace {
  std::vector<std::size_t> device_scratch_bytes;
  std::size_t host_window_bytes;
  route how;
};

expected_workspace expect_workspace(std::span<std::uint8_t const> islands,
                                    std::span<bool const> contributed,
                                    std::size_t root,
                                    std::span<std::size_t const> blocks,
                                    std::size_t host_block)
{
  std::size_t chunks = 0;
  std::size_t slot   = 0;
  for (auto const count : blocks) {
    auto const bytes = count * op::bloom_bytes_per_block;
    auto const chunk = op::detail::accumulated_bloom_chunk_bytes(bytes);
    chunks += ::cuda::ceil_div(bytes, chunk);
    slot = std::max(slot, chunk);
  }
  auto const plan      = op::detail::union_plan::make(islands, contributed, root);
  auto const in_flight = std::min<std::size_t>(2, chunks);
  expected_workspace expected{std::vector<std::size_t>(islands.size(), 0), 0, route::NONE};
  if (plan.moves_nothing()) { return expected; }
  bool peer = false;
  for (auto const& island : plan.islands) {
    peer                                         = peer || !island.others.empty();
    expected.device_scratch_bytes[island.leader] = op::detail::tracked_replica_allocation_bytes(
      in_flight * op::detail::union_plan::merge_slots(island) * slot);
  }
  expected.host_window_bytes =
    in_flight * plan.host_slots() * ::cuda::ceil_div(slot, host_block) * host_block;
  expected.how = !plan.crosses_host() ? route::PEER : peer ? route::PEER_AND_HOST : route::HOST;
  return expected;
}

/// Adds one batch of distinct rows on every GPU whose bit is set in `mask`.
std::vector<acc::batch_ptr> contribute(builder_type& builder,
                                       acc::fixture const& fixture,
                                       unsigned mask)
{
  std::vector<acc::batch_ptr> batches;
  for (std::size_t device = 0; device < fixture.spaces.size(); ++device) {
    if ((mask >> device & 1U) == 0) { continue; }
    batches.push_back(
      fixture.make_batch(static_cast<int>(device), static_cast<std::int64_t>(1000 * device), 100));
    acc::add_to_builder(builder, fixture, batches.back());
  }
  return batches;
}

/// Publishes one builder under one grouping and checks the union, the route and the workspace.
void publish_and_check(acc::fixture const& fixture,
                       std::span<std::size_t const> blocks,
                       std::span<std::uint8_t const> islands,
                       unsigned mask,
                       std::size_t root,
                       bool inline_or)
{
  auto const devices = fixture.spaces.size();
  CAPTURE(devices, describe(islands), mask, root, inline_or, blocks.size(), blocks[0]);
  auto builder        = make_builder(fixture, blocks);
  auto const batches  = contribute(builder, fixture, mask);
  auto const expected = acc::partial_union(builder, blocks.size(), devices);
  auto* host          = fixture.spaces[root]
                 .get_host_staging_space()
                 .get_memory_resource_of<cucascade::memory::Tier::HOST>();
  REQUIRE(host);
  auto const host_before = host->get_total_allocated_bytes();
  auto const baseline    = fixture.allocated_bytes();
  auto const contributed = std::make_unique<bool[]>(devices);
  for (std::size_t device = 0; device < devices; ++device) {
    contributed[device] = (mask >> device & 1U) != 0;
  }
  auto const workspace =
    expect_workspace(islands, {contributed.get(), devices}, root, blocks, host->get_block_size());
  op::detail::union_report report;
  std::optional<builder_type::publication> published;
  {
    auto const device = static_cast<int>(root);
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{device}};
    published =
      acc::builder_access::finish(builder,
                                  rmm::cuda_device_id{device},
                                  fixture.publication_stream(device),
                                  fixture.gpu(device).get_default_allocator(),
                                  {.islands = islands, .inline_or = inline_or, .report = &report});
  }
  REQUIRE(published);
  REQUIRE_FALSE(published->degraded);
  REQUIRE(published->route == workspace.how);
  REQUIRE(report.device_scratch_bytes == workspace.device_scratch_bytes);
  REQUIRE(report.host_window_bytes == workspace.host_window_bytes);
  REQUIRE(host->get_total_allocated_bytes() == host_before);
  REQUIRE(fixture.allocated_bytes() == baseline);
  acc::require_exact_union(published->filters, expected, devices);
}

bool lacks_gpus(std::size_t devices)
{
  if (static_cast<std::size_t>(acc::visible_gpus()) >= devices) { return false; }
  WARN("test needs " << devices << " GPUs; skipping");
  return true;
}
}  // namespace

TEST_CASE(
  "every replica equals the union of the partials for every grouping, contribution and root",
  "[dynamic_filter][construction][union_transfer][accumulation_workspace][multi_gpu]")
{
  auto const devices = GENERATE(std::size_t{2}, std::size_t{3}, std::size_t{4});
  if (lacks_gpus(devices)) { return; }
  acc::fixture fixture({.devices = devices});
  // Three chunks per key, the last one short: both scratch buffers and both window slots are
  // reused.
  auto const geometry = acc::bloom_geometry(2'097'153);
  REQUIRE(geometry.chunks_per_key() == 3);
  for (std::size_t keys = 1; keys <= 2; ++keys) {
    std::vector<std::size_t> const blocks(keys, geometry.blocks);
    for (std::size_t root = 0; root < devices; ++root) {
      for (auto const& islands : topologies(fixture, root)) {
        for (unsigned mask = 0; mask < (1U << devices); ++mask) {
          // The inline executor differs only where the host ORs: with several contributing islands.
          publish_and_check(fixture, blocks, islands, mask, root, false);
          if (keys == 2) { publish_and_check(fixture, blocks, islands, mask, root, true); }
        }
      }
    }
  }
}

TEST_CASE("the union is exact for arrays from one chunk to many, of different sizes per key",
          "[dynamic_filter][construction][union_transfer][multi_gpu]")
{
  auto const devices = std::min<std::size_t>(4, static_cast<std::size_t>(acc::visible_gpus()));
  if (lacks_gpus(2)) { return; }
  acc::fixture fixture({.devices = devices});
  // Bloom blocks of 32 bytes: 96 B; one short chunk; 2 MiB + 32 B (two chunks, the second 32 B);
  // 8 MiB + 32 B (five chunks); 70 MiB + 96 B (nine 8 MiB chunks with a short tail).
  std::vector<std::vector<std::size_t>> const shapes{
    {3}, {3, 1000}, {65537, 3}, {262145, 65537}, {2'293'763, 1000}};
  unsigned const all = (1U << devices) - 1;
  for (auto const& blocks : shapes) {
    for (auto const root : {std::size_t{0}, devices - 1}) {
      for (auto const& islands : topologies(fixture, root)) {
        for (auto const mask : {all, 1U << root, all & ~(1U << root), 1U << (devices - 1 - root)}) {
          publish_and_check(fixture, blocks, islands, mask, root, blocks.front() % 2 == 0);
        }
      }
    }
  }
}

TEST_CASE(
  "refused root merge scratch publishes through the host and keeps the other islands' "
  "peer copies",
  "[dynamic_filter][construction][union_transfer][multi_gpu]")
{
  auto const devices = GENERATE(std::size_t{2}, std::size_t{4});
  if (lacks_gpus(devices)) { return; }
  acc::fixture fixture({.devices = devices});
  auto const islands = real_islands(fixture);
  if (islands[0] != islands[1]) {
    WARN("GPUs 0 and 1 do not reach each other; skipping");
    return;
  }
  auto const geometry = acc::bloom_geometry(2'097'153);
  auto builder        = acc::make_builder(fixture, geometry);
  auto const batches  = contribute(builder, fixture, (1U << devices) - 1);
  auto const expected = acc::partial_union(builder, 2, devices);
  auto const baseline = fixture.allocated_bytes();
  // A task reservation that admits nothing beyond what is already allocated on the stream.
  auto const stream = fixture.publication_stream(0);
  auto refusing_task =
    op::detail::scoped_replica_reservation::try_acquire(fixture.gpu(0), 256, stream);
  REQUIRE(refusing_task);
  op::detail::union_report report;
  auto const published = acc::builder_access::finish(
    builder, rmm::cuda_device_id{0}, stream, refusing_task->allocator(), {.report = &report});
  refusing_task.reset();
  REQUIRE(published);
  REQUIRE(published->degraded);
  REQUIRE(report.degraded);
  // GPUs 0 and 1 became islands of their own; a second real island still merges by peer copies.
  bool const other_island_merges = devices == 4 && islands[2] == islands[3];
  REQUIRE(published->route == (other_island_merges ? route::PEER_AND_HOST : route::HOST));
  REQUIRE(report.device_scratch_bytes[0] == 0);
  REQUIRE(report.device_scratch_bytes[1] == 0);
  if (other_island_merges) {
    REQUIRE(report.islands == 3);
    REQUIRE(report.host_inputs == 3);
    REQUIRE(report.device_scratch_bytes[2] + report.device_scratch_bytes[3] ==
            op::detail::tracked_replica_allocation_bytes(acc::merge_scratch_bytes(geometry)));
  }
  REQUIRE(fixture.allocated_bytes() == baseline);
  acc::require_exact_union(published->filters, expected, devices);
}

TEST_CASE("a refused scratch lease on another island's leader moves only that island to the host",
          "[dynamic_filter][construction][union_transfer][multi_gpu]")
{
  if (lacks_gpus(4)) { return; }
  acc::fixture fixture({.devices = 4});
  auto const islands = real_islands(fixture);
  if (islands != std::vector<std::uint8_t>{0, 0, 1, 1}) {
    WARN("test needs the GPU pairs {0,1} and {2,3}; skipping");
    return;
  }
  auto const geometry = acc::bloom_geometry(2'097'153);
  auto builder        = acc::make_builder(fixture, geometry);
  auto const batches  = contribute(builder, fixture, 0b1111);
  auto const expected = acc::partial_union(builder, 2, 4);
  auto const baseline = fixture.allocated_bytes();
  op::detail::union_report report;
  std::optional<builder_type::publication> published;
  {
    // GPU 2 leads the second island; with its space fully reserved its scratch lease is refused.
    auto const hog = fixture.gpu(2).make_reservation_or_null(fixture.reservable(2));
    REQUIRE(hog);
    published = acc::builder_access::finish(builder,
                                            rmm::cuda_device_id{0},
                                            fixture.publication_stream(0),
                                            fixture.gpu(0).get_default_allocator(),
                                            {.report = &report});
  }
  REQUIRE(published);
  REQUIRE(published->degraded);
  REQUIRE(published->route == route::PEER_AND_HOST);
  REQUIRE(report.islands == 3);
  REQUIRE(report.host_inputs == 3);
  REQUIRE(
    report.device_scratch_bytes ==
    std::vector<std::size_t>{
      op::detail::tracked_replica_allocation_bytes(acc::merge_scratch_bytes(geometry)), 0, 0, 0});
  REQUIRE(fixture.allocated_bytes() == baseline);
  acc::require_exact_union(published->filters, expected, 4);
}

TEST_CASE("islands that join GPUs without mutual pool access are rejected before anything moves",
          "[dynamic_filter][construction][union_transfer][multi_gpu]")
{
  auto const devices = std::min<std::size_t>(4, static_cast<std::size_t>(acc::visible_gpus()));
  if (lacks_gpus(2)) { return; }
  acc::fixture fixture({.devices = devices});
  auto const islands = real_islands(fixture);
  if (std::ranges::all_of(islands, [](std::uint8_t island) { return island == 0; })) {
    WARN("every fixture GPU reaches every other; skipping");
    return;
  }
  auto builder        = acc::make_builder(fixture, acc::bloom_geometry(8000));
  auto const batches  = contribute(builder, fixture, (1U << devices) - 1);
  auto const expected = acc::partial_union(builder, 2, devices);
  std::vector<std::uint8_t> const joined(devices, 0);
  auto const finish = [&](std::span<std::uint8_t const> grouping) {
    return acc::builder_access::finish(builder,
                                       rmm::cuda_device_id{0},
                                       fixture.publication_stream(0),
                                       fixture.gpu(0).get_default_allocator(),
                                       {.islands = grouping});
  };
  REQUIRE_THROWS_AS(finish(joined), op::detail::accumulation_invariant_error);
  REQUIRE_THROWS_AS(finish(std::span{joined}.first(devices - 1)),
                    op::detail::accumulation_invariant_error);
  REQUIRE(acc::partial_union(builder, 2, devices) == expected);
  auto const published = finish({});
  REQUIRE(published);
  acc::require_exact_union(published->filters, expected, devices);
}

TEST_CASE("publication reads every partial only after its pending zero fill and inserts",
          "[dynamic_filter][construction][union_transfer][multi_gpu]")
{
  auto const devices = std::min<std::size_t>(4, static_cast<std::size_t>(acc::visible_gpus()));
  if (lacks_gpus(2)) { return; }
  bool const separate = GENERATE(false, true);
  CAPTURE(devices, separate);
  acc::fixture fixture({.devices = devices});
  auto const geometry = acc::bloom_geometry(2'097'153);
  // A twin built without delays gives the expected bytes: inserts are deterministic.
  auto twin              = acc::make_builder(fixture, geometry);
  auto const twin_inputs = contribute(twin, fixture, (1U << devices) - 1);
  auto const expected    = acc::partial_union(twin, 2, devices);

  std::vector<acc::batch_ptr> batches;
  for (std::size_t device = 0; device < devices; ++device) {
    batches.push_back(
      fixture.make_batch(static_cast<int>(device), static_cast<std::int64_t>(1000 * device), 100));
  }
  // Hold every task stream first, so each insert is still pending when publication starts.
  for (std::size_t device = 0; device < devices; ++device) {
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{static_cast<int>(device)}};
    acc::enqueue_delay(fixture.task_stream(static_cast<int>(device)),
                       std::chrono::milliseconds{60});
  }
  auto builder = acc::make_builder(fixture, geometry);
  std::vector<decltype(batches.front()->to_read_only())> sources;
  for (std::size_t device = 0; device < devices; ++device) {
    auto const id = static_cast<int>(device);
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{id}};
    sources.push_back(batches[device]->to_read_only());
    REQUIRE(builder.enqueue_add(sirius::get_cudf_table_view(sources.back()),
                                rmm::cuda_device_id{id},
                                fixture.task_stream(id)));
  }
  auto const published =
    acc::builder_access::finish(builder,
                                rmm::cuda_device_id{0},
                                fixture.publication_stream(0),
                                fixture.gpu(0).get_default_allocator(),
                                {.islands = separate ? std::span{acc::separate_gpus}.first(devices)
                                                     : std::span<std::uint8_t const>{}});
  for (std::size_t device = 0; device < devices; ++device) {
    fixture.task_stream(static_cast<int>(device)).sync();
  }
  REQUIRE(published);
  acc::require_exact_union(published->filters, expected, devices);
}

TEST_CASE("concurrent publications share the host OR workers and stay exact",
          "[dynamic_filter][construction][union_transfer][multi_gpu]")
{
  auto const devices = std::min<std::size_t>(4, static_cast<std::size_t>(acc::visible_gpus()));
  if (lacks_gpus(2)) { return; }
  constexpr std::size_t publications = 8;
  acc::fixture fixture({.devices = devices});
  auto const geometry = acc::bloom_geometry(2'097'153);
  auto* host          = fixture.spaces[0]
                 .get_host_staging_space()
                 .get_memory_resource_of<cucascade::memory::Tier::HOST>();
  auto const host_before = host->get_total_allocated_bytes();
  auto const baseline    = fixture.allocated_bytes();
  {
    std::vector<builder_type> builders;
    std::vector<std::vector<acc::batch_ptr>> inputs;
    std::vector<std::vector<std::vector<std::byte>>> expected;
    for (std::size_t index = 0; index < publications; ++index) {
      builders.push_back(acc::make_builder(fixture, geometry));
      inputs.push_back(contribute(builders.back(), fixture, (1U << devices) - 1));
      expected.push_back(acc::partial_union(builders.back(), 2, devices));
    }
    std::vector<std::optional<builder_type::publication>> published(publications);
    std::vector<std::string> failures(publications);
    {
      std::vector<std::jthread> threads;
      for (std::size_t index = 0; index < publications; ++index) {
        threads.emplace_back([&, index] {
          // Every thread publishes from another root with a stream of its own; half of them with
          // every GPU in its own island, so that the host ORs as many inputs as there are GPUs.
          auto const root = static_cast<int>(index % devices);
          try {
            rmm::cuda_set_device_raii guard{rmm::cuda_device_id{root}};
            rmm::cuda_stream stream{rmm::cuda_stream::flags::non_blocking};
            published[index] = acc::builder_access::finish(
              builders[index],
              rmm::cuda_device_id{root},
              stream,
              fixture.gpu(root).get_default_allocator(),
              {.islands = index % 2 == 0 ? std::span{acc::separate_gpus}.first(devices)
                                         : std::span<std::uint8_t const>{}});
          } catch (std::exception const& error) {
            failures[index] = error.what();
          }
        });
      }
    }
    for (std::size_t index = 0; index < publications; ++index) {
      CAPTURE(index, failures[index]);
      REQUIRE(failures[index].empty());
      REQUIRE(published[index]);
      acc::require_exact_union(published[index]->filters, expected[index], devices);
    }
  }
  REQUIRE(host->get_total_allocated_bytes() == host_before);
  REQUIRE(fixture.allocated_bytes() == baseline);
}

// Not a test: prints the time of `finish` for the array sizes and contributions of the transfer
// benchmarks. Run alone on an idle machine: sirius_unittest "[union_transfer_bench]".
TEST_CASE("time of accumulated publication by array size, contribution and grouping",
          "[.][union_transfer_bench][multi_gpu]")
{
  auto const devices = std::min<std::size_t>(4, static_cast<std::size_t>(acc::visible_gpus()));
  if (lacks_gpus(2)) { return; }
  acc::fixture fixture({.devices = devices, .gpu_bytes = std::size_t{8} << 30});
  unsigned const all = (1U << devices) - 1;
  std::printf("| MiB | contributing | grouping | median ms | ms/GiB |\n|---|---|---|---|---|\n");
  for (auto const megabytes : {4, 16, 57, 174, 281, 1434}) {
    std::vector<std::size_t> const blocks{megabytes * acc::mib / op::bloom_bytes_per_block};
    for (auto const mask : {all, 0b0101U & all, 0b0001U}) {
      for (bool const separate : {false, true}) {
        std::vector<double> times;
        for (int round = 0; round < 6; ++round) {
          auto builder       = make_builder(fixture, blocks);
          auto const batches = contribute(builder, fixture, mask);
          for (std::size_t device = 0; device < devices; ++device) {
            // Publication is timed without the pending zero fill of the arrays.
            REQUIRE(builder.inspect_partial(0, rmm::cuda_device_id{static_cast<int>(device)}));
          }
          auto const started   = std::chrono::steady_clock::now();
          auto const published = acc::builder_access::finish(
            builder,
            rmm::cuda_device_id{0},
            fixture.publication_stream(0),
            fixture.gpu(0).get_default_allocator(),
            {.islands = separate ? std::span{acc::separate_gpus}.first(devices)
                                 : std::span<std::uint8_t const>{}});
          auto const elapsed = std::chrono::steady_clock::now() - started;
          REQUIRE(published);
          // The first round warms up streams, kernels and pinned pages.
          if (round != 0) {
            times.push_back(std::chrono::duration<double, std::milli>(elapsed).count());
          }
        }
        std::ranges::sort(times);
        auto const median = times[times.size() / 2];
        std::printf("| %d | %s | %s | %.2f | %.1f |\n",
                    megabytes,
                    mask == all  ? "all"
                    : mask == 1U ? "root"
                                 : "GPUs 0+2",
                    separate ? "separate" : "real",
                    median,
                    median * 1024 / megabytes);
      }
    }
  }
}
