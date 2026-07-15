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

// #819 PR2-lite: hardening gates for the coalescer-direct cached serving path.
//
// Two failure modes on this path used to be silent:
//   - a throwing databatch_provider escaped into the dispatcher (which
//     swallows task exceptions), leaving the operator's split_connector never
//     closed — the consumer blocked in get_next_split() forever (silent query
//     hang);
//   - a malformed pinned entry (per-column chunk counts disagreeing, short
//     chunk_memory_spaces, null chunks) made the cached provider return
//     nullptr mid-stream, which the drain loop reads as end-of-stream — the
//     query completed on FEWER rows than requested (silent truncation).
//
// Gates: load_balancing_scan_batch_coalescer::drain_cached_provider
// (forward-then-close; provider throw -> close(exception) -> consumer
// rethrows; pre-stopped token -> close without draining) and
// validate_pinned_entry_for_serving (malformed entries throw so
// try_assign_cached_entries falls back to the disk read; well-formed and
// zero-chunk entries pass).

#include "operator/operator_test_utils.hpp"

#include <cudf/column/column.hpp>
#include <cudf/column/column_factories.hpp>
#include <cudf/table/table.hpp>
#include <cudf/table/table_view.hpp>

#include <rmm/cuda_stream.hpp>

#include <cuda_runtime.h>

#include <catch.hpp>
#include <compression/compressed_representation.hpp>
#include <cucascade/cudf/gpu_data_representation.hpp>
#include <cucascade/cudf/host_data_representation.hpp>
#include <cucascade/data/data_batch.hpp>
#include <cucascade/memory/memory_space.hpp>
#include <data/data_batch_utils.hpp>
#include <data/sirius_converter_registry.hpp>
#include <memory/topology_index.hpp>
#include <op/scan/sirius_gpu_scan_operator_data.hpp>
#include <scan_manager/load_balancing_scan_batch_coalescer.hpp>
#include <scan_manager/pinned_chunk_source.hpp>
#include <scan_manager/sirius_scan_manager.hpp>
#include <scan_manager/split_connector.hpp>
#include <telemetry/data_batch_probe.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using sirius::scan_manager::cache_entry_info;
using sirius::scan_manager::chunk_kind;
using sirius::scan_manager::databatch_provider;
using sirius::scan_manager::device_pinned_chunks;
using sirius::scan_manager::load_balancing_scan_batch_coalescer;
using sirius::scan_manager::make_provider_for_pinned_entry;
using sirius::scan_manager::pinned_entry;
using sirius::scan_manager::sirius_scan_manager;
using sirius::scan_manager::split_connector;
using sirius::scan_manager::validate_pinned_entry_for_serving;

namespace {

// Shared test environment: memory manager (+ converter registry) initialized
// once for every gate in this file. The converter needs a non-default stream
// (same constraint test_convertible_data_batch documents).
struct test_env {
  std::unique_ptr<sirius::memory::sirius_memory_reservation_manager> mgr;
  cucascade::memory::memory_space* gpu_space;
  cucascade::memory::memory_space* host_space;
  rmm::cuda_stream conv_stream;

  test_env()
    : mgr(sirius::test::operator_utils::initialize_memory_manager()),
      gpu_space(mgr->get_memory_space(cucascade::memory::Tier::GPU, 0)),
      host_space(mgr->get_memory_space(cucascade::memory::Tier::HOST, 0)),
      conv_stream()
  {
  }

  rmm::cuda_stream_view stream() { return conv_stream.view(); }
};

test_env& env()
{
  static test_env e;
  return e;
}

std::shared_ptr<const sirius::memory::topology_index> single_gpu_index()
{
  cucascade::memory::system_topology_info topology;
  topology.num_gpus = 1;
  cucascade::memory::gpu_topology_info gpu;
  gpu.id        = 0;
  gpu.numa_node = 0;
  topology.gpus.push_back(std::move(gpu));
  return std::make_shared<sirius::memory::topology_index>(std::move(topology), std::vector<int>{0});
}

void set_cache_columns(pinned_entry& entry, std::vector<std::string> names)
{
  entry.cache_info.names = std::move(names);
  entry.cache_info.column_ids.clear();
  for (std::size_t index = 0; index < entry.cache_info.names.size(); ++index) {
    entry.cache_info.column_ids.emplace_back(duckdb::ColumnIndex(index));
  }
}

cache_entry_info make_cache_info(std::vector<std::string> names)
{
  cache_entry_info info;
  info.names = std::move(names);
  for (std::size_t index = 0; index < info.names.size(); ++index) {
    info.column_ids.emplace_back(duckdb::ColumnIndex(index));
  }
  return info;
}

/// Deterministic per-cell value so a chunk/column/row mixup fails loudly.
int32_t cell(std::size_t chunk, std::size_t col, std::size_t row)
{
  return static_cast<int32_t>(1000 * chunk + 100 * col + row);
}

std::shared_ptr<cudf::column> make_gpu_column(cucascade::memory::memory_space& space,
                                              std::vector<int32_t> const& values)
{
  auto mr     = sirius::test::operator_utils::get_resource_ref(space);
  auto stream = sirius::test::operator_utils::default_stream();
  auto col    = cudf::make_numeric_column(cudf::data_type{cudf::type_id::INT32},
                                       static_cast<cudf::size_type>(values.size()),
                                       cudf::mask_state::UNALLOCATED,
                                       stream,
                                       mr);
  if (cudaMemcpy(col->mutable_view().data<int32_t>(),
                 values.data(),
                 sizeof(int32_t) * values.size(),
                 cudaMemcpyHostToDevice) != cudaSuccess) {
    throw std::runtime_error("make_gpu_column: host-to-device copy failed");
  }
  return std::shared_ptr<cudf::column>(std::move(col));
}

std::unique_ptr<cudf::table> make_gpu_table(cucascade::memory::memory_space& space,
                                            std::size_t rows)
{
  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(cudf::make_numeric_column(cudf::data_type{cudf::type_id::INT32},
                                              static_cast<cudf::size_type>(rows),
                                              cudf::mask_state::UNALLOCATED,
                                              sirius::test::operator_utils::default_stream(),
                                              space.get_default_allocator()));
  return std::make_unique<cudf::table>(std::move(columns));
}

/// GPU-tier pinned entry with columns {k, v, w} x @p n_chunks chunks of
/// @p rows rows, every chunk placed in @p space.
pinned_entry make_gpu_entry(cucascade::memory::memory_space& space,
                            std::size_t n_chunks,
                            std::size_t rows)
{
  pinned_entry entry;
  set_cache_columns(entry, {"k", "v", "w"});
  entry.tier         = cucascade::memory::Tier::GPU;
  entry.memory_space = &space;
  for (std::size_t c = 0; c < n_chunks; ++c) {
    entry.chunk_memory_spaces.push_back(&space);
    for (std::size_t col = 0; col < entry.cache_info.names.size(); ++col) {
      std::vector<int32_t> values(rows);
      for (std::size_t r = 0; r < rows; ++r) {
        values[r] = cell(c, col, r);
      }
      entry.data_batches_by_column[entry.cache_info.names[col]].push_back(
        make_gpu_column(space, values));
    }
  }
  entry.num_rows = n_chunks * rows;
  return entry;
}

/// HOST-tier pinned entry with columns {k, v} x @p n_chunks chunks, built the
/// way the pin path builds them (GPU table -> converter -> host chunk).
pinned_entry make_host_entry(test_env& e, std::size_t n_chunks, std::size_t rows)
{
  pinned_entry entry;
  set_cache_columns(entry, {"k", "v"});
  entry.tier         = cucascade::memory::Tier::HOST;
  entry.memory_space = e.host_space;

  auto& registry = sirius::converter_registry::get();
  for (std::size_t c = 0; c < n_chunks; ++c) {
    std::vector<std::unique_ptr<cudf::column>> cols;
    for (std::size_t col = 0; col < entry.cache_info.names.size(); ++col) {
      std::vector<int32_t> values(rows);
      for (std::size_t r = 0; r < rows; ++r) {
        values[r] = cell(c, col, r);
      }
      auto shared = make_gpu_column(*e.gpu_space, values);
      cols.push_back(std::make_unique<cudf::column>(
        shared->view(), e.stream(), e.gpu_space->get_default_allocator()));
    }
    cucascade::gpu_table_representation gpu_repr(
      std::make_unique<cudf::table>(std::move(cols)), *e.gpu_space, e.stream());
    auto host_repr =
      registry.convert<cucascade::host_data_representation>(gpu_repr, e.host_space, e.stream());
    e.stream().synchronize();
    entry.host_chunks.emplace_back(std::move(host_repr));
  }
  entry.num_rows = n_chunks * rows;
  return entry;
}

/// One-column resident GPU batch — payload for the fake providers below.
std::shared_ptr<cucascade::data_batch> make_test_batch(test_env& e, std::size_t rows)
{
  auto col = make_gpu_column(*e.gpu_space, std::vector<int32_t>(rows, 7));
  std::vector<std::shared_ptr<cudf::column>> columns{col};
  std::vector<cudf::column_view> views{col->view()};
  auto const alloc_size = col->alloc_size();
  auto repr             = std::make_unique<cucascade::gpu_table_representation>(
    cudf::table_view(views), std::move(columns), alloc_size, *e.gpu_space, rmm::cuda_stream_view{});
  return cucascade::data_batch::make(sirius::get_next_batch_id(), std::move(repr));
}

/// Serves its scripted batches in order, then either ends the stream
/// (nullptr) or throws — the two provider behaviors the drain must handle.
struct scripted_provider final : databatch_provider {
  std::vector<std::shared_ptr<cucascade::data_batch>> batches;
  std::size_t served{0};
  bool throw_when_exhausted{false};

  std::shared_ptr<cucascade::data_batch> get_next_batch() override
  {
    if (served < batches.size()) { return batches[served++]; }
    if (throw_when_exhausted) { throw std::runtime_error("provider blew up mid-stream"); }
    return nullptr;
  }
};

//===----------------------------------------------------------------------===//
// Mixed-entry (E0) builders + serving harness
//===----------------------------------------------------------------------===//
//
// A pin may compress some batches and leave others raw. The provider must then
// walk pinned_entry::logical_order and serve each logical chunk from its arm at
// its dense slot. These helpers build a mixed entry with a DISTINCT row-count
// signal per slot so a served batch reveals both the arm it came from (its
// concrete representation type) and the exact slot within that arm (its row
// count / num_rows sentinel) — catching a wrong-arm or wrong-slot dispatch.

constexpr std::size_t kRawBaseRows   = 4;     // raw slot i  -> kRawBaseRows + i rows
constexpr std::int64_t kCompBaseRows = 5000;  // comp slot j -> kCompBaseRows + j num_rows()

/// Cheap compressed chunk: valid column_names + a distinct num_rows() sentinel,
/// empty backing blob. The serving path only calls select_columns() (which shares
/// the blob and reads column_names) — it never decompresses — so an empty blob
/// exercises the mixed dispatch fully.
std::shared_ptr<sirius::compressed_host_representation> make_host_compressed(
  cucascade::memory::memory_space& space, std::vector<std::string> const& names, std::int64_t rows)
{
  return std::make_shared<sirius::compressed_host_representation>(
    space, std::make_shared<sirius::pinned_compressed_blob>(), names, 0, 0, rows);
}

std::shared_ptr<sirius::compressed_device_representation> make_device_compressed(
  cucascade::memory::memory_space& space, std::vector<std::string> const& names, std::int64_t rows)
{
  return std::make_shared<sirius::compressed_device_representation>(
    space, std::make_shared<sirius::device_compressed_blob>(), names, 0, 0, rows);
}

/// One raw host chunk of @p rows rows over @p names, built the way the pin path
/// builds them (GPU table -> converter -> host chunk).
std::shared_ptr<cucascade::host_data_representation> make_host_raw_chunk(
  test_env& e, std::vector<std::string> const& names, std::size_t slot, std::size_t rows)
{
  auto& registry = sirius::converter_registry::get();
  std::vector<std::unique_ptr<cudf::column>> cols;
  for (std::size_t col = 0; col < names.size(); ++col) {
    std::vector<int32_t> values(rows);
    for (std::size_t r = 0; r < rows; ++r) {
      values[r] = cell(slot, col, r);
    }
    auto shared = make_gpu_column(*e.gpu_space, values);
    cols.push_back(std::make_unique<cudf::column>(
      shared->view(), e.stream(), e.gpu_space->get_default_allocator()));
  }
  cucascade::gpu_table_representation gpu_repr(
    std::make_unique<cudf::table>(std::move(cols)), *e.gpu_space, e.stream());
  auto host_repr =
    registry.convert<cucascade::host_data_representation>(gpu_repr, e.host_space, e.stream());
  e.stream().synchronize();
  return host_repr;
}

/// GPU mixed entry over columns {k,v,w}: @p layout gives each logical chunk's
/// scan-order kind; both arms are kept dense and logical_order records the
/// interleaving.
pinned_entry make_mixed_gpu_entry(test_env& e, std::vector<chunk_kind> const& layout)
{
  pinned_entry entry;
  set_cache_columns(entry, {"k", "v", "w"});
  entry.tier           = cucascade::memory::Tier::GPU;
  entry.memory_space   = e.gpu_space;
  std::size_t raw_slot = 0, comp_slot = 0;
  for (auto kind : layout) {
    if (kind == chunk_kind::raw) {
      std::size_t const rows = kRawBaseRows + raw_slot;
      entry.chunk_memory_spaces.push_back(e.gpu_space);
      for (std::size_t col = 0; col < entry.cache_info.names.size(); ++col) {
        std::vector<int32_t> values(rows);
        for (std::size_t r = 0; r < rows; ++r) {
          values[r] = cell(raw_slot, col, r);
        }
        entry.data_batches_by_column[entry.cache_info.names[col]].push_back(
          make_gpu_column(*e.gpu_space, values));
      }
      entry.num_rows += rows;
      entry.logical_order.push_back({chunk_kind::raw, raw_slot});
      ++raw_slot;
    } else {
      entry.compressed_device_chunks.push_back(
        make_device_compressed(*e.gpu_space,
                               entry.cache_info.names,
                               kCompBaseRows + static_cast<std::int64_t>(comp_slot)));
      entry.num_rows += static_cast<std::size_t>(kCompBaseRows + comp_slot);
      entry.logical_order.push_back({chunk_kind::compressed, comp_slot});
      ++comp_slot;
    }
  }
  return entry;
}

/// HOST mixed entry over columns {k,v}, analogous to make_mixed_gpu_entry.
pinned_entry make_mixed_host_entry(test_env& e, std::vector<chunk_kind> const& layout)
{
  pinned_entry entry;
  set_cache_columns(entry, {"k", "v"});
  entry.tier           = cucascade::memory::Tier::HOST;
  entry.memory_space   = e.host_space;
  std::size_t raw_slot = 0, comp_slot = 0;
  for (auto kind : layout) {
    if (kind == chunk_kind::raw) {
      entry.host_chunks.push_back(
        make_host_raw_chunk(e, entry.cache_info.names, raw_slot, kRawBaseRows + raw_slot));
      entry.num_rows += kRawBaseRows + raw_slot;
      entry.logical_order.push_back({chunk_kind::raw, raw_slot});
      ++raw_slot;
    } else {
      entry.compressed_host_chunks.push_back(
        make_host_compressed(*e.host_space,
                             entry.cache_info.names,
                             kCompBaseRows + static_cast<std::int64_t>(comp_slot)));
      entry.num_rows += static_cast<std::size_t>(kCompBaseRows + comp_slot);
      entry.logical_order.push_back({chunk_kind::compressed, comp_slot});
      ++comp_slot;
    }
  }
  return entry;
}

/// Drive @p provider to exhaustion, returning a read-only accessor per served
/// batch in order. Each accessor retains a shared_ptr to its batch and a shared
/// lock, so the representations stay valid for the lifetime of the returned vector.
std::vector<cucascade::read_only_data_batch> drain_provider(databatch_provider& provider)
{
  std::vector<cucascade::read_only_data_batch> out;
  while (auto batch = provider.get_next_batch()) {
    out.push_back(batch->to_read_only());
  }
  return out;
}

/// Assert the served sequence matches @p layout arm-for-arm and slot-for-slot.
void verify_mixed_serving(std::vector<chunk_kind> const& layout,
                          std::vector<cucascade::read_only_data_batch> const& served,
                          bool gpu)
{
  REQUIRE(served.size() == layout.size());
  std::size_t raw_slot = 0, comp_slot = 0;
  for (std::size_t i = 0; i < layout.size(); ++i) {
    INFO("logical position " << i);
    const cucascade::idata_representation* repr = served[i].get_data();
    if (layout[i] == chunk_kind::raw) {
      auto const expected_rows = static_cast<std::int64_t>(kRawBaseRows + raw_slot);
      if (gpu) {
        auto const* r = dynamic_cast<const cucascade::gpu_table_representation*>(repr);
        REQUIRE(r != nullptr);
        REQUIRE(static_cast<std::int64_t>(r->get_table_view().num_rows()) == expected_rows);
      } else {
        auto const* r = dynamic_cast<const cucascade::host_data_representation*>(repr);
        REQUIRE(r != nullptr);
        REQUIRE(static_cast<std::int64_t>(r->get_host_table()->columns.front().num_rows) ==
                expected_rows);
      }
      ++raw_slot;
    } else {
      auto const expected_rows = kCompBaseRows + static_cast<std::int64_t>(comp_slot);
      if (gpu) {
        auto const* c = dynamic_cast<const sirius::compressed_device_representation*>(repr);
        REQUIRE(c != nullptr);
        REQUIRE(c->num_rows() == expected_rows);
      } else {
        auto const* c = dynamic_cast<const sirius::compressed_host_representation*>(repr);
        REQUIRE(c != nullptr);
        REQUIRE(c->num_rows() == expected_rows);
      }
      ++comp_slot;
    }
  }
}

}  // namespace

//===----------------------------------------------------------------------===//
// drain_cached_provider gates
//===----------------------------------------------------------------------===//

TEST_CASE("drain_cached_provider forwards every batch then closes",
          "[cached_serving][scan_manager]")
{
  auto& e = env();
  scripted_provider provider;
  provider.batches = {make_test_batch(e, 4), make_test_batch(e, 4), make_test_batch(e, 4)};

  split_connector connector;
  std::stop_source stop;
  load_balancing_scan_batch_coalescer::drain_cached_provider(provider, connector, stop.get_token());

  for (int i = 0; i < 3; ++i) {
    auto split = connector.get_next_split();
    REQUIRE(split.has_value());
    auto* input = dynamic_cast<sirius::op::scan::scan_operator_input*>(split->get());
    REQUIRE(input != nullptr);
    REQUIRE(input->is_resident());
  }
  REQUIRE_FALSE(connector.get_next_split().has_value());  // closed and drained
}

TEST_CASE("drain_cached_provider surfaces a provider exception instead of hanging",
          "[cached_serving][scan_manager]")
{
  auto& e = env();
  scripted_provider provider;
  provider.batches              = {make_test_batch(e, 4)};
  provider.throw_when_exhausted = true;

  split_connector connector;
  std::stop_source stop;
  // Must not propagate: the dispatcher would swallow it and leave the
  // connector open forever (the old silent-hang bug).
  REQUIRE_NOTHROW(load_balancing_scan_batch_coalescer::drain_cached_provider(
    provider, connector, stop.get_token()));

  // The stored error takes precedence over queued splits: every consumer
  // pull now rethrows the producer failure (instead of a partial stream
  // followed by an eternal block — the old silent-hang bug).
  REQUIRE_THROWS_AS(connector.get_next_split(), std::runtime_error);
  REQUIRE_THROWS_AS(connector.get_next_split(), std::runtime_error);
}

TEST_CASE("drain_cached_provider honors a pre-stopped token", "[cached_serving][scan_manager]")
{
  auto& e = env();
  scripted_provider provider;
  provider.batches = {make_test_batch(e, 4), make_test_batch(e, 4)};

  split_connector connector;
  std::stop_source stop;
  stop.request_stop();
  load_balancing_scan_batch_coalescer::drain_cached_provider(provider, connector, stop.get_token());

  REQUIRE(provider.served == 0);                          // nothing pulled after stop
  REQUIRE_FALSE(connector.get_next_split().has_value());  // still closed: consumer unblocked
}

//===----------------------------------------------------------------------===//
// validate_pinned_entry_for_serving gates
//===----------------------------------------------------------------------===//

TEST_CASE("validate_pinned_entry_for_serving accepts well-formed and zero-chunk entries",
          "[cached_serving][scan_manager]")
{
  auto& e = env();

  SECTION("well-formed GPU entry")
  {
    auto entry = make_gpu_entry(*e.gpu_space, 3, 4);
    REQUIRE_NOTHROW(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0, 1, 2}));
  }

  SECTION("zero-chunk GPU entry")
  {
    pinned_entry entry;
    set_cache_columns(entry, {"k"});
    entry.tier = cucascade::memory::Tier::GPU;
    REQUIRE_NOTHROW(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0}));
  }

  SECTION("well-formed HOST entry")
  {
    auto entry = make_host_entry(e, 2, 4);
    REQUIRE_NOTHROW(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0, 1}));
  }
}

TEST_CASE("validate_pinned_entry_for_serving refuses malformed entries",
          "[cached_serving][scan_manager]")
{
  auto& e = env();

  SECTION("per-column chunk counts disagree")
  {
    auto entry = make_gpu_entry(*e.gpu_space, 3, 4);
    entry.data_batches_by_column["v"].pop_back();  // v now has 2 chunks, k/w have 3
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0, 1, 2}),
                      std::runtime_error);
  }

  SECTION("selected column missing from the entry's storage")
  {
    auto entry = make_gpu_entry(*e.gpu_space, 2, 4);
    entry.data_batches_by_column.erase("w");
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{2}),
                      std::runtime_error);
  }

  SECTION("chunk_memory_spaces does not cover every chunk")
  {
    auto entry = make_gpu_entry(*e.gpu_space, 3, 4);
    entry.chunk_memory_spaces.resize(1);
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0}),
                      std::runtime_error);
  }

  SECTION("null chunk memory_space")
  {
    auto entry                   = make_gpu_entry(*e.gpu_space, 2, 4);
    entry.chunk_memory_spaces[1] = nullptr;
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0}),
                      std::runtime_error);
  }

  SECTION("non-empty entry requires a representative memory space")
  {
    auto entry         = make_gpu_entry(*e.gpu_space, 2, 4);
    entry.memory_space = nullptr;
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0}),
                      std::runtime_error);
  }

  SECTION("representative memory space must match the declared tier")
  {
    auto entry         = make_gpu_entry(*e.gpu_space, 2, 4);
    entry.memory_space = e.host_space;
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0}),
                      std::runtime_error);
  }

  SECTION("raw GPU placements must actually be GPU tier")
  {
    auto entry                   = make_gpu_entry(*e.gpu_space, 2, 4);
    entry.chunk_memory_spaces[1] = e.host_space;
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0}),
                      std::runtime_error);
  }

  SECTION("null GPU chunk")
  {
    auto entry                           = make_gpu_entry(*e.gpu_space, 2, 4);
    entry.data_batches_by_column["k"][1] = nullptr;
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0}),
                      std::runtime_error);
  }

  SECTION("raw GPU columns disagree on a chunk row boundary")
  {
    auto entry = make_gpu_entry(*e.gpu_space, 2, 4);
    entry.data_batches_by_column["v"][1] =
      make_gpu_column(*e.gpu_space, std::vector<int32_t>(3, 0));
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0, 1}),
                      std::runtime_error);
  }

  SECTION("stored row total disagrees with num_rows")
  {
    auto entry = make_gpu_entry(*e.gpu_space, 2, 4);
    --entry.num_rows;
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0}),
                      std::runtime_error);
  }

  SECTION("DuckDB MVCC counts disagree with storage boundaries at the same total")
  {
    auto entry                    = make_gpu_entry(*e.gpu_space, 2, 4);
    entry.cache_info.catalog_name = "memory";
    entry.cache_info.schema_name  = "main";
    entry.cache_info.table_name   = "mvcc_rows";
    entry.mvcc                    = std::make_unique<sirius::scan_manager::duckdb_mvcc_metadata>();
    entry.mvcc->v_base            = 7;
    entry.mvcc->base_row_count_per_chunk = {3, 5};
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0}),
                      std::runtime_error);
  }

  SECTION("null host chunk")
  {
    auto entry           = make_host_entry(e, 2, 4);
    entry.host_chunks[1] = nullptr;
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0}),
                      std::runtime_error);
  }
}

//===----------------------------------------------------------------------===//
// validate_pinned_entry_for_serving: mixed-entry logical_order gates (E0)
//===----------------------------------------------------------------------===//

TEST_CASE("validate_pinned_entry_for_serving accepts a well-formed mixed entry",
          "[cached_serving][scan_manager][mixed]")
{
  auto& e = env();
  std::vector<chunk_kind> const layout{
    chunk_kind::compressed, chunk_kind::raw, chunk_kind::raw, chunk_kind::compressed};

  SECTION("GPU tier")
  {
    auto entry = make_mixed_gpu_entry(e, layout);
    REQUIRE_NOTHROW(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0, 1, 2}));
  }
  SECTION("HOST tier")
  {
    auto entry = make_mixed_host_entry(e, layout);
    REQUIRE_NOTHROW(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0, 1}));
  }
}

TEST_CASE("validate_pinned_entry_for_serving refuses a malformed logical_order",
          "[cached_serving][scan_manager][mixed]")
{
  auto& e = env();
  std::vector<chunk_kind> const layout{
    chunk_kind::raw, chunk_kind::compressed, chunk_kind::raw, chunk_kind::compressed};

  SECTION("logical_order size does not cover both arms")
  {
    auto entry = make_mixed_gpu_entry(e, layout);
    entry.logical_order.pop_back();  // 3 slots but arms hold 2 raw + 2 compressed
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0, 1, 2}),
                      std::runtime_error);
  }

  SECTION("logical_order references an out-of-range arm slot")
  {
    auto entry                       = make_mixed_gpu_entry(e, layout);
    entry.logical_order[0].arm_index = 99;  // raw arm has only 2 slots
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0, 1, 2}),
                      std::runtime_error);
  }

  SECTION("logical_order references the same slot twice (dropping another)")
  {
    auto entry = make_mixed_host_entry(e, layout);
    // Point the second raw slot at raw index 0: raw 0 is served twice, raw 1 never.
    // Size still matches, so only the one-to-one check catches it.
    entry.logical_order[2] = {chunk_kind::raw, 0};
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0, 1}),
                      std::runtime_error);
  }

  SECTION("null compressed device chunk in a mixed entry")
  {
    // A null compressed chunk would serve as end-of-stream mid-scan (silent
    // truncation) — validate must reject it like a null raw chunk.
    auto entry                        = make_mixed_gpu_entry(e, layout);
    entry.compressed_device_chunks[0] = nullptr;
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0, 1, 2}),
                      std::runtime_error);
  }

  SECTION("null compressed host chunk in a mixed entry")
  {
    auto entry                      = make_mixed_host_entry(e, layout);
    entry.compressed_host_chunks[0] = nullptr;
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0, 1}),
                      std::runtime_error);
  }

  SECTION("both populated arms require a logical_order")
  {
    auto entry = make_mixed_gpu_entry(e, layout);
    entry.logical_order.clear();
    std::vector<std::size_t> selected{0, 1, 2};
    REQUIRE_THROWS_AS(
      make_provider_for_pinned_entry(entry, selected, sirius::telemetry::batch_telemetry_info{}),
      std::runtime_error);
  }

  SECTION("logical_order is reserved for genuinely mixed entries")
  {
    auto entry          = make_gpu_entry(*e.gpu_space, 2, 4);
    entry.logical_order = {{chunk_kind::raw, 0}, {chunk_kind::raw, 1}};
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0}),
                      std::runtime_error);
  }

  SECTION("GPU raw placements cannot exist without raw columns")
  {
    auto entry = make_mixed_gpu_entry(e, layout);
    entry.data_batches_by_column.clear();
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0}),
                      std::runtime_error);
  }

  SECTION("compressed schemas must match cache order exactly")
  {
    auto entry = make_mixed_gpu_entry(e, layout);
    entry.compressed_device_chunks[0] =
      make_device_compressed(*e.gpu_space, {"v", "k", "w"}, kCompBaseRows);
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0, 1, 2}),
                      std::runtime_error);
  }

  SECTION("compressed chunk memory space must match its tier")
  {
    auto entry = make_mixed_gpu_entry(e, layout);
    entry.compressed_device_chunks[0] =
      make_device_compressed(*e.host_space, entry.cache_info.names, kCompBaseRows);
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0, 1, 2}),
                      std::runtime_error);
  }

  SECTION("cross-tier storage is rejected")
  {
    auto entry = make_mixed_host_entry(e, layout);
    entry.compressed_device_chunks.push_back(
      make_device_compressed(*e.gpu_space, entry.cache_info.names, kCompBaseRows));
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0, 1}),
                      std::runtime_error);
  }

  SECTION("invalid chunk kinds are rejected")
  {
    auto entry                  = make_mixed_host_entry(e, layout);
    entry.logical_order[0].kind = static_cast<chunk_kind>(255);
    REQUIRE_THROWS_AS(validate_pinned_entry_for_serving(entry, std::vector<std::size_t>{0, 1}),
                      std::runtime_error);
  }
}

TEST_CASE("raw GPU re-pin replaces compressed and mixed storage with equal row counts",
          "[cached_serving][scan_manager][mixed][repin]")
{
  auto& e          = env();
  bool const mixed = GENERATE(false, true);

  sirius::scan_manager::scan_manager_config config{};
  config.use_sirius_datasource   = false;
  config.thread_pool.num_threads = 1;
  config.enable_prefetch_cache   = false;
  sirius_scan_manager manager{config, *e.mgr, single_gpu_index()};

  device_pinned_chunks existing;
  if (mixed) {
    existing.compressed.push_back(make_device_compressed(*e.gpu_space, {"k"}, 2));
    existing.raw.push_back(make_gpu_table(*e.gpu_space, 2));
    existing.raw_memory_spaces.push_back(e.gpu_space);
    existing.logical_order = {{chunk_kind::compressed, 0}, {chunk_kind::raw, 0}};
  } else {
    existing.compressed.push_back(make_device_compressed(*e.gpu_space, {"k"}, 4));
  }
  manager.insert_pinned_entry_device_compressed(
    "repin", make_cache_info({"k"}), std::move(existing), *e.gpu_space);

  std::vector<std::unique_ptr<cudf::table>> raw_tables;
  raw_tables.push_back(make_gpu_table(*e.gpu_space, 4));
  manager.insert_pinned_entry(
    "repin", make_cache_info({"k"}), std::move(raw_tables), {e.gpu_space});

  bool found                                      = false;
  bool homogeneous_raw_gpu                        = false;
  std::size_t raw_chunks                          = 0;
  std::size_t rows                                = 0;
  cucascade::memory::memory_space* representative = nullptr;
  manager.visit_pinned_entries([&](std::string_view name, pinned_entry const& entry) {
    if (name != "repin") { return true; }
    found               = true;
    auto const raw_it   = entry.data_batches_by_column.find("k");
    raw_chunks          = raw_it == entry.data_batches_by_column.end() ? 0 : raw_it->second.size();
    rows                = entry.num_rows;
    representative      = entry.memory_space;
    homogeneous_raw_gpu = entry.tier == cucascade::memory::Tier::GPU &&
                          entry.logical_order.empty() && entry.compressed_device_chunks.empty() &&
                          entry.compressed_host_chunks.empty() && entry.host_chunks.empty();
    return false;
  });

  REQUIRE(found);
  REQUIRE(homogeneous_raw_gpu);
  REQUIRE(raw_chunks == 1);
  REQUIRE(rows == 4);
  REQUIRE(representative == e.gpu_space);
}

TEST_CASE("DuckDB storage and MVCC metadata publish atomically and visits permit re-entry",
          "[cached_serving][scan_manager][mvcc][publication]")
{
  auto& e = env();

  sirius::scan_manager::scan_manager_config config{};
  config.use_sirius_datasource   = false;
  config.thread_pool.num_threads = 1;
  config.enable_prefetch_cache   = false;
  sirius_scan_manager manager{config, *e.mgr, single_gpu_index()};

  auto make_duckdb_info = [] {
    auto info         = make_cache_info({"k"});
    info.catalog_name = "memory";
    info.schema_name  = "main";
    info.table_name   = "atomic_publication";
    return info;
  };
  auto make_mvcc = [](duckdb::transaction_t version) {
    auto metadata    = std::make_unique<sirius::scan_manager::duckdb_mvcc_metadata>();
    metadata->v_base = version;
    metadata->base_row_count_per_chunk = {4};
    return metadata;
  };
  auto make_tables = [&] {
    std::vector<std::unique_ptr<cudf::table>> tables;
    tables.push_back(make_gpu_table(*e.gpu_space, 4));
    return tables;
  };

  REQUIRE_THROWS_AS(manager.insert_pinned_entry(
                      "atomic_publication", make_duckdb_info(), make_tables(), {e.gpu_space}),
                    std::invalid_argument);
  bool found_after_rejection = false;
  manager.visit_pinned_entries([&](std::string_view name, pinned_entry const&) {
    found_after_rejection = name == "atomic_publication";
    return !found_after_rejection;
  });
  REQUIRE_FALSE(found_after_rejection);

  manager.insert_pinned_entry(
    "atomic_publication", make_duckdb_info(), make_tables(), {e.gpu_space}, make_mvcc(1));
  // Same identity, row count, placement, and boundaries takes the staged raw-GPU
  // merge path; its commit must refresh metadata with storage still serviceable.
  manager.insert_pinned_entry(
    "atomic_publication", make_duckdb_info(), make_tables(), {e.gpu_space}, make_mvcc(2));

  bool visited  = false;
  bool coherent = false;
  manager.visit_pinned_entries([&](std::string_view name, pinned_entry const& entry) {
    if (name != "atomic_publication") { return true; }
    visited        = true;
    auto const raw = entry.data_batches_by_column.find("k");
    coherent = raw != entry.data_batches_by_column.end() && raw->second.size() == 1 && entry.mvcc &&
               entry.mvcc->v_base == 2 &&
               entry.mvcc->base_row_count_per_chunk == std::vector<std::size_t>{4};
    // CP.22: this re-entry must not run while the registry mutex is held.
    manager.remove_pinned_entry(std::string{name});
    return false;
  });
  REQUIRE(visited);
  REQUIRE(coherent);

  bool remains = false;
  manager.visit_pinned_entries([&](std::string_view name, pinned_entry const&) {
    remains = name == "atomic_publication";
    return !remains;
  });
  REQUIRE_FALSE(remains);
}

//===----------------------------------------------------------------------===//
// cached provider: interleaved mixed serving (E0)
//===----------------------------------------------------------------------===//

TEST_CASE("cached provider serves a mixed entry interleaved, arm-for-arm and in order",
          "[cached_serving][scan_manager][mixed]")
{
  auto& e = env();
  sirius::telemetry::batch_telemetry_info telemetry{};

  // All three real mixed shapes: per-chunk fallback (interleaved), the
  // exception-latch (compressed prefix then raw suffix), and its mirror.
  auto const layout =
    GENERATE(std::vector<chunk_kind>{chunk_kind::raw,
                                     chunk_kind::compressed,
                                     chunk_kind::raw,
                                     chunk_kind::compressed,
                                     chunk_kind::raw},
             std::vector<chunk_kind>{
               chunk_kind::compressed, chunk_kind::compressed, chunk_kind::raw, chunk_kind::raw},
             std::vector<chunk_kind>{chunk_kind::raw, chunk_kind::raw, chunk_kind::compressed});

  SECTION("GPU tier")
  {
    auto entry = make_mixed_gpu_entry(e, layout);
    std::vector<std::size_t> selected{0, 1, 2};
    auto provider =
      make_provider_for_pinned_entry(entry, std::span<const std::size_t>(selected), telemetry);
    verify_mixed_serving(layout, drain_provider(*provider), /*gpu=*/true);
  }

  SECTION("HOST tier")
  {
    auto entry = make_mixed_host_entry(e, layout);
    std::vector<std::size_t> selected{0, 1};
    auto provider =
      make_provider_for_pinned_entry(entry, std::span<const std::size_t>(selected), telemetry);
    verify_mixed_serving(layout, drain_provider(*provider), /*gpu=*/false);
  }
}

TEST_CASE("cached provider snapshots chunk ownership instead of borrowing its entry",
          "[cached_serving][scan_manager][mixed]")
{
  auto& e = env();
  sirius::telemetry::batch_telemetry_info telemetry{};
  std::vector<chunk_kind> const layout{chunk_kind::raw, chunk_kind::compressed, chunk_kind::raw};
  std::unique_ptr<databatch_provider> provider;
  {
    auto entry = make_mixed_gpu_entry(e, layout);
    std::vector<std::size_t> selected{0, 1, 2};
    provider = make_provider_for_pinned_entry(entry, selected, telemetry);
  }
  verify_mixed_serving(layout, drain_provider(*provider), /*gpu=*/true);
}

TEST_CASE("cached provider still serves a homogeneous entry via the empty-logical_order fast path",
          "[cached_serving][scan_manager][mixed]")
{
  auto& e = env();
  sirius::telemetry::batch_telemetry_info telemetry{};

  SECTION("all-raw GPU entry (empty logical_order)")
  {
    auto entry = make_gpu_entry(*e.gpu_space, 3, 4);  // no logical_order set
    REQUIRE(entry.logical_order.empty());
    std::vector<std::size_t> selected{0, 1, 2};
    auto provider =
      make_provider_for_pinned_entry(entry, std::span<const std::size_t>(selected), telemetry);
    auto served = drain_provider(*provider);
    REQUIRE(served.size() == 3);
    for (auto const& ro : served) {
      REQUIRE(dynamic_cast<const cucascade::gpu_table_representation*>(ro.get_data()) != nullptr);
    }
  }

  SECTION("all-compressed GPU entry (empty logical_order)")
  {
    pinned_entry entry;
    set_cache_columns(entry, {"k", "v"});
    entry.tier                     = cucascade::memory::Tier::GPU;
    entry.memory_space             = e.gpu_space;
    entry.compressed_device_chunks = {
      make_device_compressed(*e.gpu_space, entry.cache_info.names, 3),
      make_device_compressed(*e.gpu_space, entry.cache_info.names, 5)};
    entry.num_rows = 8;
    std::vector<std::size_t> selected{0, 1};
    auto provider = make_provider_for_pinned_entry(entry, selected, telemetry);
    auto served   = drain_provider(*provider);
    REQUIRE(served.size() == 2);
    for (auto const& batch : served) {
      REQUIRE(dynamic_cast<const sirius::compressed_device_representation*>(batch.get_data()) !=
              nullptr);
    }
  }

  SECTION("all-compressed HOST entry (empty logical_order)")
  {
    pinned_entry entry;
    set_cache_columns(entry, {"k", "v"});
    entry.tier                   = cucascade::memory::Tier::HOST;
    entry.memory_space           = e.host_space;
    entry.compressed_host_chunks = {make_host_compressed(*e.host_space, entry.cache_info.names, 3),
                                    make_host_compressed(*e.host_space, entry.cache_info.names, 5)};
    entry.num_rows               = 8;
    std::vector<std::size_t> selected{0, 1};
    auto provider = make_provider_for_pinned_entry(entry, selected, telemetry);
    auto served   = drain_provider(*provider);
    REQUIRE(served.size() == 2);
    for (auto const& batch : served) {
      REQUIRE(dynamic_cast<const sirius::compressed_host_representation*>(batch.get_data()) !=
              nullptr);
    }
  }
}

//===----------------------------------------------------------------------===//
// Compressed resident sizing and projection contracts
//===----------------------------------------------------------------------===//

TEST_CASE("resident compressed scan sizing uses logical and decode footprints",
          "[cached_serving][scan_manager][compression][sizing]")
{
  auto& e                                  = env();
  constexpr std::size_t compressed_bytes   = 17;
  constexpr std::size_t uncompressed_bytes = 101;

  auto check = [compressed_bytes, uncompressed_bytes](
                 std::unique_ptr<cucascade::idata_representation> representation) {
    auto batch =
      cucascade::data_batch::make(sirius::get_next_batch_id(), std::move(representation));
    sirius::op::scan::scan_operator_input input(std::move(batch));
    CHECK(input.get_estimated_size_in_bytes() == uncompressed_bytes);
    CHECK(input.get_estimated_working_set_size_in_bytes() == compressed_bytes + uncompressed_bytes);
  };

  SECTION("GPU-tier compressed batch")
  {
    check(std::make_unique<sirius::compressed_device_representation>(
      *e.gpu_space,
      std::make_shared<sirius::device_compressed_blob>(),
      std::vector<std::string>{"k"},
      compressed_bytes,
      uncompressed_bytes,
      3));
  }

  SECTION("HOST-tier compressed batch")
  {
    check(std::make_unique<sirius::compressed_host_representation>(
      *e.host_space,
      std::make_shared<sirius::pinned_compressed_blob>(),
      std::vector<std::string>{"k"},
      compressed_bytes,
      uncompressed_bytes,
      3));
  }
}

TEST_CASE("compressed projections reject duplicate resolved columns",
          "[cached_serving][scan_manager][compression][projection]")
{
  auto& e = env();
  std::vector<std::size_t> const duplicates{0, 0};
  std::vector<std::size_t> const reordered{1, 0};

  SECTION("HOST-tier representation")
  {
    auto representation = make_host_compressed(*e.host_space, {"k", "v"}, 3);
    REQUIRE_THROWS_AS(representation->select_columns(duplicates), std::invalid_argument);
    auto projected = representation->select_columns(reordered);
    REQUIRE_THROWS_AS(projected->select_columns(duplicates), std::invalid_argument);
  }

  SECTION("GPU-tier representation")
  {
    auto representation = make_device_compressed(*e.gpu_space, {"k", "v"}, 3);
    REQUIRE_THROWS_AS(representation->select_columns(duplicates), std::invalid_argument);
    auto projected = representation->select_columns(reordered);
    REQUIRE_THROWS_AS(projected->select_columns(duplicates), std::invalid_argument);
  }
}

TEST_CASE("compressed representations reject null backing blobs",
          "[cached_serving][scan_manager][compression][contracts]")
{
  auto& e = env();

  REQUIRE_THROWS_AS(std::make_unique<sirius::compressed_host_representation>(
                      *e.host_space,
                      std::shared_ptr<sirius::pinned_compressed_blob>{},
                      std::vector<std::string>{"k"},
                      0,
                      0,
                      0),
                    std::invalid_argument);

  REQUIRE_THROWS_AS(std::make_unique<sirius::compressed_device_representation>(
                      *e.gpu_space,
                      std::shared_ptr<sirius::device_compressed_blob>{},
                      std::vector<std::string>{"k"},
                      0,
                      0,
                      0),
                    std::invalid_argument);
}

TEST_CASE("pinned block copies reject out-of-allocation ranges",
          "[cached_serving][scan_manager][compression][contracts]")
{
  auto allocation =
    cucascade::memory::fixed_size_host_memory_resource::multiple_blocks_allocation::empty();

  REQUIRE_NOTHROW(
    sirius::copy_device_to_pinned_blocks(nullptr, *allocation, 0, 0, rmm::cuda_stream_view{}));
  REQUIRE_NOTHROW(
    sirius::copy_pinned_blocks_to_device(*allocation, 0, nullptr, 0, rmm::cuda_stream_view{}));

  REQUIRE_THROWS_AS(
    sirius::copy_device_to_pinned_blocks(nullptr, *allocation, 0, 1, rmm::cuda_stream_view{}),
    std::out_of_range);
  REQUIRE_THROWS_AS(
    sirius::copy_device_to_pinned_blocks(nullptr, *allocation, 1, 0, rmm::cuda_stream_view{}),
    std::out_of_range);
  REQUIRE_THROWS_AS(
    sirius::copy_pinned_blocks_to_device(*allocation, 0, nullptr, 1, rmm::cuda_stream_view{}),
    std::out_of_range);
}
