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

#include "catch.hpp"
#include "cudf/ast_compat.hpp"
#include "io/cache/controlled_backend.hpp"
#include "op/scan/parquet_materialize.hpp"
#include "scan/test_utils.hpp"
#include "utils/host_allocation_fault.hpp"

#include <cudf/io/experimental/hybrid_scan.hpp>
#include <cudf/scalar/scalar.hpp>

#include <rmm/cuda_device.hpp>
#include <rmm/cuda_stream.hpp>
#include <rmm/device_buffer.hpp>
#include <rmm/mr/cuda_memory_resource.hpp>

#include <cuda/stream>
#include <cuda_runtime.h>

#include <duckdb.hpp>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace sirius::test::controlled;
using sirius::op::scan::materialize_parquet;
using sirius::op::scan::parquet_source;
using byte_range       = cudf::io::text::byte_range_info;
using allocation_fault = sirius::test::scoped_host_allocation_fault;

struct parquet_file {
  std::vector<std::uint8_t> bytes;
  std::shared_ptr<cudf::io::parquet::FileMetaData const> metadata;
};

std::array<parquet_file, 3> make_fixture_files(bool filtered)
{
  auto const directory =
    std::filesystem::temp_directory_path() / ("sirius_bulk_lifetime_" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  duckdb::DuckDB database{nullptr};
  duckdb::Connection connection{database};
  std::array<parquet_file, 3> result;
  auto const options = cudf::io::parquet_reader_options::builder().build();
  for (std::size_t i = 0; i < result.size(); ++i) {
    auto const path   = directory / (std::to_string(i) + ".parquet");
    auto const select = filtered ? "SELECT i::BIGINT id, (i * 2)::BIGINT payload FROM range(" +
                                     std::to_string(i * 8192) + "," +
                                     std::to_string((i + 1) * 8192) + ") t(i)"
                                 : "SELECT " + std::to_string(101 + i) + "::BIGINT AS value";
    auto query =
      connection.Query("COPY (" + select + ") TO '" + path.string() +
                       "' (FORMAT PARQUET, ROW_GROUP_SIZE 2048, COMPRESSION UNCOMPRESSED)");
    if (query->HasError()) { throw std::runtime_error(query->GetError()); }
    std::ifstream input{path, std::ios::binary};
    auto& file = result[i];
    file.bytes.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    if (file.bytes.size() < 8) { throw std::runtime_error("missing parquet fixture footer"); }
    std::uint32_t footer_size{};
    std::memcpy(&footer_size, file.bytes.data() + file.bytes.size() - 8, sizeof(footer_size));
    if (footer_size > file.bytes.size() - 8) {
      throw std::runtime_error("invalid parquet fixture footer");
    }
    cudf::io::parquet::experimental::hybrid_scan_reader reader{
      cudf::host_span<std::uint8_t const>{file.bytes.data() + file.bytes.size() - 8 - footer_size,
                                          footer_size},
      options};
    file.metadata = std::make_shared<cudf::io::parquet::FileMetaData>(reader.parquet_metadata());
  }
  std::filesystem::remove_all(directory);
  return result;
}

/// The read layout the materializer derives: within each of @p groups equally sized row groups,
/// chunks that follow each other in the file merge into one (offset, size) read.
std::vector<std::pair<std::size_t, std::size_t>> coalesced_runs(
  std::vector<byte_range> const& ranges, std::size_t groups)
{
  std::vector<std::pair<std::size_t, std::size_t>> runs;
  auto const per_group = ranges.size() / groups;
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    auto const offset = static_cast<std::size_t>(ranges[i].offset());
    auto const size   = static_cast<std::size_t>(ranges[i].size());
    if (!runs.empty() && i % per_group != 0 && runs.back().first + runs.back().second == offset) {
      runs.back().second += size;
    } else {
      runs.emplace_back(offset, size);
    }
  }
  return runs;
}

std::array<parquet_file, 3> const& fixture_files()
{
  static auto const files = make_fixture_files(false);
  return files;
}

std::array<parquet_file, 3> const& filtered_fixture_files()
{
  static auto const files = make_fixture_files(true);
  return files;
}

class materialize_reactor : public controlled_reactor {
 public:
  static constexpr bool prefers_bulk_io = true;
};

/**
 * Parks file reads with preallocated completion states. All requests remain owned here until
 * settled, and shutdown also rejects submissions made while an assertion unwinds. No producer
 * writes after publishing completion.
 */
class materialize_context final : public sirius::io::templated_ioctx<materialize_reactor> {
 public:
  struct preparation_probe {
    allocation_fault& fault;
    std::size_t allocations{0};
    std::size_t backend_entries{0};
  };

  struct request {
    sirius::exec::promise<std::size_t> promise;
    sirius::exec::semi_future<std::size_t> future{promise.get_semi_future()};
    std::vector<sirius::io::slice> slices;
    ::cuda::stream_ref stream{cudaStream_t{nullptr}};
    std::size_t file{};
    rmm::cuda_device_id device;
    bool settled{false};
  };

  explicit materialize_context(std::array<parquet_file, 3> const& files)
    : templated_ioctx{1, [] { return std::make_unique<materialize_reactor>(); }},
      _files{files},
      _teardown_error{std::make_exception_ptr(std::runtime_error("materialize test teardown"))}
  {
  }

  ~materialize_context() override
  {
    pre_destroy();
    drain();
  }

  sirius::io::io_context_type type() const noexcept override
  {
    return sirius::io::io_context_type::uring;
  }

  std::size_t host_read_io(sirius::io::io_object const&,
                           std::size_t,
                           std::size_t,
                           std::uint8_t*) override
  {
    ++host_reads;
    throw std::runtime_error("unexpected host read with supplied footer");
  }

  sirius::exec::semi_future<std::size_t> mixed_readv_async_io(
    sirius::io::io_object const&, std::vector<sirius::io::prepared_io_slice>&&) noexcept override
  {
    ++unexpected_reads;
    return sirius::exec::make_semi_future<std::size_t>(_teardown_error);
  }

  sirius::exec::semi_future<std::size_t> device_readv_async_io(
    sirius::io::io_object const& object,
    std::span<sirius::io::slice const> slices,
    ::cuda::stream_ref stream) noexcept override
  {
    if (probe_preparation != nullptr) {
      auto& probe = *probe_preparation;
      if (probe.backend_entries == 0) { probe.allocations = probe.fault.stop(); }
      // Stop before fixture descriptor allocation or destination publication. Prebuilt failed
      // completions let the baseline drain without issuing I/O or entering decode.
      auto& read = _requests[probe.backend_entries++];
      read.promise.set_exception(_teardown_error);
      return std::move(read.future);
    }
    try {
      std::unique_lock lock{_mutex};
      auto& read = _requests.at(_submitted);
      read.slices.assign(slices.begin(), slices.end());
      read.stream = stream;
      read.device = rmm::get_current_cuda_device();
      read.file   = static_cast<std::size_t>(object.object_path().back() - '0');
      auto future = std::move(read.future);
      ++_submitted;
      if (_draining) {
        read.settled = true;
        read.promise.set_exception(_teardown_error);
      } else {
        outstanding.fetch_add(1);
      }
      lock.unlock();
      _changed.notify_all();
      if (block_first_submission && _submitted == 1) {
        _submission_entered.release();
        _submission_release.acquire();
      }
      if (arm_after_first != nullptr && _submitted == 1) {
        arm_after_first->emplace(allocation_fault::sustained_allocation_scope, 1);
      }
      return future;
    } catch (...) {
      return sirius::exec::make_semi_future<std::size_t>(std::current_exception());
    }
  }

  bool await_submissions(std::size_t count)
  {
    std::unique_lock lock{_mutex};
    return _changed.wait_for(lock, 5s, [&] { return _submitted >= count; });
  }

  bool await_blocked_submission() { return _submission_entered.try_acquire_for(5s); }

  void release_submission() noexcept
  {
    if (!_submission_released.exchange(true)) { _submission_release.release(); }
  }

  std::size_t submitted() const
  {
    std::lock_guard lock{_mutex};
    return _submitted;
  }

  request const& submitted_request(std::size_t index) const { return _requests.at(index); }

  void fail(std::size_t index, std::exception_ptr error)
  {
    std::lock_guard lock{_mutex};
    auto& read = _requests.at(index);
    if (read.settled) { return; }
    read.settled = true;
    outstanding.fetch_sub(1);
    read.promise.set_exception(std::move(error));
  }

  void finish(std::size_t index, std::optional<std::size_t> reported_bytes = std::nullopt)
  {
    auto& read        = _requests.at(index);
    auto const& bytes = _files.at(read.file).bytes;
    std::size_t total{};
    for (auto const& slice : read.slices) {
      if (slice.offset() > bytes.size() || slice.size() > bytes.size() - slice.offset()) {
        throw std::runtime_error("fixture read exceeds parquet bytes");
      }
      auto const status = cudaMemcpyAsync(slice.dst,
                                          bytes.data() + slice.offset(),
                                          slice.size(),
                                          cudaMemcpyHostToDevice,
                                          read.stream.get());
      if (status != cudaSuccess) { throw std::runtime_error(cudaGetErrorString(status)); }
      total += slice.size();
    }
    auto const status = cudaStreamSynchronize(read.stream.get());
    if (status != cudaSuccess) { throw std::runtime_error(cudaGetErrorString(status)); }
    std::lock_guard lock{_mutex};
    read.settled = true;
    outstanding.fetch_sub(1);
    read.promise.set_value(reported_bytes.value_or(total));
  }

  void drain() noexcept
  {
    release_submission();
    std::lock_guard lock{_mutex};
    _draining = true;
    for (std::size_t i = 0; i < _submitted; ++i) {
      auto& read = _requests[i];
      if (!read.settled) {
        read.settled = true;
        outstanding.fetch_sub(1);
        read.promise.set_exception(_teardown_error);
      }
    }
  }

  std::atomic<std::size_t> outstanding{0};
  std::atomic<std::size_t> host_reads{0};
  std::atomic<std::size_t> unexpected_reads{0};
  std::optional<allocation_fault>* arm_after_first{nullptr};
  preparation_probe* probe_preparation{nullptr};
  bool block_first_submission{false};

 private:
  std::array<parquet_file, 3> const& _files;
  mutable std::mutex _mutex;
  std::condition_variable _changed;
  std::binary_semaphore _submission_entered{0};
  std::binary_semaphore _submission_release{0};
  std::atomic<bool> _submission_released{false};
  std::array<request, 3> _requests;
  std::size_t _submitted{0};
  bool _draining{false};
  std::exception_ptr _teardown_error;
};

/**
 * Records allocation capacities and flags any release while a file writer remains pending. Fixed
 * bookkeeping also permits sustained host-allocation failure during unwind.
 */
class tracking_state {
 public:
  explicit tracking_state(std::atomic<std::size_t>& outstanding) : _outstanding{outstanding} {}

  void* allocate(::cuda::stream_ref stream,
                 std::size_t bytes,
                 std::size_t alignment = alignof(std::max_align_t))
  {
    std::lock_guard lock{_mutex};
    if (++_allocations == fail_allocation) { throw std::bad_alloc{}; }
    if (_allocations > _records.size()) { throw std::runtime_error("tracking fixture capacity"); }
    auto* pointer              = _upstream.allocate(stream, bytes, alignment);
    _records[_allocations - 1] = {pointer, bytes, true};
    ++_live;
    return pointer;
  }

  void deallocate(::cuda::stream_ref stream,
                  void* pointer,
                  std::size_t bytes,
                  std::size_t alignment = alignof(std::max_align_t)) noexcept
  {
    {
      std::lock_guard lock{_mutex};
      if (_outstanding.load() != 0) { ++_early_releases; }
      for (auto& record : _records) {
        if (record.pointer == pointer && record.live) {
          record.live = false;
          --_live;
          break;
        }
      }
    }
    _upstream.deallocate(stream, pointer, bytes, alignment);
  }

  void* allocate_sync(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t))
  {
    return allocate(::cuda::stream_ref{cudaStream_t{nullptr}}, bytes, alignment);
  }

  void deallocate_sync(void* pointer,
                       std::size_t bytes,
                       std::size_t alignment = alignof(std::max_align_t)) noexcept
  {
    deallocate(::cuda::stream_ref{cudaStream_t{nullptr}}, pointer, bytes, alignment);
  }

  std::size_t allocations() const
  {
    std::lock_guard lock{_mutex};
    return _allocations;
  }

  std::size_t live() const
  {
    std::lock_guard lock{_mutex};
    return _live;
  }

  std::size_t early_releases() const
  {
    std::lock_guard lock{_mutex};
    return _early_releases;
  }

  std::size_t capacity(void const* pointer) const
  {
    std::lock_guard lock{_mutex};
    for (auto const& record : _records) {
      if (record.pointer == pointer && record.live) { return record.bytes; }
    }
    return 0;
  }

  std::size_t fail_allocation{0};

 private:
  struct allocation {
    void* pointer{nullptr};
    std::size_t bytes{0};
    bool live{false};
  };

  rmm::mr::cuda_memory_resource _upstream;
  std::atomic<std::size_t>& _outstanding;
  mutable std::mutex _mutex;
  std::array<allocation, 128> _records{};
  std::size_t _allocations{0};
  std::size_t _live{0};
  std::size_t _early_releases{0};
};

class tracking_resource {
 public:
  tracking_resource() = default;
  explicit tracking_resource(tracking_state& state) noexcept : _state{&state} {}

  void* allocate(::cuda::stream_ref stream,
                 std::size_t bytes,
                 std::size_t alignment = alignof(std::max_align_t))
  {
    return _state->allocate(stream, bytes, alignment);
  }

  void deallocate(::cuda::stream_ref stream,
                  void* pointer,
                  std::size_t bytes,
                  std::size_t alignment = alignof(std::max_align_t)) noexcept
  {
    _state->deallocate(stream, pointer, bytes, alignment);
  }

  void* allocate_sync(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t))
  {
    return _state->allocate_sync(bytes, alignment);
  }

  void deallocate_sync(void* pointer,
                       std::size_t bytes,
                       std::size_t alignment = alignof(std::max_align_t)) noexcept
  {
    _state->deallocate_sync(pointer, bytes, alignment);
  }

  bool operator==(tracking_resource const& other) const noexcept = default;
  friend void get_property(tracking_resource const&, ::cuda::mr::device_accessible) noexcept {}

 private:
  tracking_state* _state{nullptr};
};

struct materialize_fixture {
  explicit materialize_fixture(std::size_t count                        = 3,
                               std::array<parquet_file, 3> const& files = fixture_files())
    : context{std::make_shared<materialize_context>(files)}, memory{context->outstanding}
  {
    for (std::size_t i = 0; i < count; ++i) {
      cudf::io::parquet::experimental::hybrid_scan_reader reader{*files[i].metadata, options};
      auto groups = reader.all_row_groups(options);
      ranges.push_back(sirius::op::scan::column_chunk_ranges(*files[i].metadata, options, groups));
      sources.push_back({context->open_datasource("controlled://bulk-" + std::to_string(i)),
                         files[i].metadata,
                         std::move(groups)});
    }
  }

  std::unique_ptr<cudf::table> materialize()
  {
    return materialize_parquet(
      sources, options, ranges, stream, rmm::device_async_resource_ref{resource});
  }

  rmm::cuda_device_id device{rmm::get_current_cuda_device()};
  std::shared_ptr<materialize_context> context;
  tracking_state memory;
  tracking_resource resource{memory};
  rmm::cuda_stream stream;
  cudf::io::parquet_reader_options options{cudf::io::parquet_reader_options::builder().build()};
  std::vector<parquet_source> sources;
  std::vector<std::vector<byte_range>> ranges;
};

/**
 * Drains every parked request before joining the materializer, including when REQUIRE exits a test.
 * Declare after the fixture so stream, allocator, and borrowed metadata remain alive through
 * teardown.
 */
class materialize_task {
 public:
  explicit materialize_task(materialize_fixture& fixture, bool fail_second_submission = false)
    : _context{fixture.context},
      result{std::async(std::launch::async, [&fixture, fail_second_submission] {
        rmm::cuda_set_device_raii device{fixture.device};
        std::optional<allocation_fault> fault;
        if (fail_second_submission) { fixture.context->arm_after_first = &fault; }
        try {
          auto table                       = fixture.materialize();
          fixture.context->arm_after_first = nullptr;
          return table;
        } catch (...) {
          fixture.context->arm_after_first = nullptr;
          fault.reset();
          throw;
        }
      })}
  {
  }

  ~materialize_task()
  {
    _context->drain();
    if (result.valid()) {
      try {
        static_cast<void>(result.get());
      } catch (...) {
      }
    }
  }

  materialize_task(materialize_task const&)            = delete;
  materialize_task& operator=(materialize_task const&) = delete;

 private:
  std::shared_ptr<materialize_context> _context;

 public:
  std::future<std::unique_ptr<cudf::table>> result;
};

}  // namespace

TEST_CASE("bulk materialization submits all files before waiting and preserves source order",
          "[parquet_helpers][bulk_lifetime][gpu_execution]")
{
  materialize_fixture fixture;
  materialize_task task{fixture};
  REQUIRE(fixture.context->await_submissions(3));
  CHECK(task.result.wait_for(0ms) == std::future_status::timeout);
  bool has_padded_chunk = false;
  for (std::size_t i = 0; i < 3; ++i) {
    auto const& request = fixture.context->submitted_request(i);
    CHECK(request.file == i);
    REQUIRE(request.slices.size() == fixture.ranges[i].size());
    for (std::size_t j = 0; j < request.slices.size(); ++j) {
      auto const& slice = request.slices[j];
      has_padded_chunk  = has_padded_chunk || (slice.size() % 8 != 0);
      CHECK(slice.size() == static_cast<std::size_t>(fixture.ranges[i][j].size()));
      CHECK(fixture.memory.capacity(slice.dst) == ((slice.size() + 7) & ~std::size_t{7}));
    }
  }
  REQUIRE(has_padded_chunk);
  fixture.context->finish(2);
  fixture.context->finish(1);
  CHECK(task.result.wait_for(0ms) == std::future_status::timeout);
  fixture.context->finish(0);
  REQUIRE(task.result.wait_for(5s) == std::future_status::ready);
  auto table = task.result.get();
  REQUIRE(table->num_columns() == 1);
  REQUIRE(table->num_rows() == 3);
  std::array<std::int64_t, 3> values{};
  REQUIRE(cudaMemcpyAsync(values.data(),
                          table->view().column(0).data<std::int64_t>(),
                          sizeof(values),
                          cudaMemcpyDeviceToHost,
                          fixture.stream.value()) == cudaSuccess);
  fixture.stream.synchronize();
  CHECK(values == std::array<std::int64_t, 3>{101, 102, 103});
  table.reset();
  CHECK(fixture.memory.live() == 0);
  CHECK(fixture.memory.early_releases() == 0);
}

TEST_CASE("bulk submission waits for a synchronously blocked first file call",
          "[parquet_helpers][bulk_lifetime][gpu_execution]")
{
  materialize_fixture fixture;
  fixture.context->block_first_submission = true;
  materialize_task task{fixture};
  REQUIRE(fixture.context->await_blocked_submission());
  CHECK(fixture.context->submitted() == 1);
  CHECK(task.result.wait_for(0ms) == std::future_status::timeout);
  fixture.context->release_submission();
  REQUIRE(fixture.context->await_submissions(3));
  fixture.context->finish(2);
  fixture.context->finish(1);
  fixture.context->finish(0);
  REQUIRE(task.result.wait_for(5s) == std::future_status::ready);
  auto table = task.result.get();
  REQUIRE(table != nullptr);
  CHECK(table->num_rows() == 3);
  table.reset();
  CHECK(fixture.memory.live() == 0);
  CHECK(fixture.memory.early_releases() == 0);
}

TEST_CASE("bulk decode allocation failure occurs after every file writer settles",
          "[parquet_helpers][bulk_lifetime][gpu_execution]")
{
  materialize_fixture fixture;
  fixture.memory.fail_allocation = 4;
  materialize_task task{fixture};
  REQUIRE(fixture.context->await_submissions(3));
  REQUIRE(fixture.memory.allocations() == 3);
  fixture.context->finish(2);
  fixture.context->finish(1);
  fixture.context->finish(0);
  REQUIRE(task.result.wait_for(5s) == std::future_status::ready);
  CHECK_THROWS_AS(task.result.get(), std::bad_alloc);
  CHECK(fixture.context->submitted() == 3);
  CHECK(fixture.context->outstanding.load() == 0);
  for (std::size_t i = 0; i < 3; ++i) {
    CHECK(fixture.context->submitted_request(i).settled);
  }
  CHECK(fixture.memory.allocations() == 4);
  CHECK(fixture.memory.live() == 0);
  CHECK(fixture.memory.early_releases() == 0);
}

TEST_CASE("bulk preparation allocation failures publish no reads",
          "[parquet_helpers][bulk_lifetime][gpu_execution]")
{
  for (std::size_t ordinal = 1; ordinal <= 3; ++ordinal) {
    CAPTURE(ordinal);
    materialize_fixture fixture;
    fixture.memory.fail_allocation = ordinal;
    CHECK_THROWS_AS(fixture.materialize(), std::bad_alloc);
    CHECK(fixture.context->submitted() == 0);
    CHECK(fixture.memory.live() == 0);
  }
}

TEST_CASE("bulk preparation host allocation failures precede demand publication",
          "[.][host_allocation_fault][bulk_preparation_host_allocation_fault][gpu_execution]")
{
  REQUIRE(allocation_fault::available());
  struct outcome {
    std::exception_ptr error;
    std::size_t allocations;
    std::size_t backend_entries;
    std::size_t submitted;
    std::size_t live_destinations;
    std::size_t early_releases;
    bool fired;
  };
  auto attempt = [](std::size_t ordinal) {
    materialize_fixture fixture;
    fixture.ranges.clear();
    outcome result{};
    {
      allocation_fault fault{allocation_fault::every_allocation_scope, ordinal};
      materialize_context::preparation_probe probe{fault};
      fixture.context->probe_preparation = &probe;
      try {
        static_cast<void>(fixture.materialize());
      } catch (...) {
        result.error = std::current_exception();
      }
      result.allocations     = probe.backend_entries == 0 ? fault.stop() : probe.allocations;
      result.backend_entries = probe.backend_entries;
      result.fired           = fault.fired();
      fixture.context->probe_preparation = nullptr;
    }
    result.submitted         = fixture.context->submitted();
    result.live_destinations = fixture.memory.live();
    result.early_releases    = fixture.memory.early_releases();
    return result;
  };

  // Initialize cuDF/CUDA process-local state before measuring the repeatable preparation path.
  static_cast<void>(attempt(0));
  auto const baseline = attempt(0);
  REQUIRE(baseline.error != nullptr);
  REQUIRE(baseline.backend_entries == 3);
  REQUIRE(baseline.allocations > 0);
  REQUIRE_FALSE(baseline.fired);
  REQUIRE(baseline.submitted == 0);
  REQUIRE(baseline.live_destinations == 0);
  for (std::size_t ordinal = 1; ordinal <= baseline.allocations; ++ordinal) {
    CAPTURE(ordinal, baseline.allocations);
    auto const result = attempt(ordinal);
    REQUIRE(result.fired);
    CHECK(result.error != nullptr);
    CHECK(result.backend_entries == 0);
    CHECK(result.submitted == 0);
    CHECK(result.live_destinations == 0);
    CHECK(result.early_releases == 0);
  }
}

TEST_CASE("final bulk filters submit only surviving groups and filter-only columns",
          "[parquet_helpers][bulk_lifetime][bulk_filter][gpu_execution]")
{
  auto const all_pruned = GENERATE(false, true);
  materialize_fixture fixture{2, filtered_fixture_files()};
  fixture.options.set_column_names({"payload"});
  for (std::size_t i = 0; i < fixture.sources.size(); ++i) {
    auto const& source = fixture.sources[i];
    REQUIRE(source.row_group_indices == std::vector<cudf::size_type>{0, 1, 2, 3});
    fixture.ranges[i] = sirius::op::scan::column_chunk_ranges(
      *source.metadata, fixture.options, source.row_group_indices);
  }
  cudf::numeric_scalar<std::int64_t> limit{all_pruned ? 20000 : 12287, true, fixture.stream.view()};
  cudf::ast::literal literal{limit};
  cudf::ast::column_name_reference column{"id"};
  cudf::ast::operation filter{cudf::ast::ast_operator::GREATER, column, literal};
  fixture.options.set_filter(filter);

  materialize_task task{fixture};
  if (!all_pruned) {
    REQUIRE(fixture.context->await_submissions(1));
    auto const& request = fixture.context->submitted_request(0);
    CHECK(request.file == 1);
    auto const expected =
      sirius::op::scan::column_chunk_ranges(*fixture.sources[1].metadata, fixture.options, {2, 3});
    cudf::io::parquet_reader_options projection_only;
    projection_only.set_column_names({"payload"});
    auto const projected =
      sirius::op::scan::column_chunk_ranges(*fixture.sources[1].metadata, projection_only, {2, 3});
    REQUIRE(expected.size() > projected.size());
    // cuDF lists the filter-only column after the projected one, so no two consecutive chunks are
    // forward-adjacent and every chunk is read on its own; compare against that derived layout
    // rather than assuming it.
    auto const runs = coalesced_runs(expected, 2);
    REQUIRE(request.slices.size() == runs.size());
    for (std::size_t i = 0; i < runs.size(); ++i) {
      CHECK(request.slices[i].offset() == runs[i].first);
      CHECK(request.slices[i].size() == runs[i].second);
    }
    fixture.context->finish(0);
  }
  REQUIRE(task.result.wait_for(5s) == std::future_status::ready);
  auto table = task.result.get();
  REQUIRE(table != nullptr);
  REQUIRE(table->num_columns() == 1);
  CHECK(table->num_rows() == (all_pruned ? 0 : 4096));
  CHECK(table->view().column(0).type() == cudf::data_type{cudf::type_id::INT64});
  CHECK(fixture.context->submitted() == (all_pruned ? 0 : 1));
  CHECK(fixture.context->host_reads.load() == 0);
  CHECK(fixture.context->unexpected_reads.load() == 0);
  if (!all_pruned) {
    std::vector<std::int64_t> values(4096);
    REQUIRE(cudaMemcpyAsync(values.data(),
                            table->view().column(0).data<std::int64_t>(),
                            values.size() * sizeof(std::int64_t),
                            cudaMemcpyDeviceToHost,
                            fixture.stream.value()) == cudaSuccess);
    fixture.stream.synchronize();
    for (std::size_t i = 0; i < values.size(); ++i) {
      CHECK(values[i] == static_cast<std::int64_t>((12288 + i) * 2));
    }
  }
}

TEST_CASE("final bulk filters rebuild the multifile reader over the surviving sources",
          "[parquet_helpers][bulk_lifetime][bulk_filter][gpu_execution]")
{
  // Files hold ids 0..8191, 8192..16383 and 16384..24575; the filter prunes the first file
  // entirely, so the task must continue with a reader over the two survivors and read nothing from
  // file 0.
  materialize_fixture fixture{3, filtered_fixture_files()};
  fixture.options.set_column_names({"payload"});
  cudf::numeric_scalar<std::int64_t> limit{8191, true, fixture.stream.view()};
  cudf::ast::literal literal{limit};
  cudf::ast::column_name_reference column{"id"};
  cudf::ast::operation filter{cudf::ast::ast_operator::GREATER, column, literal};
  fixture.options.set_filter(filter);
  materialize_task task{fixture};
  REQUIRE(fixture.context->await_submissions(2));
  CHECK(fixture.context->submitted_request(0).file == 1);
  CHECK(fixture.context->submitted_request(1).file == 2);
  fixture.context->finish(1);
  fixture.context->finish(0);
  REQUIRE(task.result.wait_for(5s) == std::future_status::ready);
  auto table = task.result.get();
  REQUIRE(table != nullptr);
  REQUIRE(table->num_columns() == 1);
  REQUIRE(table->num_rows() == 16384);
  CHECK(fixture.context->submitted() == 2);
  std::vector<std::int64_t> values(16384);
  REQUIRE(cudaMemcpyAsync(values.data(),
                          table->view().column(0).data<std::int64_t>(),
                          values.size() * sizeof(std::int64_t),
                          cudaMemcpyDeviceToHost,
                          fixture.stream.value()) == cudaSuccess);
  fixture.stream.synchronize();
  for (std::size_t i = 0; i < values.size(); ++i) {
    CHECK(values[i] == static_cast<std::int64_t>((8192 + i) * 2));
  }
  table.reset();
  CHECK(fixture.memory.live() == 0);
}

TEST_CASE("a single-source final filter prunes groups and ignores the split's cached ranges",
          "[parquet_helpers][bulk_lifetime][bulk_filter][gpu_execution]")
{
  materialize_fixture fixture{1, filtered_fixture_files()};
  fixture.options.set_column_names({"payload"});
  // Cached ranges cover all four row groups and only the projected column; the task must read the
  // two surviving groups and the filter-only column instead.
  fixture.ranges[0] = sirius::op::scan::column_chunk_ranges(
    *fixture.sources[0].metadata, fixture.options, fixture.sources[0].row_group_indices);
  cudf::numeric_scalar<std::int64_t> limit{4095, true, fixture.stream.view()};
  cudf::ast::literal literal{limit};
  cudf::ast::column_name_reference column{"id"};
  cudf::ast::operation filter{cudf::ast::ast_operator::GREATER, column, literal};
  fixture.options.set_filter(filter);
  auto const expected =
    sirius::op::scan::column_chunk_ranges(*fixture.sources[0].metadata, fixture.options, {2, 3});
  REQUIRE(expected.size() == 4);
  materialize_task task{fixture};
  REQUIRE(fixture.context->await_submissions(1));
  auto const& request = fixture.context->submitted_request(0);
  CHECK(request.file == 0);
  auto const runs = coalesced_runs(expected, 2);
  REQUIRE(request.slices.size() == runs.size());
  for (std::size_t i = 0; i < runs.size(); ++i) {
    CHECK(request.slices[i].offset() == runs[i].first);
    CHECK(request.slices[i].size() == runs[i].second);
  }
  fixture.context->finish(0);
  REQUIRE(task.result.wait_for(5s) == std::future_status::ready);
  auto table = task.result.get();
  REQUIRE(table != nullptr);
  REQUIRE(table->num_columns() == 1);
  REQUIRE(table->num_rows() == 4096);
  std::vector<std::int64_t> values(4096);
  REQUIRE(cudaMemcpyAsync(values.data(),
                          table->view().column(0).data<std::int64_t>(),
                          values.size() * sizeof(std::int64_t),
                          cudaMemcpyDeviceToHost,
                          fixture.stream.value()) == cudaSuccess);
  fixture.stream.synchronize();
  for (std::size_t i = 0; i < values.size(); ++i) {
    CHECK(values[i] == static_cast<std::int64_t>((4096 + i) * 2));
  }
  table.reset();
  CHECK(fixture.memory.live() == 0);
}

TEST_CASE("bulk materialization preserves a nonzero device and explicit stream",
          "[parquet_helpers][bulk_lifetime][multi_gpu][gpu_execution]")
{
  if (rmm::get_num_cuda_devices() < 2) { SKIP("requires two visible CUDA devices"); }
  rmm::cuda_set_device_raii device{rmm::cuda_device_id{1}};
  materialize_fixture fixture{1};
  materialize_task task{fixture};
  REQUIRE(fixture.context->await_submissions(1));
  auto const& request = fixture.context->submitted_request(0);
  CHECK(request.device == rmm::cuda_device_id{1});
  CHECK(request.stream.get() == fixture.stream.value());
  REQUIRE(request.slices.size() == 1);
  cudaPointerAttributes attributes{};
  REQUIRE(cudaPointerGetAttributes(&attributes, request.slices.front().dst) == cudaSuccess);
  CHECK(attributes.device == 1);
  fixture.context->finish(0);
  REQUIRE(task.result.wait_for(5s) == std::future_status::ready);
  auto table = task.result.get();
  REQUIRE(table->num_rows() == 1);
  std::int64_t value{};
  REQUIRE(cudaMemcpyAsync(&value,
                          table->view().column(0).data<std::int64_t>(),
                          sizeof(value),
                          cudaMemcpyDeviceToHost,
                          fixture.stream.value()) == cudaSuccess);
  fixture.stream.synchronize();
  CHECK(value == 101);
  CHECK(rmm::get_current_cuda_device() == rmm::cuda_device_id{1});
}

TEST_CASE("bulk errors drain every writer and report the first source-order failure",
          "[parquet_helpers][bulk_lifetime][gpu_execution]")
{
  materialize_fixture fixture;
  materialize_task task{fixture};
  REQUIRE(fixture.context->await_submissions(3));
  fixture.context->fail(2, std::make_exception_ptr(std::runtime_error("file C failed first")));
  fixture.context->fail(0, std::make_exception_ptr(std::runtime_error("file A failed second")));
  CHECK(task.result.wait_for(100ms) == std::future_status::timeout);
  CHECK(fixture.memory.live() == 3);
  CHECK(fixture.memory.early_releases() == 0);
  fixture.context->finish(1);
  REQUIRE(task.result.wait_for(5s) == std::future_status::ready);
  CHECK_THROWS_WITH(task.result.get(), "file A failed second");
  CHECK(fixture.memory.live() == 0);
  CHECK(fixture.memory.early_releases() == 0);
}

TEST_CASE("bulk short reads fail before decode while other files are drained",
          "[parquet_helpers][bulk_lifetime][gpu_execution]")
{
  materialize_fixture fixture{2};
  materialize_task task{fixture};
  REQUIRE(fixture.context->await_submissions(2));
  fixture.context->finish(0, 0);
  CHECK(task.result.wait_for(100ms) == std::future_status::timeout);
  CHECK(fixture.memory.live() == 2);
  fixture.context->finish(1);
  REQUIRE(task.result.wait_for(5s) == std::future_status::ready);
  CHECK_THROWS_WITH(task.result.get(), "short parquet column chunk read");
  CHECK(fixture.memory.live() == 0);
  CHECK(fixture.memory.early_releases() == 0);
}

TEST_CASE("bulk skips zero-byte descriptors and rejects overflowing totals before publication",
          "[parquet_helpers][bulk_lifetime][gpu_execution]")
{
  SECTION("a zero-byte file publishes no read")
  {
    materialize_fixture fixture{2};
    fixture.ranges[0] = {byte_range{0, 0}};
    materialize_task task{fixture};
    REQUIRE(fixture.context->await_submissions(1));
    CHECK(fixture.context->submitted_request(0).file == 1);
    fixture.context->fail(0, std::make_exception_ptr(std::runtime_error("stop before decode")));
    REQUIRE(task.result.wait_for(5s) == std::future_status::ready);
    CHECK_THROWS_WITH(task.result.get(), "stop before decode");
    CHECK(fixture.context->submitted() == 1);
    CHECK(fixture.memory.live() == 0);
  }
  SECTION("padding increases capacity without increasing the logical read")
  {
    materialize_fixture fixture{1};
    fixture.ranges[0] = {byte_range{4, 5}};
    materialize_task task{fixture};
    REQUIRE(fixture.context->await_submissions(1));
    auto const& request = fixture.context->submitted_request(0);
    REQUIRE(request.slices.size() == 1);
    CHECK(request.slices.front().size() == 5);
    CHECK(fixture.memory.capacity(request.slices.front().dst) == 8);
    fixture.context->fail(0, std::make_exception_ptr(std::runtime_error("stop before decode")));
    REQUIRE(task.result.wait_for(5s) == std::future_status::ready);
    CHECK_THROWS_WITH(task.result.get(), "stop before decode");
    CHECK(fixture.memory.live() == 0);
  }
  SECTION("the complete range list is validated before allocating huge destinations")
  {
    materialize_fixture fixture{1};
    auto const maximum = std::numeric_limits<std::int64_t>::max();
    fixture.ranges[0]  = {byte_range{0, maximum}, byte_range{0, maximum}, byte_range{0, maximum}};
    CHECK_THROWS_AS(fixture.materialize(), std::overflow_error);
    CHECK(fixture.context->submitted() == 0);
    CHECK(fixture.memory.live() == 0);
  }
}

TEST_CASE("bulk packs file-contiguous chunks into one padded allocation and one read",
          "[parquet_helpers][bulk_lifetime][gpu_execution]")
{
  materialize_fixture fixture{1};
  // 4..9 and 9..13 pack into one nine-byte run, padded to sixteen. 13..17 directly precedes 17..20
  // in the file but follows it in decode order, so neither joins a run; 20..27 is not adjacent.
  fixture.ranges[0] = {
    byte_range{4, 5}, byte_range{9, 4}, byte_range{17, 3}, byte_range{13, 4}, byte_range{20, 7}};
  materialize_task task{fixture};
  REQUIRE(fixture.context->await_submissions(1));
  auto const& request = fixture.context->submitted_request(0);
  REQUIRE(request.slices.size() == 4);
  std::array<std::pair<std::size_t, std::size_t>, 4> const expected{
    {{4, 9}, {17, 3}, {13, 4}, {20, 7}}};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    CHECK(request.slices[i].offset() == expected[i].first);
    CHECK(request.slices[i].size() == expected[i].second);
    CHECK(fixture.memory.capacity(request.slices[i].dst) ==
          ((expected[i].second + 7) & ~std::size_t{7}));
  }
  CHECK(fixture.memory.allocations() == 4);
  fixture.context->fail(0, std::make_exception_ptr(std::runtime_error("stop before decode")));
  REQUIRE(task.result.wait_for(5s) == std::future_status::ready);
  CHECK_THROWS_WITH(task.result.get(), "stop before decode");
  CHECK(fixture.memory.live() == 0);
}

TEST_CASE("bulk runs stop at row-group boundaries and decode through packed spans",
          "[parquet_helpers][bulk_lifetime][gpu_execution]")
{
  // The uncompressed fixture lays every chunk directly after the previous one, across row groups
  // too. Two row groups of two columns must therefore give two runs, not one file-sized run, and
  // each run's second chunk must decode from its packed position.
  materialize_fixture fixture{1, filtered_fixture_files()};
  fixture.options.set_column_names({"id", "payload"});
  fixture.sources[0].row_group_indices = {0, 1};
  fixture.ranges[0]                    = sirius::op::scan::column_chunk_ranges(
    *fixture.sources[0].metadata, fixture.options, fixture.sources[0].row_group_indices);
  REQUIRE(fixture.ranges[0].size() == 4);
  for (std::size_t i = 1; i < 4; ++i) {
    REQUIRE(fixture.ranges[0][i].offset() ==
            fixture.ranges[0][i - 1].offset() + fixture.ranges[0][i - 1].size());
  }
  materialize_task task{fixture};
  REQUIRE(fixture.context->await_submissions(1));
  auto const& request = fixture.context->submitted_request(0);
  REQUIRE(request.slices.size() == 2);
  for (std::size_t group = 0; group < 2; ++group) {
    auto const& first  = fixture.ranges[0][2 * group];
    auto const& second = fixture.ranges[0][2 * group + 1];
    CHECK(request.slices[group].offset() == static_cast<std::size_t>(first.offset()));
    CHECK(request.slices[group].size() == static_cast<std::size_t>(first.size() + second.size()));
  }
  CHECK(fixture.memory.allocations() == 2);
  fixture.context->finish(0);
  REQUIRE(task.result.wait_for(5s) == std::future_status::ready);
  auto table = task.result.get();
  REQUIRE(table != nullptr);
  REQUIRE(table->num_columns() == 2);
  REQUIRE(table->num_rows() == 4096);
  std::vector<std::int64_t> ids(4096);
  std::vector<std::int64_t> payloads(4096);
  REQUIRE(cudaMemcpyAsync(ids.data(),
                          table->view().column(0).data<std::int64_t>(),
                          ids.size() * sizeof(std::int64_t),
                          cudaMemcpyDeviceToHost,
                          fixture.stream.value()) == cudaSuccess);
  REQUIRE(cudaMemcpyAsync(payloads.data(),
                          table->view().column(1).data<std::int64_t>(),
                          payloads.size() * sizeof(std::int64_t),
                          cudaMemcpyDeviceToHost,
                          fixture.stream.value()) == cudaSuccess);
  fixture.stream.synchronize();
  for (std::size_t i = 0; i < ids.size(); ++i) {
    CHECK(ids[i] == static_cast<std::int64_t>(i));
    CHECK(payloads[i] == static_cast<std::int64_t>(2 * i));
  }
  table.reset();
  CHECK(fixture.memory.live() == 0);
  CHECK(fixture.memory.early_releases() == 0);
}

TEST_CASE("bulk file-B submission allocation failure retains A and wins over its error",
          "[.][host_allocation_fault][bulk_host_allocation_fault][gpu_execution]")
{
  REQUIRE(allocation_fault::available());
  materialize_fixture fixture;
  materialize_task task{fixture, true};
  REQUIRE(fixture.context->await_submissions(1));
  CHECK(task.result.wait_for(100ms) == std::future_status::timeout);
  CHECK(fixture.memory.live() == 3);
  CHECK(fixture.memory.early_releases() == 0);
  fixture.context->fail(0, std::make_exception_ptr(std::runtime_error("file A also failed")));
  REQUIRE(task.result.wait_for(5s) == std::future_status::ready);
  CHECK_THROWS_AS(task.result.get(), std::bad_alloc);
  CHECK(fixture.context->submitted() == 1);
  CHECK(fixture.memory.live() == 0);
  CHECK(fixture.memory.early_releases() == 0);
}

namespace {

class stream_gate {
 public:
  explicit stream_gate(::cuda::stream_ref stream) : _stream{stream}
  {
    auto const status = cudaLaunchHostFunc(
      stream.get(),
      [](void* opaque) {
        auto& self = *static_cast<stream_gate*>(opaque);
        self._entered.release();
        self._release.acquire();
      },
      this);
    if (status != cudaSuccess) { throw std::runtime_error(cudaGetErrorString(status)); }
  }

  ~stream_gate()
  {
    release();
    static_cast<void>(cudaStreamSynchronize(_stream.get()));
  }

  stream_gate(stream_gate const&)            = delete;
  stream_gate& operator=(stream_gate const&) = delete;

  bool await_entry() { return _entered.try_acquire_for(5s); }

  void release() noexcept
  {
    if (!_released.exchange(true)) { _release.release(); }
  }

 private:
  ::cuda::stream_ref _stream;
  std::binary_semaphore _entered{0};
  std::binary_semaphore _release{0};
  std::atomic<bool> _released{false};
};

struct cache_read_fixture {
  cache_read_fixture()
    : memory{initialize_memory_manager(1)}, context{std::make_shared<controlled_context>(1, [] {
        return std::make_unique<controlled_reactor>();
      })}
  {
    sirius::io::cache::config config;
    config.mode = sirius::io::cache::cache_mode::sirius;
    config.apply_mode();
    context->initialize_cache(*memory, config, single_gpu_topology());
    source = context->open_datasource("controlled://bulk-completion");
  }

  ~cache_read_fixture()
  {
    source.reset();
    context->shutdown_cache();
  }

  std::unique_ptr<sirius::memory::sirius_memory_reservation_manager> memory;
  std::shared_ptr<controlled_context> context;
  std::unique_ptr<sirius::io::sirius_datasource> source;
};

class held_backend_request {
 public:
  explicit held_backend_request(std::unique_ptr<sirius::io::grouped_io_request> value)
    : request{std::move(value)},
      _error{std::make_exception_ptr(std::runtime_error("held request teardown"))}
  {
  }

  ~held_backend_request()
  {
    if (request != nullptr) { complete_failure(*request, _error); }
  }

  std::unique_ptr<sirius::io::grouped_io_request> request;

 private:
  std::exception_ptr _error;
};

}  // namespace

TEST_CASE("a mixed cached and backend file read cannot fail before both writers stop",
          "[parquet_helpers][bulk_lifetime][cache][read-arbitration][gpu_execution]")
{
  cache_read_fixture fixture;
  REQUIRE(fixture.context->cache() != nullptr);
  std::array<byte_range, 1> cached_range{byte_range{0, 64}};
  fixture.source->fadvise(cached_range, 0);
  REQUIRE(fixture.source->prepare_prefetch(false) == sirius::io::prepare_result::prepared);
  REQUIRE(fixture.source->prefetch_async([](sirius::io::cache::prefetch_result) noexcept {}) ==
          sirius::io::prefetch_refusal::issued);
  held_backend_request prefetch{fixture.context->reactor().take_next()};
  REQUIRE(prefetch.request != nullptr);
  complete_success(*prefetch.request);

  rmm::cuda_stream stream;
  rmm::device_buffer cached_destination{64, stream};
  rmm::device_buffer backend_destination{64, stream};
  std::array<sirius::io::slice, 2> slices{
    sirius::io::slice{0, 64, static_cast<std::uint8_t*>(cached_destination.data())},
    sirius::io::slice{2 * chunk_size, 64, static_cast<std::uint8_t*>(backend_destination.data())}};

  SECTION("backend failure does not retire a held cached copy")
  {
    stream_gate gate{stream};
    REQUIRE(gate.await_entry());
    auto read = fixture.source->device_read_ranges_async(slices, stream);
    held_backend_request backend{fixture.context->reactor().take_next()};
    REQUIRE(backend.request != nullptr);
    complete_failure(*backend.request,
                     std::make_exception_ptr(std::runtime_error("backend failed")));
    CHECK(read.wait_for(0ms) == std::future_status::timeout);
    gate.release();
    REQUIRE(read.wait_for(5s) == std::future_status::ready);
    CHECK_THROWS_WITH(read.get(), "backend failed");
  }

  SECTION("cached completion does not retire a held backend writer")
  {
    auto read = fixture.source->device_read_ranges_async(slices, stream);
    held_backend_request backend{fixture.context->reactor().take_next()};
    REQUIRE(backend.request != nullptr);
    stream.synchronize();
    CHECK(read.wait_for(0ms) == std::future_status::timeout);
    complete_failure(*backend.request,
                     std::make_exception_ptr(std::runtime_error("backend failed last")));
    REQUIRE(read.wait_for(5s) == std::future_status::ready);
    CHECK_THROWS_WITH(read.get(), "backend failed last");
  }
}
