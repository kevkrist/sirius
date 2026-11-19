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
 * PROTOTYPE microbenchmarks of the dynamic-filter construction lifecycle (not for landing as is).
 *
 * Every case drives the production API: the publication session PARTITION uses
 * (`begin` -> `contribute` -> `publish_if_final`) or, where the Bloom array size must be chosen
 * independently of the build's rows, `accumulated_bloom_builder` directly.
 *
 *   MB0  prices of the primitives (host wait, stream, event)
 *   MB2  fixed cost per contribution
 *   MB3  cost of `begin` (lease + zero-fill + streams) against bytes and GPUs
 *   MB4  cost of `finish` against bytes, GPUs, transfer chunk and exact slices
 *   MB5  small build end to end against the one-shot shape (column constructor + replicate)
 *   MB7  zone-map bounds against rows
 *
 * Each row is printed as `ROW,<case>,key=value,...`; times are microseconds, median and minimum of
 * `SIRIUS_BENCH_DF_REPS` repetitions after one unreported warm-up. Sweeps are narrowed with
 * `SIRIUS_BENCH_DF_*` lists (comma separated).
 */

#include "operator/dynamic_filter_accumulation_test_utils.hpp"

#include <cudf/column/column_factories.hpp>
#include <cudf/concatenate.hpp>
#include <cudf/copying.hpp>
#include <cudf/filling.hpp>
#include <cudf/scalar/scalar.hpp>
#include <cudf/table/table.hpp>

#include <rmm/cuda_device.hpp>
#include <rmm/cuda_stream.hpp>
#include <rmm/device_buffer.hpp>

#include <cuda_profiler_api.h>
#include <cuda_runtime_api.h>

#include <catch.hpp>
#include <op/dynamic_filter/bloom_sizing.hpp>
#include <op/dynamic_filter/detail/accumulated_bloom_builder.hpp>
#include <op/dynamic_filter/dynamic_filter_publish_plan.hpp>
#include <op/dynamic_filter/sirius_dynamic_filter.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace op  = sirius::op;
namespace acc = sirius::test::accumulation;
using clock_type = std::chrono::steady_clock;

[[nodiscard]] double us_between(clock_type::time_point from, clock_type::time_point to)
{
  return std::chrono::duration<double, std::micro>(to - from).count();
}

[[nodiscard]] std::vector<std::uint64_t> env_list(char const* name,
                                                  std::vector<std::uint64_t> fallback)
{
  auto const* text = std::getenv(name);
  if (text == nullptr || *text == '\0') { return fallback; }
  std::vector<std::uint64_t> values;
  std::stringstream stream{text};
  for (std::string item; std::getline(stream, item, ',');) {
    values.push_back(std::strtoull(item.c_str(), nullptr, 10));
  }
  return values;
}

[[nodiscard]] std::vector<std::string> env_names(char const* name,
                                                 std::vector<std::string> fallback)
{
  auto const* text = std::getenv(name);
  if (text == nullptr || *text == '\0') { return fallback; }
  std::vector<std::string> values;
  std::stringstream stream{text};
  for (std::string item; std::getline(stream, item, ',');) {
    values.push_back(item);
  }
  return values;
}

[[nodiscard]] std::size_t reps() { return env_list("SIRIUS_BENCH_DF_REPS", {7}).front(); }

/**
 * @brief Samples of one metric; reports the median and the minimum.
 */
struct samples {
  std::vector<double> values;
  void add(double value) { values.push_back(value); }
  [[nodiscard]] double median() const
  {
    if (values.empty()) { return 0; }
    auto sorted = values;
    std::ranges::sort(sorted);
    return sorted[sorted.size() / 2];
  }
  [[nodiscard]] double minimum() const
  {
    return values.empty() ? 0 : *std::ranges::min_element(values);
  }
};

void sync_devices(std::size_t devices)
{
  for (std::size_t device = 0; device < devices; ++device) {
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{static_cast<int>(device)}};
    REQUIRE(cudaDeviceSynchronize() == cudaSuccess);
  }
}

/**
 * @brief A batch of `columns` INT64 key columns of `rows` distinct values starting at `start`.
 */
[[nodiscard]] acc::batch_ptr make_keys(acc::fixture const& fixture,
                                       int device,
                                       std::int64_t start,
                                       cudf::size_type rows,
                                       std::size_t columns)
{
  auto& space = fixture.gpu(device);
  rmm::cuda_set_device_raii guard{rmm::cuda_device_id{device}};
  auto const stream = space.acquire_stream();
  auto const mr     = space.get_default_allocator();
  std::vector<std::unique_ptr<cudf::column>> made;
  for (std::size_t column = 0; column < columns; ++column) {
    auto const step = static_cast<std::int64_t>(2 * column + 1);
    made.push_back(cudf::sequence(rows,
                                  cudf::numeric_scalar<std::int64_t>(start * step, true, stream, mr),
                                  cudf::numeric_scalar<std::int64_t>(step, true, stream, mr),
                                  stream,
                                  mr));
  }
  auto table = std::make_unique<cudf::table>(std::move(made));
  stream.sync();
  return sirius::make_data_batch(
    std::move(table), space, stream, sirius::telemetry::batch_telemetry_info{});
}

/**
 * @brief One session-level case.
 *
 * `kind`: "bloom" plans Bloom filters only; "auto" is the production choice (exact IN-list while
 * the keys fit the L2 fraction, Bloom otherwise); "bloomzone" and "autozone" add zone maps.
 */
struct session_case {
  std::string kind      = "bloom";
  std::size_t keys      = 1;
  std::size_t gpus      = 1;      ///< Replica GPUs of the plan
  std::size_t batches   = 1;
  std::uint64_t rows    = 1;      ///< Rows per batch
  bool spread           = false;  ///< Batches round-robin over the GPUs; otherwise all on GPU 0
  bool one_shot         = false;  ///< Also time the one-shot shape
};

struct session_result {
  samples begin, enqueue, wait, publish, total, shot_build, shot_replicate;
  std::string published = "?";
  std::size_t filters   = 0;
};

[[nodiscard]] op::dynamic_filter_publish_plan plan_for(acc::fixture const& fixture,
                                                       session_case const& subject)
{
  using plan_type = op::dynamic_filter_publish_plan;
  bool const zone = subject.kind.ends_with("zone");
  std::vector<plan_type::admitted_key> keys;
  std::vector<plan_type::key_binding> bindings;
  cudf::data_type const type{cudf::type_id::INT64};
  for (std::size_t index = 0; index < subject.keys; ++index) {
    keys.push_back({.planner_condition_index = index,
                    .build_key_ordinal       = static_cast<cudf::size_type>(index),
                    .probe_key_ordinal       = static_cast<cudf::size_type>(index),
                    .storage_type            = type,
                    .probe_storage_type      = type});
    bindings.push_back(
      {.admitted_key_index = index, .channel_push_ordinal = index, .probe_storage_type = type});
  }
  std::vector<op::dynamic_filter_replica_space> replicas(
    fixture.spaces.begin(), fixture.spaces.begin() + static_cast<std::ptrdiff_t>(subject.gpus));
  op::dynamic_filter_publication_policy policy{};
  policy.emit_zone_map_filters   = zone;
  policy.max_bloom_bytes_per_gpu = std::uint64_t{16} << 30;
  policy.bloom_only              = subject.kind.starts_with("bloom");
  return plan_type{std::move(keys),
                   {{fixture.channel, op::dynamic_filter_route_class::scan, zone, std::move(bindings)}},
                   std::move(replicas),
                   policy};
}

[[nodiscard]] std::string describe(op::sirius_dynamic_filter_set const& channel,
                                   std::size_t& count)
{
  std::size_t bloom = 0, small = 0, hash = 0, zone = 0;
  auto const snapshot = channel.snapshot();
  for (auto const& entry : snapshot.entries()) {
    auto const* filter = entry.filter.get();
    if (dynamic_cast<op::sirius_dynamic_bloom_filter const*>(filter)) {
      ++bloom;
    } else if (dynamic_cast<op::sirius_dynamic_small_in_list_filter const*>(filter)) {
      ++small;
    } else if (dynamic_cast<op::sirius_dynamic_in_list_filter const*>(filter)) {
      ++hash;
    } else {
      ++zone;
    }
  }
  count = bloom + small + hash + zone;
  if (count == 0) { return "NONE"; }
  std::string text;
  if (bloom) { text += "bloom" + std::to_string(bloom); }
  if (small) { text += "small" + std::to_string(small); }
  if (hash) { text += "hash" + std::to_string(hash); }
  if (zone) { text += "zone" + std::to_string(zone); }
  return text;
}

/**
 * @brief The one-shot shape the base built: one column constructor per key over the whole build on
 * GPU 0, then `replicate_to_devices`. The concatenation itself is not timed: the join needs it
 * either way.
 */
void one_shot(acc::fixture const& fixture,
              session_case const& subject,
              cudf::table_view const& whole,
              std::string const& published,
              session_result& result)
{
  rmm::cuda_set_device_raii guard{rmm::cuda_device_id{0}};
  auto const stream = fixture.publication_stream(0);
  auto const mr     = fixture.gpu(0).get_default_allocator();
  std::vector<std::shared_ptr<op::sirius_dynamic_filter>> filters;
  auto const start = clock_type::now();
  for (std::size_t key = 0; key < subject.keys; ++key) {
    auto const column = whole.column(static_cast<cudf::size_type>(key));
    if (published.starts_with("small")) {
      filters.push_back(std::make_shared<op::sirius_dynamic_small_in_list_filter>(column, stream, mr));
    } else if (published.starts_with("hash")) {
      filters.push_back(std::make_shared<op::sirius_dynamic_in_list_filter>(column, stream, mr));
    } else {
      filters.push_back(std::make_shared<op::sirius_dynamic_bloom_filter>(column, stream, mr));
    }
  }
  stream.sync();
  auto const built = clock_type::now();
  std::span<op::dynamic_filter_replica_space const> const spaces{fixture.spaces.data(),
                                                                 subject.gpus};
  if (subject.gpus > 1) {
    for (auto const& filter : filters) {
      auto* replicable = dynamic_cast<op::sirius_device_replicable*>(filter.get());
      REQUIRE(replicable != nullptr);
      replicable->replicate_to_devices(spaces);
    }
    sync_devices(subject.gpus);
  }
  auto const replicated = clock_type::now();
  result.shot_build.add(us_between(start, built));
  result.shot_replicate.add(us_between(built, replicated));
  filters.clear();
  sync_devices(subject.gpus);
}

/**
 * @brief Runs `subject` through the publication session `reps()` times after one warm-up.
 */
[[nodiscard]] session_result run_session(acc::fixture& fixture, session_case const& subject)
{
  session_result result;
  std::vector<acc::batch_ptr> batches;
  for (std::size_t index = 0; index < subject.batches; ++index) {
    auto const device = subject.spread ? static_cast<int>(index % subject.gpus) : 0;
    batches.push_back(make_keys(fixture,
                                device,
                                static_cast<std::int64_t>(index * subject.rows),
                                static_cast<cudf::size_type>(subject.rows),
                                subject.keys));
  }
  std::unique_ptr<cudf::table> whole;
  if (subject.one_shot) {
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{0}};
    auto const stream = fixture.publication_stream(0);
    std::vector<acc::batch_ptr> local;
    std::vector<cudf::table_view> views;
    for (std::size_t index = 0; index < subject.batches; ++index) {
      // The one-shot build sees every batch on GPU 0, as CONCAT delivered it.
      local.push_back(subject.spread ? make_keys(fixture,
                                                 0,
                                                 static_cast<std::int64_t>(index * subject.rows),
                                                 static_cast<cudf::size_type>(subject.rows),
                                                 subject.keys)
                                     : batches[index]);
      views.push_back(sirius::get_cudf_table_view(*local.back()));
    }
    whole = cudf::concatenate(views, stream, fixture.gpu(0).get_default_allocator());
    stream.sync();
  }
  sync_devices(fixture.devices);

  for (std::size_t rep = 0; rep <= reps(); ++rep) {
    bool const record = rep != 0;
    fixture.session.reset();
    fixture.channel = std::make_shared<op::sirius_dynamic_filter_set>();
    fixture.session =
      std::make_unique<op::dynamic_filter_publication_session>(plan_for(fixture, subject),
                                                               &fixture.stats);
    auto certified = acc::fixture::certify(batches);
    REQUIRE(std::holds_alternative<op::complete_build_inventory>(certified));

    auto const start = clock_type::now();
    bool const begun =
      fixture.session->begin(std::move(std::get<op::complete_build_inventory>(certified)));
    auto const begin_done = clock_type::now();
    REQUIRE(begun);

    double enqueue = 0;
    double wait    = 0;
    for (auto const& batch : batches) {
      auto source       = batch->to_read_only();
      auto const device = source.get_memory_space()->get_device_id();
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{device}};
      auto const stream = fixture.task_stream(device);
      auto const before = clock_type::now();
      fixture.session->contribute(batch->get_batch_id(), source, stream);
      auto const queued = clock_type::now();
      // The task's post-operator sync, which retires the inserts before the input is released.
      stream.sync();
      auto const retired = clock_type::now();
      enqueue += us_between(before, queued);
      wait += us_between(queued, retired);
    }
    auto const last = batches.back();
    auto const root = last->to_read_only().get_memory_space()->get_device_id();
    auto const publish_start = clock_type::now();
    fixture.publish(root, last->get_batch_id());
    auto const published = clock_type::now();

    if (record) {
      result.begin.add(us_between(start, begin_done));
      result.enqueue.add(enqueue / static_cast<double>(subject.batches));
      result.wait.add(wait / static_cast<double>(subject.batches));
      result.publish.add(us_between(publish_start, published));
      result.total.add(us_between(start, begin_done) + enqueue + wait +
                       us_between(publish_start, published));
    }
    result.published = describe(*fixture.channel, result.filters);
    fixture.session->finalize_input();
    fixture.session.reset();
    fixture.channel.reset();
    sync_devices(fixture.devices);
    if (subject.one_shot && result.filters != 0 && (record || rep == 0)) {
      session_result discard;
      one_shot(fixture, subject, whole->view(), result.published, record ? result : discard);
    }
  }
  return result;
}

void print(char const* name, session_case const& subject, session_result const& result)
{
  std::printf(
    "ROW,%s,kind=%s,keys=%zu,gpus=%zu,spread=%d,batches=%zu,rows=%llu,published=%s,"
    "begin_us=%.1f,begin_min=%.1f,enq_us=%.2f,enq_min=%.2f,wait_us=%.2f,wait_min=%.2f,"
    "publish_us=%.1f,publish_min=%.1f,total_us=%.1f,total_min=%.1f",
    name,
    subject.kind.c_str(),
    subject.keys,
    subject.gpus,
    subject.spread ? 1 : 0,
    subject.batches,
    static_cast<unsigned long long>(subject.rows),
    result.published.c_str(),
    result.begin.median(),
    result.begin.minimum(),
    result.enqueue.median(),
    result.enqueue.minimum(),
    result.wait.median(),
    result.wait.minimum(),
    result.publish.median(),
    result.publish.minimum(),
    result.total.median(),
    result.total.minimum());
  if (subject.one_shot) {
    std::printf(",shot_build_us=%.1f,shot_build_min=%.1f,shot_repl_us=%.1f,shot_repl_min=%.1f",
                result.shot_build.median(),
                result.shot_build.minimum(),
                result.shot_replicate.median(),
                result.shot_replicate.minimum());
  }
  std::printf("\n");
  std::fflush(stdout);
}

[[nodiscard]] std::size_t usable_gpus(std::size_t wanted)
{
  return std::min<std::size_t>(wanted, static_cast<std::size_t>(acc::visible_gpus()));
}

/**
 * @brief Flushes the CUDA trace when a case ends: nsys loses the API trace of this binary
 * otherwise.
 */
struct trace_flush {
  trace_flush()                              = default;
  trace_flush(trace_flush const&)            = delete;
  trace_flush& operator=(trace_flush const&) = delete;
  ~trace_flush() { (void)cudaProfilerStop(); }
};

}  // namespace

TEST_CASE("bench dynamic filter primitives (MB0)", "[!benchmark][dynamic_filter][mb0]")
{
  trace_flush const flush;
  acc::fixture fixture({.devices = 1, .gpu_bytes = 1024 * acc::mib});
  rmm::cuda_set_device_raii guard{rmm::cuda_device_id{0}};
  auto const stream = fixture.task_stream(0);
  rmm::device_buffer buffer{4096, stream, fixture.gpu(0).get_default_allocator()};
  std::size_t constexpr rounds = 2000;
  auto const price = [&](char const* name, auto&& body) {
    samples each;
    for (std::size_t rep = 0; rep <= reps(); ++rep) {
      auto const start = clock_type::now();
      for (std::size_t round = 0; round < rounds; ++round) {
        body();
      }
      stream.sync();
      if (rep != 0) { each.add(us_between(start, clock_type::now()) / rounds); }
    }
    std::printf("ROW,MB0,what=%s,us=%.3f,min=%.3f\n", name, each.median(), each.minimum());
  };
  price("memset4_enqueue", [&] { (void)cudaMemsetAsync(buffer.data(), 0, 4, stream.get()); });
  price("memset4_plus_wait", [&] {
    (void)cudaMemsetAsync(buffer.data(), 0, 4, stream.get());
    stream.sync();
  });
  price("idle_stream_sync", [&] { stream.sync(); });
  std::uint32_t word = 0;
  price("d2h4_plus_wait", [&] {
    (void)cudaMemcpyAsync(&word, buffer.data(), 4, cudaMemcpyDeviceToHost, stream.get());
    stream.sync();
  });
  price("stream_create_destroy",
        [&] { rmm::cuda_stream made{rmm::cuda_stream::flags::non_blocking}; });
  price("event_create_record_destroy", [&] {
    cudaEvent_t event{};
    (void)cudaEventCreateWithFlags(&event, cudaEventDisableTiming);
    (void)cudaEventRecord(event, stream.get());
    (void)cudaEventDestroy(event);
  });
  cudaEvent_t shared{};
  REQUIRE(cudaEventCreateWithFlags(&shared, cudaEventDisableTiming) == cudaSuccess);
  rmm::cuda_stream other{rmm::cuda_stream::flags::non_blocking};
  price("event_record_plus_wait_event", [&] {
    (void)cudaEventRecord(shared, stream.get());
    (void)cudaStreamWaitEvent(other.value(), shared, 0);
  });
  int device = 0;
  price("stream_get_device", [&] { (void)cudaStreamGetDevice(stream.get(), &device); });
  price("alloc_free_4k", [&] {
    rmm::device_buffer scratch{4096, stream, fixture.gpu(0).get_default_allocator()};
  });
  other.synchronize();
  REQUIRE(cudaEventDestroy(shared) == cudaSuccess);
}

TEST_CASE("bench dynamic filter contribution fixed cost (MB2)", "[!benchmark][dynamic_filter][mb2]")
{
  trace_flush const flush;
  auto const gpus_list = env_list("SIRIUS_BENCH_DF_GPUS", {1});
  for (auto const wanted : gpus_list) {
    auto const gpus = usable_gpus(wanted);
    if (gpus != wanted) { continue; }
    acc::fixture fixture({.devices = gpus, .gpu_bytes = 8192 * acc::mib});
    for (auto const& kind : env_names("SIRIUS_BENCH_DF_KINDS", {"bloom", "auto", "bloomzone"})) {
      for (auto const keys : env_list("SIRIUS_BENCH_DF_KEYS", {1, 2, 4})) {
        for (auto const rows : env_list("SIRIUS_BENCH_DF_ROWS", {1, 32, 1000})) {
          for (auto const batches : env_list("SIRIUS_BENCH_DF_BATCHES", {1, 8, 64, 512})) {
            session_case const subject{.kind    = kind,
                                       .keys    = keys,
                                       .gpus    = gpus,
                                       .batches = batches,
                                       .rows    = rows,
                                       .spread  = gpus > 1};
            print("MB2", subject, run_session(fixture, subject));
          }
        }
      }
    }
  }
}

TEST_CASE("bench dynamic filter begin (MB3)", "[!benchmark][dynamic_filter][mb3]")
{
  trace_flush const flush;
  using builder = op::detail::accumulated_bloom_builder;
  for (auto const wanted : env_list("SIRIUS_BENCH_DF_GPUS", {1, 2, 4})) {
    auto const gpus = usable_gpus(wanted);
    if (gpus != wanted) { continue; }
    acc::fixture fixture({.devices = gpus, .gpu_bytes = std::size_t{40} << 30});
    std::span<op::dynamic_filter_replica_space const> const spaces{fixture.spaces.data(), gpus};
    for (auto const keys : env_list("SIRIUS_BENCH_DF_KEYS", {1, 2})) {
      for (auto const megabytes :
           env_list("SIRIUS_BENCH_DF_MB", {1, 16, 128, 512, 1024, 3072, 12288})) {
        auto const bytes  = megabytes << 20;
        auto const blocks = bytes / op::bloom_bytes_per_block / keys;
        samples create, zero, destroy;
        for (std::size_t rep = 0; rep <= reps(); ++rep) {
          std::vector<builder::key> planned;
          for (std::size_t key = 0; key < keys; ++key) {
            planned.push_back(
              {static_cast<cudf::size_type>(key), cudf::data_type{cudf::type_id::INT64}, blocks});
          }
          auto const start = clock_type::now();
          auto made        = builder::try_create(std::move(planned), spaces);
          auto const leased = clock_type::now();
          REQUIRE(made);
          sync_devices(gpus);
          auto const zeroed = clock_type::now();
          made.reset();
          sync_devices(gpus);
          auto const released = clock_type::now();
          if (rep != 0) {
            create.add(us_between(start, leased));
            zero.add(us_between(leased, zeroed));
            destroy.add(us_between(zeroed, released));
          }
        }
        std::printf(
          "ROW,MB3,gpus=%zu,keys=%zu,total_mb=%llu,create_us=%.1f,create_min=%.1f,"
          "zero_wait_us=%.1f,zero_wait_min=%.1f,destroy_us=%.1f,destroy_min=%.1f\n",
          gpus,
          keys,
          static_cast<unsigned long long>(megabytes),
          create.median(),
          create.minimum(),
          zero.median(),
          zero.minimum(),
          destroy.median(),
          destroy.minimum());
        std::fflush(stdout);
      }
    }
  }
}

TEST_CASE("bench dynamic filter Bloom finish (MB4)", "[!benchmark][dynamic_filter][mb4]")
{
  trace_flush const flush;
  using builder = op::detail::accumulated_bloom_builder;
  for (auto const wanted : env_list("SIRIUS_BENCH_DF_GPUS", {1, 2, 4})) {
    auto const gpus = usable_gpus(wanted);
    if (gpus != wanted) { continue; }
    acc::fixture fixture({.devices = gpus, .gpu_bytes = std::size_t{40} << 30});
    std::span<op::dynamic_filter_replica_space const> const spaces{fixture.spaces.data(), gpus};
    std::vector<acc::batch_ptr> batches;
    for (std::size_t device = 0; device < gpus; ++device) {
      batches.push_back(make_keys(
        fixture, static_cast<int>(device), static_cast<std::int64_t>(device) * 1000, 1000, 1));
    }
    for (auto const chunk : env_list("SIRIUS_BENCH_DF_CHUNK_MIB", {0, 32, 64})) {
      if (chunk == 0) {
        unsetenv("SIRIUS_PROTO_BLOOM_CHUNK_MIB");
      } else {
        setenv("SIRIUS_PROTO_BLOOM_CHUNK_MIB", std::to_string(chunk).c_str(), 1);
      }
      for (auto const megabytes : env_list("SIRIUS_BENCH_DF_MB", {1, 16, 256, 1024, 3072})) {
        // Sources: 1 = only the root GPU contributed; `gpus` = every GPU contributed.
        for (auto const sources : std::vector<std::size_t>{1, gpus}) {
          if (sources == 1 && gpus == 1 && megabytes != 1) {
            // Kept: the single-GPU case has one source by construction.
          }
          auto const blocks = (megabytes << 20) / op::bloom_bytes_per_block;
          samples finish;
          int transport = -1;
          for (std::size_t rep = 0; rep <= reps(); ++rep) {
            auto made = builder::try_create(
              {{0, cudf::data_type{cudf::type_id::INT64}, blocks}}, spaces);
            REQUIRE(made);
            for (std::size_t device = 0; device < sources; ++device) {
              auto source = batches[device]->to_read_only();
              rmm::cuda_set_device_raii guard{rmm::cuda_device_id{static_cast<int>(device)}};
              REQUIRE(made->enqueue_add(sirius::get_cudf_table_view(source),
                                        rmm::cuda_device_id{static_cast<int>(device)},
                                        fixture.task_stream(static_cast<int>(device))));
              fixture.task_stream(static_cast<int>(device)).sync();
            }
            sync_devices(gpus);
            {
              rmm::cuda_set_device_raii guard{rmm::cuda_device_id{0}};
              auto const start = clock_type::now();
              auto published   = made->finish(rmm::cuda_device_id{0},
                                              fixture.publication_stream(0),
                                              fixture.gpu(0).get_default_allocator());
              auto const done  = clock_type::now();
              REQUIRE(published);
              transport = static_cast<int>(published->transport);
              if (rep != 0) { finish.add(us_between(start, done)); }
            }
            sync_devices(gpus);
            made.reset();
            sync_devices(gpus);
          }
          std::printf(
            "ROW,MB4,gpus=%zu,sources=%zu,mb=%llu,chunk_mib=%llu,transport=%s,finish_us=%.1f,"
            "finish_min=%.1f\n",
            gpus,
            sources,
            static_cast<unsigned long long>(megabytes),
            static_cast<unsigned long long>(chunk == 0 ? 8 : chunk),
            transport == 0 ? "PEER" : "HOST",
            finish.median(),
            finish.minimum());
          std::fflush(stdout);
          if (gpus == 1) { break; }
        }
      }
    }
    unsetenv("SIRIUS_PROTO_BLOOM_CHUNK_MIB");
  }
}

TEST_CASE("bench dynamic filter exact slices at finish (MB4x)",
          "[!benchmark][dynamic_filter][mb4x]")
{
  trace_flush const flush;
  for (auto const wanted : env_list("SIRIUS_BENCH_DF_GPUS", {1, 2, 4})) {
    auto const gpus = usable_gpus(wanted);
    if (gpus != wanted) { continue; }
    acc::fixture fixture({.devices = gpus, .gpu_bytes = 8192 * acc::mib});
    for (auto const keys : env_list("SIRIUS_BENCH_DF_KEYS", {1, 2})) {
      for (auto const total : env_list("SIRIUS_BENCH_DF_TOTAL_ROWS", {512, 32768})) {
        for (auto const batches : env_list("SIRIUS_BENCH_DF_BATCHES", {1, 64, 512})) {
          for (auto const spread : std::vector<bool>{false, true}) {
            if (spread && gpus == 1) { continue; }
            session_case const subject{.kind    = "auto",
                                       .keys    = keys,
                                       .gpus    = gpus,
                                       .batches = batches,
                                       .rows    = total / batches,
                                       .spread  = spread};
            print("MB4x", subject, run_session(fixture, subject));
          }
        }
      }
    }
  }
}

TEST_CASE("bench dynamic filter small build against one shot (MB5)",
          "[!benchmark][dynamic_filter][mb5]")
{
  trace_flush const flush;
  for (auto const wanted : env_list("SIRIUS_BENCH_DF_GPUS", {1, 2, 4})) {
    auto const gpus = usable_gpus(wanted);
    if (gpus != wanted) { continue; }
    acc::fixture fixture({.devices = gpus, .gpu_bytes = 8192 * acc::mib});
    for (auto const& kind : env_names("SIRIUS_BENCH_DF_KINDS", {"auto", "bloom"})) {
      for (auto const keys : env_list("SIRIUS_BENCH_DF_KEYS", {1, 2})) {
        for (auto const total :
             env_list("SIRIUS_BENCH_DF_TOTAL_ROWS", {12, 1000, 100000, 10000000})) {
          for (auto const batches : env_list("SIRIUS_BENCH_DF_BATCHES", {1, 16, 128})) {
            if (total < batches) { continue; }
            for (auto const spread : std::vector<bool>{false, true}) {
              if (spread && (gpus == 1 || batches < gpus)) { continue; }
              session_case const subject{.kind     = kind,
                                         .keys     = keys,
                                         .gpus     = gpus,
                                         .batches  = batches,
                                         .rows     = total / batches,
                                         .spread   = spread,
                                         .one_shot = true};
              print("MB5", subject, run_session(fixture, subject));
            }
          }
        }
      }
    }
  }
}

TEST_CASE("bench dynamic filter bounds against rows (MB7)", "[!benchmark][dynamic_filter][mb7]")
{
  trace_flush const flush;
  acc::fixture fixture({.devices = 1, .gpu_bytes = 16384 * acc::mib});
  for (auto const rows : env_list("SIRIUS_BENCH_DF_ROWS", {1000000, 10000000, 100000000})) {
    for (auto const& kind : std::vector<std::string>{"bloom", "bloomzone"}) {
      session_case const subject{.kind = kind, .keys = 1, .gpus = 1, .batches = 1, .rows = rows};
      print("MB7", subject, run_session(fixture, subject));
    }
  }
}

// PROTO(sgb) MB8: tip shape (each source GPU inserts its share, root merges and replicates) against
// local replicas (every GPU inserts the whole build, no cross-GPU copy). One thread per GPU.
TEST_CASE("bench dynamic filter local replicas (MB8)", "[!benchmark][dynamic_filter][mb8]")
{
  trace_flush const flush;
  using builder = op::detail::accumulated_bloom_builder;
  for (auto const wanted : env_list("SIRIUS_BENCH_DF_GPUS", {2, 4})) {
    auto const gpus = usable_gpus(wanted);
    if (gpus != wanted) { continue; }
    acc::fixture fixture({.devices = gpus, .gpu_bytes = std::size_t{40} << 30});
    std::span<op::dynamic_filter_replica_space const> const spaces{fixture.spaces.data(), gpus};
    for (auto const megabytes : env_list("SIRIUS_BENCH_DF_MB", {57, 174, 281, 1434})) {
      auto const blocks = (megabytes << 20) / op::bloom_bytes_per_block;
      auto const rows   = static_cast<cudf::size_type>(blocks * 16);
      std::vector<acc::batch_ptr> whole;
      for (std::size_t device = 0; device < gpus; ++device) {
        whole.push_back(make_keys(fixture, static_cast<int>(device), 0, rows, 1));
      }
      std::vector<std::size_t> placements{1, 2, gpus};
      placements.erase(std::unique(placements.begin(), placements.end()), placements.end());
      for (auto const sources : placements) {
        samples tip_insert, tip_finish, tip_total, local_total;
        int transport = -1;
        for (std::size_t rep = 0; rep <= reps(); ++rep) {
          {
            auto made =
              builder::try_create({{0, cudf::data_type{cudf::type_id::INT64}, blocks}}, spaces);
            REQUIRE(made);
            sync_devices(gpus);
            auto const start = clock_type::now();
            std::vector<std::thread> workers;
            for (std::size_t device = 0; device < sources; ++device) {
              workers.emplace_back([&, device] {
                rmm::cuda_set_device_raii guard{rmm::cuda_device_id{static_cast<int>(device)}};
                auto source      = whole[device]->to_read_only();
                auto const share = rows / static_cast<cudf::size_type>(sources);
                auto const first = share * static_cast<cudf::size_type>(device);
                auto const last  = device + 1 == sources ? rows : first + share;
                auto const part =
                  cudf::slice(sirius::get_cudf_table_view(source), {first, last}).front();
                (void)made->enqueue_add(part,
                                        rmm::cuda_device_id{static_cast<int>(device)},
                                        fixture.task_stream(static_cast<int>(device)));
                fixture.task_stream(static_cast<int>(device)).sync();
              });
            }
            for (auto& worker : workers) {
              worker.join();
            }
            auto const inserted = clock_type::now();
            {
              rmm::cuda_set_device_raii guard{rmm::cuda_device_id{0}};
              auto published = made->finish(rmm::cuda_device_id{0},
                                            fixture.publication_stream(0),
                                            fixture.gpu(0).get_default_allocator());
              REQUIRE(published);
              transport = static_cast<int>(published->transport);
            }
            auto const done = clock_type::now();
            if (rep != 0) {
              tip_insert.add(us_between(start, inserted));
              tip_finish.add(us_between(inserted, done));
              tip_total.add(us_between(start, done));
            }
            sync_devices(gpus);
            made.reset();
            sync_devices(gpus);
          }
          {
            auto made =
              builder::try_create({{0, cudf::data_type{cudf::type_id::INT64}, blocks}}, spaces);
            REQUIRE(made);
            sync_devices(gpus);
            auto const start = clock_type::now();
            std::vector<std::thread> workers;
            for (std::size_t device = 0; device < gpus; ++device) {
              workers.emplace_back([&, device] {
                rmm::cuda_set_device_raii guard{rmm::cuda_device_id{static_cast<int>(device)}};
                auto source = whole[device]->to_read_only();
                (void)made->enqueue_add_local(sirius::get_cudf_table_view(source),
                                              rmm::cuda_device_id{static_cast<int>(device)});
              });
            }
            for (auto& worker : workers) {
              worker.join();
            }
            {
              rmm::cuda_set_device_raii guard{rmm::cuda_device_id{0}};
              auto published = made->finish_local(rmm::cuda_device_id{0});
              REQUIRE(published);
            }
            auto const done = clock_type::now();
            if (rep != 0) { local_total.add(us_between(start, done)); }
            sync_devices(gpus);
            made.reset();
            sync_devices(gpus);
          }
        }
        std::printf(
          "ROW,MB8,gpus=%zu,sources=%zu,mb=%llu,rows=%d,transport=%s,tip_insert_us=%.0f,"
          "tip_finish_us=%.0f,tip_total_us=%.0f,tip_total_min=%.0f,local_total_us=%.0f,"
          "local_total_min=%.0f\n",
          gpus,
          sources,
          static_cast<unsigned long long>(megabytes),
          rows,
          transport == 0 ? "PEER" : "HOST",
          tip_insert.median(),
          tip_finish.median(),
          tip_total.median(),
          tip_total.minimum(),
          local_total.median(),
          local_total.minimum());
        std::fflush(stdout);
      }
      whole.clear();
      sync_devices(gpus);
    }
  }
}
