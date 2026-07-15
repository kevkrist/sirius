/*
 * Copyright 2025, Sirius Contributors.
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

#include "pin_table.hpp"

#include "compression/compressed_representation.hpp"
#include "data/sirius_converter_registry.hpp"
#include "io/io_context.hpp"
#include "log/logging.hpp"
#include "op/scan/duckdb_native_gpu_ingestible.hpp"
#include "op/scan/gpu_ingestible.hpp"
#include "scan_manager/pinned_chunk_stats.hpp"
#include "scan_manager/round_robin_strategy.hpp"

#include <cudf/table/table.hpp>
#include <cudf/utilities/error.hpp>
#include <cudf/utilities/traits.hpp>

#include <rmm/cuda_device.hpp>
#include <rmm/cuda_stream_view.hpp>
#include <rmm/device_buffer.hpp>

#include <cuda_runtime.h>

#include <api/compressed_table_io.hpp>
#include <api/simpatico_codegen.hpp>
#include <cucascade/cudf/gpu_data_representation.hpp>
#include <cucascade/cudf/host_data_representation.hpp>
#include <cucascade/memory/common.hpp>
#include <cucascade/memory/memory_space.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace duckdb {

namespace {

std::mutex& recorded_calls_mutex()
{
  static std::mutex m;
  return m;
}

std::vector<PinTableArgs>& recorded_calls_storage()
{
  static std::vector<PinTableArgs> calls;
  return calls;
}

std::vector<std::string>& recorded_unpin_calls_storage()
{
  static std::vector<std::string> calls;
  return calls;
}

}  // namespace

void pin_table_to(const PinTableArgs& args)
{
  std::lock_guard<std::mutex> lock(recorded_calls_mutex());
  recorded_calls_storage().push_back(args);
}

void unpin_table_to(const std::string& name)
{
  std::lock_guard<std::mutex> lock(recorded_calls_mutex());
  recorded_unpin_calls_storage().push_back(name);
}

namespace pin_table_testing {

const std::vector<PinTableArgs>& recorded_calls() { return recorded_calls_storage(); }

void clear_recorded_calls()
{
  std::lock_guard<std::mutex> lock(recorded_calls_mutex());
  recorded_calls_storage().clear();
}

const std::vector<std::string>& recorded_unpin_calls() { return recorded_unpin_calls_storage(); }

void clear_recorded_unpin_calls()
{
  std::lock_guard<std::mutex> lock(recorded_calls_mutex());
  recorded_unpin_calls_storage().clear();
}

}  // namespace pin_table_testing

}  // namespace duckdb

namespace sirius {

// A coalescer change that splits a row group across batches or reorders batches
// must fail the pin loudly here instead of silently misaligning the per-chunk
// visibility masks built over these boundaries (full contract in pin_table.hpp).
void validate_duckdb_pin_chunk(const op::scan::scan_info& batch,
                               std::size_t chunk_rows,
                               std::size_t rows_before_chunk)
{
  auto const* native = dynamic_cast<const op::scan::duckdb_native_scan_info*>(&batch);
  if (native == nullptr) { return; }

  std::size_t next_row = rows_before_chunk;
  for (auto const& rg : native->row_groups) {
    if (static_cast<std::size_t>(rg.row_group_start) != next_row) {
      throw std::runtime_error(
        "[pin_table] duckdb-native batch breaks the whole-row-group contiguity invariant: row "
        "group " +
        std::to_string(rg.row_group_index) + " starts at row " +
        std::to_string(rg.row_group_start) + " but the pin has materialized rows [0, " +
        std::to_string(next_row) + ")");
    }
    next_row += rg.row_count;
  }
  if (next_row - rows_before_chunk != chunk_rows) {
    throw std::runtime_error(
      "[pin_table] duckdb-native chunk decoded " + std::to_string(chunk_rows) +
      " rows but its row-group metadata covers " + std::to_string(next_row - rows_before_chunk));
  }
}

namespace {

/// Per-materialized-batch sink: receives one GPU-resident table, its GPU placement, the
/// stream it was decoded on, and the chunk's zone-map capture (empty when capture is off).
/// The driver does NOT synchronize — the sink owns the sync, because the host-streaming path
/// must synchronize while the GPU table (and the gpu_table_representation wrapping it) is
/// still alive, after the D2H conversion has been enqueued on the same stream.
using pin_batch_sink =
  std::function<void(std::unique_ptr<cudf::table>,
                     cucascade::memory::memory_space* target,
                     rmm::cuda_stream_view stream,
                     std::vector<duckdb::unique_ptr<duckdb::BaseStatistics>> chunk_stats)>;

/// Shared driver behind @ref materialize_all_batches and the pin_to_host/device_with_compression
/// drivers: walk the
/// ingestible's metadata + batch coalescer to completion, materialize each emitted batch onto a
/// round-robin GPU, and hand it to @p on_batch. The single-threaded, deterministic round-robin
/// placement means re-pinning the same source yields identical placement (required by
/// insert_pinned_entry's merge path on the GPU tier) and bounds peak GPU residency to ~one batch
/// (the host tier frees each table in @p on_batch before the next is materialized).
void materialize_pin_batches(op::scan::gpu_ingestible& ingestible,
                             std::span<cucascade::memory::memory_space* const> gpu_spaces,
                             io::sirius_ioctx& io_ctx,
                             duckdb::vector<duckdb::LogicalType> const& pinned_column_types,
                             const pin_batch_sink& on_batch)
{
  if (gpu_spaces.empty()) {
    throw std::invalid_argument("[materialize_pin_batches] gpu_spaces must be non-empty");
  }

  // GPU placement via the same strategy the query path uses
  // (sirius_scan_manager::prepare_for_query). A fresh strategy per call starts its
  // cursor at 0, so re-pinning the same source yields identical placement — required
  // by insert_pinned_entry's merge path. The strategy hands out device ids; map each
  // back to the memory_space to materialize into.
  std::vector<int> device_ids;
  std::unordered_map<int, cucascade::memory::memory_space*> space_by_device;
  device_ids.reserve(gpu_spaces.size());
  for (auto* sp : gpu_spaces) {
    device_ids.push_back(sp->get_device_id());
    space_by_device.emplace(sp->get_device_id(), sp);
  }
  scan_manager::round_robin_strategy placement(std::move(device_ids));

  // next_split_provider takes the io_ctx by shared_ptr; sirius_ioctx derives
  // std::enable_shared_from_this and the scan manager owns it via a shared_ptr, so
  // this hands the metadata reads a valid owning reference for the read's duration.
  auto io_ctx_sp = io_ctx.shared_from_this();

  auto coalescer = ingestible.create_batch_coalescer();

  // Rows materialized so far, in emission order — feeds the chunk-contiguity
  // validation (duckdb-native pins only).
  std::size_t rows_materialized = 0;

  // Materialize one coalesced batch into a GPU-resident cudf::table and hand it to on_batch
  // together with its GPU placement + the decode stream. Mirrors
  // load_balancing_scan_batch_coalescer::process_provider_inputs, minus the connector/balancer
  // and the post-decode step: there is no downstream scan operator at pin time, so the
  // unfiltered table is delivered to the sink directly. The device guard stays in scope across
  // the on_batch call, so the sink runs on the correct device.
  auto handle_batch = [&](std::unique_ptr<op::scan::scan_info> batch) {
    if (!batch) { return; }
    // round_robin_strategy ignores pipeline_id/data; it returns the next device id
    // from its cursor. gpu_spaces is non-empty (checked above), so the strategy always
    // yields a device. space_by_device round-trips it to the materialization target.
    int const gpu_id =
      placement.get_next_gpu(/*pipeline_id=*/0, /*data=*/nullptr, /*hint=*/{}).value();
    auto* target = space_by_device.at(gpu_id);
    rmm::cuda_set_device_raii device_guard{rmm::cuda_device_id{gpu_id}};
    // Borrow a real (non-default) stream from the target memory_space's pool: the
    // duckdb-native decoder's cudaMemcpyBatchAsync rejects the legacy default
    // stream. The pool owns the stream for the memory_space's lifetime (which
    // outlives the pinned data), so the materialized buffers' deallocation stream
    // stays valid after this call.
    auto stream = target->acquire_stream();

    // A pin caches the table UNFILTERED: it has no row filter (no WHERE), and the
    // ingestible's projection_ids are identity into column_ids, so the reader already
    // emits exactly the pinned columns in column_ids order. materialize_metadata_to_table
    // thus yields the cache-ready table directly — no scan_operator_input wrapper and no
    // post_filter_and_project (which would only apply a row filter or re-project to a
    // query's output layout, neither of which a pin needs). `batch` is borrowed by const
    // ref and outlives the call.
    auto materialized = ingestible.materialize_metadata_to_table(*batch, *target, stream);
    auto tbl          = materialized.table.release(stream, target->get_default_allocator());
    if (!tbl) { throw std::runtime_error("pin-table materialization produced no owned table"); }
    auto const chunk_rows = static_cast<std::size_t>(tbl->num_rows());
    validate_duckdb_pin_chunk(*batch, chunk_rows, rows_materialized);
    rows_materialized += chunk_rows;
    // Zone-map capture, while the decode device guard + stream are active and the
    // GPU table is alive. The scalar downloads inside are synchronous, so the
    // capture is complete before the sink converts or frees the table. Clean
    // unsupported-metadata cases degrade to null cells inside; CUDA errors
    // propagate and abort the pin like any other pin-time CUDA failure.
    std::vector<duckdb::unique_ptr<duckdb::BaseStatistics>> chunk_stats;
    if (!pinned_column_types.empty()) {
      chunk_stats = scan_manager::compute_pinned_chunk_stats(
        tbl->view(), pinned_column_types, stream, target->get_default_allocator());
    }
    on_batch(std::move(tbl), target, stream, std::move(chunk_stats));
  };

  while (!ingestible.has_processed_all_metadata()) {
    // A pin reads its fixed ingestible on the one ioctx it was given; route every file to it.
    auto task = ingestible.next_split_provider([io_ctx_sp](std::string_view) { return io_ctx_sp; });
    if (!task) { continue; }
    auto info = task();
    if (!info) { continue; }
    for (auto& b : coalescer->push(std::move(info))) {
      handle_batch(std::move(b));
    }
  }
  for (auto& b : coalescer->flush()) {
    handle_batch(std::move(b));
  }
}

}  // namespace

materialized_pin materialize_all_batches(
  op::scan::gpu_ingestible& ingestible,
  std::span<cucascade::memory::memory_space* const> gpu_spaces,
  io::sirius_ioctx& io_ctx,
  duckdb::vector<duckdb::LogicalType> const& pinned_column_types)
{
  materialized_pin out;
  materialize_pin_batches(
    ingestible,
    gpu_spaces,
    io_ctx,
    pinned_column_types,
    [&](std::unique_ptr<cudf::table> tbl,
        cucascade::memory::memory_space* target,
        rmm::cuda_stream_view stream,
        std::vector<duckdb::unique_ptr<duckdb::BaseStatistics>> chunk_stats) {
      // Cached GPU batches are stored with a null writer stream, so the data
      // must be fully resident before it can be served or host-converted.
      stream.synchronize();
      out.base_row_count_per_chunk.push_back(static_cast<std::size_t>(tbl->num_rows()));
      out.tables.emplace_back(std::move(tbl));
      out.chunk_memory_spaces.push_back(target);
      if (!chunk_stats.empty()) { out.chunk_stats.emplace_back(std::move(chunk_stats)); }
    });
  return out;
}

namespace {

enum class staged_batch_outcome { keep_compressed, ratio_reject, compression_failure };

/// A reversible compression stage plus the inspection data needed to stage its
/// payload. The outcome is a single state, so impossible keep/latch boolean
/// combinations are not representable.
struct staged_batch_decision {
  simpatico::staged_compression stage;
  staged_batch_outcome outcome{staged_batch_outcome::compression_failure};
  rmm::cuda_stream_view inspection_stream;
  std::vector<std::uint8_t> header;
  std::vector<simpatico::payload_buffer_ref> buffers;
  std::uint64_t payload_bytes{0};
  std::size_t payload_size{0};
  std::size_t compressed_bytes{0};
  std::string log_reason;
};

/// Own the source transaction and the external payload destination as one
/// exception-safe unit. Before accept, the destination may contain asynchronous
/// writes from the candidate. Reject proves the current stream tail before freeing
/// it. If that proof fails, both sides are retained rather than recycled while GPU
/// work may still reference them.
template <typename Destination>
class staged_payload_transaction {
 public:
  explicit staged_payload_transaction(simpatico::staged_compression stage) noexcept
    : stage_(std::move(stage))
  {
  }

  staged_payload_transaction(staged_payload_transaction const&)            = delete;
  staged_payload_transaction& operator=(staged_payload_transaction const&) = delete;
  staged_payload_transaction(staged_payload_transaction&&)                 = delete;
  staged_payload_transaction& operator=(staged_payload_transaction&&)      = delete;

  ~staged_payload_transaction() noexcept
  {
    if (!active_) return;
    try {
      [[maybe_unused]] auto original = std::move(stage_).reject();
      active_                        = false;
      release_destination_after_completion();
    } catch (...) {
      retain_destination();
    }
  }

  Destination& emplace_destination()
  {
    if (destination_) {
      throw std::logic_error("staged payload transaction already has a destination");
    }

    // Construct every shared-ownership allocation while the stage is reversible.
    // Until accept publishes ownership, the shared deleter is deliberately inert
    // and the unique_ptr remains the sole deleting owner.
    auto destination       = std::make_unique<Destination>();
    auto destination_state = std::make_shared<shared_destination_state>();
    auto destination_owner = std::shared_ptr<Destination>{
      destination.get(), shared_destination_deleter{destination_state}};
    destination_       = std::move(destination);
    destination_state_ = std::move(destination_state);
    destination_owner_ = std::move(destination_owner);
    return *destination_;
  }

  [[nodiscard]] std::shared_ptr<Destination> const& destination_owner() const
  {
    if (!destination_owner_) {
      throw std::logic_error("staged payload transaction has no destination");
    }
    return destination_owner_;
  }

  [[nodiscard]] std::unique_ptr<cudf::table> reject()
  {
    auto original = std::move(stage_).reject();
    active_       = false;
    release_destination_after_completion();
    return original;
  }

  [[nodiscard]] simpatico::compressed_table accept()
  {
    if (!destination_ || !destination_state_ || !destination_owner_ ||
        destination_owner_.use_count() < 2) {
      throw std::logic_error("staged payload destination must have a prepared owner before accept");
    }

    // accept() is the only fallible operation below. If its completion proof
    // throws, this transaction is still active and its destructor can retry
    // reject before choosing the final leak-safe fallback.
    auto accepted                        = std::move(stage_).accept();
    destination_state_->owns_destination = true;
    [[maybe_unused]] auto* transferred   = destination_.release();
    active_                              = false;
    destination_owner_.reset();
    destination_state_.reset();
    return accepted;
  }

 private:
  struct shared_destination_state {
    bool owns_destination{false};
  };

  struct shared_destination_deleter {
    std::shared_ptr<shared_destination_state> state;

    void operator()(Destination* destination) const noexcept
    {
      if (state->owns_destination) { delete destination; }
    }
  };

  void release_destination_after_completion() noexcept
  {
    if (destination_owner_ && destination_owner_.use_count() > 1) {
      destination_state_->owns_destination = true;
      [[maybe_unused]] auto* transferred   = destination_.release();
    }
    destination_owner_.reset();
    destination_state_.reset();
    destination_.reset();
  }

  void retain_destination() noexcept
  {
    // A failed final completion proof means queued GPU work may still target the
    // destination. Keep the deleter inert and intentionally retain the storage.
    [[maybe_unused]] auto* retained = destination_.release();
  }

  std::unique_ptr<Destination> destination_;
  std::shared_ptr<shared_destination_state> destination_state_;
  std::shared_ptr<Destination> destination_owner_;
  simpatico::staged_compression stage_;
  bool active_{true};
};

struct payload_copy_region {
  std::size_t offset;
  std::size_t size;
};

[[nodiscard]] payload_copy_region checked_payload_copy_region(
  simpatico::payload_buffer_ref const& buffer, std::size_t payload_size)
{
  if (buffer.offset > std::numeric_limits<std::size_t>::max() ||
      buffer.size_bytes > std::numeric_limits<std::size_t>::max()) {
    throw std::overflow_error("compressed leaf buffer exceeds the platform size limit");
  }

  auto const offset = static_cast<std::size_t>(buffer.offset);
  auto const size   = static_cast<std::size_t>(buffer.size_bytes);
  if (offset > payload_size || size > payload_size - offset) {
    throw std::out_of_range("compressed leaf buffer lies outside the staged payload");
  }
  if (size != 0 && buffer.device_ptr == nullptr) {
    throw std::invalid_argument("non-empty compressed leaf buffer has a null device pointer");
  }
  return {.offset = offset, .size = size};
}

template <typename T>
void reserve_one_more(std::vector<T>& values)
{
  if (values.size() == values.max_size()) {
    throw std::length_error("pin result cannot hold another chunk");
  }
  values.reserve(values.size() + 1);
}

void validate_compression_config(compression_pin_config const& compression)
{
  if (compression.enabled &&
      (!std::isfinite(compression.max_compressed_fraction) ||
       compression.max_compressed_fraction < 0.0 || compression.max_compressed_fraction > 1.0)) {
    throw std::invalid_argument("max_compressed_fraction must be finite and in [0, 1]");
  }
}

void set_failure_reason(staged_batch_decision& decision, std::string_view reason) noexcept
{
  decision.outcome = staged_batch_outcome::compression_failure;
  try {
    decision.log_reason.assign(reason);
  } catch (...) {
    // Preserve the rejectable stage even when recording a diagnostic runs out of
    // host memory. The caller supplies a stable fallback message.
  }
}

/// Move @p table into a reversible compression stage, build the header through
/// its stream-bound inspection facade, and apply the ratio while both outcomes
/// remain available.
staged_batch_decision stage_and_decide(std::unique_ptr<cudf::table> table,
                                       compression_pin_config const& compression,
                                       std::size_t uncompressed_bytes,
                                       rmm::cuda_stream_view stream,
                                       rmm::device_async_resource_ref mr)
{
  staged_batch_decision decision{
    .stage = simpatico::try_compress_with_plan(
      std::move(table), compression.plan_dsl, stream, mr, compression.column_names),
    .inspection_stream = stream};
  if (!decision.stage.ok()) {
    set_failure_reason(decision, decision.stage.error());
    return decision;
  }

  try {
    auto const inspection       = decision.stage.table();
    decision.inspection_stream  = inspection.stream();
    std::string const hdr_error = simpatico::build_compressed_table_header(
      inspection, decision.header, decision.buffers, decision.payload_bytes);
    if (!hdr_error.empty()) {
      set_failure_reason(decision, "build_compressed_table_header: " + hdr_error);
      return decision;
    }
  } catch (cudf::fatal_cuda_error const&) {
    throw;
  } catch (std::bad_alloc const&) {
    set_failure_reason(decision, "host allocation failed while inspecting compressed payload");
    return decision;
  } catch (std::exception const& failure) {
    set_failure_reason(decision, failure.what());
    return decision;
  } catch (...) {
    set_failure_reason(decision, "compressed payload inspection failed");
    return decision;
  }

  if (decision.payload_bytes > std::numeric_limits<std::size_t>::max()) {
    set_failure_reason(decision, "compressed payload exceeds the platform size limit");
    return decision;
  }
  decision.payload_size = static_cast<std::size_t>(decision.payload_bytes);
  if (decision.payload_size > std::numeric_limits<std::size_t>::max() - decision.header.size()) {
    set_failure_reason(decision, "compressed header and payload size overflow");
    return decision;
  }
  decision.compressed_bytes = decision.header.size() + decision.payload_size;
  decision.outcome          = staged_batch_outcome::ratio_reject;
  if ((uncompressed_bytes == 0 && decision.compressed_bytes != 0) ||
      (uncompressed_bytes != 0 &&
       static_cast<double>(decision.compressed_bytes) >
         compression.max_compressed_fraction * static_cast<double>(uncompressed_bytes))) {
    return decision;
  }

  decision.outcome = staged_batch_outcome::keep_compressed;
  return decision;
}

}  // namespace

host_pin_result materialize_pin_to_host_with_compression(
  op::scan::gpu_ingestible& ingestible,
  std::span<cucascade::memory::memory_space* const> gpu_spaces,
  const std::unordered_map<int, cucascade::memory::memory_space*>& host_space_by_gpu,
  io::sirius_ioctx& io_ctx,
  compression_pin_config const& compression,
  duckdb::vector<duckdb::LogicalType> const& pinned_column_types)
{
  validate_compression_config(compression);
  auto& registry = converter_registry::get();
  host_pin_result out;
  bool compression_failed = false;

  materialize_pin_batches(
    ingestible,
    gpu_spaces,
    io_ctx,
    pinned_column_types,
    [&](std::unique_ptr<cudf::table> table,
        cucascade::memory::memory_space* src_space,
        rmm::cuda_stream_view stream,
        std::vector<duckdb::unique_ptr<duckdb::BaseStatistics>> chunk_stats) {
      if (!table) {
        throw std::runtime_error(
          "materialize_pin_to_host_with_compression received a null materialized table");
      }
      auto* target_host_space = host_space_by_gpu.at(src_space->get_device_id());
      bool compressed_this_chunk{false};
      auto const num_rows          = static_cast<std::int64_t>(table->num_rows());
      auto const uncompressed_size = table->alloc_size();
      out.base_row_count_per_chunk.push_back(static_cast<std::size_t>(num_rows));

      if (compression.enabled && !compression_failed && table->num_columns() > 0 &&
          !compression.plan_dsl.empty() && uncompressed_size >= compression.min_batch_size_bytes) {
        auto decision = stage_and_decide(std::move(table),
                                         compression,
                                         uncompressed_size,
                                         stream,
                                         src_space->get_default_allocator());

        if (decision.outcome != staged_batch_outcome::keep_compressed) {
          table = std::move(decision.stage).reject();
          if (decision.outcome == staged_batch_outcome::compression_failure) {
            compression_failed = true;
            SIRIUS_LOG_WARN(
              "[materialize_pin_to_host_with_compression] compression failed: {}; "
              "pinning uncompressed and disabling compression for the rest of the pin",
              decision.log_reason.empty() ? "no diagnostic available" : decision.log_reason);
          } else {
            SIRIUS_LOG_DEBUG(
              "[materialize_pin_to_host_with_compression] compressed {}B > {:.0f}% of {}B "
              "original; pinning uncompressed",
              decision.compressed_bytes,
              compression.max_compressed_fraction * 100.0,
              uncompressed_size);
          }
        } else {
          staged_payload_transaction<sirius::pinned_compressed_blob> transfer{
            std::move(decision.stage)};
          std::shared_ptr<sirius::compressed_host_representation> prepared_representation;
          bool payload_ready{false};
          try {
            auto& blob  = transfer.emplace_destination();
            blob.header = std::move(decision.header);
            auto* host_mr =
              target_host_space->get_memory_resource_of<cucascade::memory::Tier::HOST>();
            if (host_mr == nullptr) {
              throw std::runtime_error("target host space has no fixed_size_host_memory_resource");
            }
            blob.payload       = host_mr->allocate_multiple_blocks(decision.payload_size);
            blob.payload_bytes = decision.payload_bytes;
            reserve_one_more(out.compressed_chunks);
            reserve_one_more(out.logical_order);
            prepared_representation =
              std::make_shared<sirius::compressed_host_representation>(*target_host_space,
                                                                       transfer.destination_owner(),
                                                                       compression.column_names,
                                                                       decision.compressed_bytes,
                                                                       uncompressed_size,
                                                                       num_rows);
            for (auto const& buffer : decision.buffers) {
              auto const region = checked_payload_copy_region(buffer, decision.payload_size);
              if (region.size == 0) { continue; }
              sirius::copy_device_to_pinned_blocks(buffer.device_ptr,
                                                   *blob.payload,
                                                   region.offset,
                                                   region.size,
                                                   decision.inspection_stream);
            }
            decision.buffers.clear();
            payload_ready = true;
          } catch (std::exception const& failure) {
            prepared_representation.reset();
            table              = transfer.reject();
            compression_failed = true;
            SIRIUS_LOG_WARN(
              "[materialize_pin_to_host_with_compression] payload staging failed: {}; "
              "pinning uncompressed and disabling compression for the rest of the pin",
              failure.what());
          } catch (...) {
            prepared_representation.reset();
            table              = transfer.reject();
            compression_failed = true;
            SIRIUS_LOG_WARN(
              "[materialize_pin_to_host_with_compression] payload staging failed; "
              "pinning uncompressed and disabling compression for the rest of the pin");
          }

          if (payload_ready) {
            [[maybe_unused]] auto accepted = transfer.accept();
            auto const compressed_index    = out.compressed_chunks.size();
            // Capacity and shared ownership were prepared before accept, so
            // publishing the completed transaction cannot allocate.
            out.compressed_chunks.push_back(std::move(prepared_representation));
            out.logical_order.push_back(
              {sirius::scan_manager::chunk_kind::compressed, compressed_index});
            compressed_this_chunk = true;
          }
        }
      }

      if (!compressed_this_chunk) {
        cucascade::gpu_table_representation gpu_repr(std::move(table), *src_space, stream);
        auto host_repr = registry.convert<cucascade::host_data_representation>(
          gpu_repr, target_host_space, stream);
        stream.synchronize();
        out.host_chunks.emplace_back(std::move(host_repr));
        out.logical_order.push_back(
          {sirius::scan_manager::chunk_kind::raw, out.host_chunks.size() - 1});
        // Keep the raw arm's zone maps positional with host_chunks. Empty when capture is
        // off; a compressed chunk contributes no entry, so a homogeneous uncompressed pin
        // yields chunk_stats.size() == host_chunks.size() for insert_pinned_entry_host.
        out.chunk_stats.push_back(std::move(chunk_stats));
      }
    });

  if (out.host_chunks.empty() || out.compressed_chunks.empty()) { out.logical_order.clear(); }
  return out;
}

device_pin_result materialize_pin_to_device_with_compression(
  op::scan::gpu_ingestible& ingestible,
  std::span<cucascade::memory::memory_space* const> gpu_spaces,
  io::sirius_ioctx& io_ctx,
  compression_pin_config const& compression)
{
  validate_compression_config(compression);
  device_pin_result out;
  bool compression_failed = false;

  materialize_pin_batches(
    ingestible,
    gpu_spaces,
    io_ctx,
    {},
    [&](std::unique_ptr<cudf::table> table,
        cucascade::memory::memory_space* src_space,
        rmm::cuda_stream_view stream,
        std::vector<duckdb::unique_ptr<duckdb::BaseStatistics>> /*chunk_stats*/) {
      if (!table) {
        throw std::runtime_error(
          "materialize_pin_to_device_with_compression received a null materialized table");
      }
      bool compressed_this_chunk{false};
      auto const num_rows          = static_cast<std::int64_t>(table->num_rows());
      auto const uncompressed_size = table->alloc_size();
      out.base_row_count_per_chunk.push_back(static_cast<std::size_t>(num_rows));

      if (compression.enabled && !compression_failed && table->num_columns() > 0 &&
          !compression.plan_dsl.empty() && uncompressed_size >= compression.min_batch_size_bytes) {
        auto decision = stage_and_decide(std::move(table),
                                         compression,
                                         uncompressed_size,
                                         stream,
                                         src_space->get_default_allocator());

        if (decision.outcome != staged_batch_outcome::keep_compressed) {
          table = std::move(decision.stage).reject();
          if (decision.outcome == staged_batch_outcome::compression_failure) {
            compression_failed = true;
            SIRIUS_LOG_WARN(
              "[materialize_pin_to_device_with_compression] compression failed: {}; "
              "pinning uncompressed and disabling compression for the rest of the pin",
              decision.log_reason.empty() ? "no diagnostic available" : decision.log_reason);
          } else {
            SIRIUS_LOG_DEBUG(
              "[materialize_pin_to_device_with_compression] compressed {}B > {:.0f}% of {}B "
              "original; pinning uncompressed",
              decision.compressed_bytes,
              compression.max_compressed_fraction * 100.0,
              uncompressed_size);
          }
        } else {
          staged_payload_transaction<sirius::device_compressed_blob> transfer{
            std::move(decision.stage)};
          std::shared_ptr<sirius::compressed_device_representation> prepared_representation;
          bool payload_ready{false};
          try {
            auto& blob         = transfer.emplace_destination();
            blob.header        = std::move(decision.header);
            blob.payload       = rmm::device_buffer(decision.payload_size,
                                              decision.inspection_stream,
                                              src_space->get_default_allocator());
            blob.payload_bytes = decision.payload_bytes;
            reserve_one_more(out.compressed_chunks);
            reserve_one_more(out.logical_order);
            prepared_representation = std::make_shared<sirius::compressed_device_representation>(
              *src_space,
              transfer.destination_owner(),
              compression.column_names,
              decision.compressed_bytes,
              uncompressed_size,
              num_rows);
            for (auto const& buffer : decision.buffers) {
              auto const region = checked_payload_copy_region(buffer, decision.payload_size);
              if (region.size == 0) { continue; }
              CUCASCADE_CUDA_TRY(
                cudaMemcpyAsync(static_cast<std::byte*>(blob.payload.data()) + region.offset,
                                buffer.device_ptr,
                                region.size,
                                cudaMemcpyDeviceToDevice,
                                decision.inspection_stream.value()));
            }
            decision.buffers.clear();
            payload_ready = true;
          } catch (std::exception const& failure) {
            prepared_representation.reset();
            table              = transfer.reject();
            compression_failed = true;
            SIRIUS_LOG_WARN(
              "[materialize_pin_to_device_with_compression] payload staging failed: {}; "
              "pinning uncompressed and disabling compression for the rest of the pin",
              failure.what());
          } catch (...) {
            prepared_representation.reset();
            table              = transfer.reject();
            compression_failed = true;
            SIRIUS_LOG_WARN(
              "[materialize_pin_to_device_with_compression] payload staging failed; "
              "pinning uncompressed and disabling compression for the rest of the pin");
          }

          if (payload_ready) {
            [[maybe_unused]] auto accepted = transfer.accept();
            auto const compressed_index    = out.compressed_chunks.size();
            // Capacity and shared ownership were prepared before accept, so
            // publishing the completed transaction cannot allocate.
            out.compressed_chunks.push_back(std::move(prepared_representation));
            out.logical_order.push_back(
              {sirius::scan_manager::chunk_kind::compressed, compressed_index});
            compressed_this_chunk = true;
          }
        }
      }

      if (!compressed_this_chunk) {
        stream.synchronize();
        out.tables.emplace_back(std::move(table));
        out.chunk_memory_spaces.push_back(src_space);
        out.logical_order.push_back({sirius::scan_manager::chunk_kind::raw, out.tables.size() - 1});
      }
    });

  if (out.tables.empty() || out.compressed_chunks.empty()) { out.logical_order.clear(); }
  return out;
}
}  // namespace sirius
