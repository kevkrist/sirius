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

#pragma once

#include "scan_manager/pinned_chunk_source.hpp"

#include <duckdb/common/types.hpp>
#include <duckdb/common/vector.hpp>
#include <duckdb/storage/statistics/base_statistics.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb {

struct PinTableArgs {
  std::string path;
  std::string tier;
  std::string name;
  std::optional<std::vector<std::string>> cols;
  /// Resolved at bind time: "parquet" or "duckdb". Chosen from an explicit
  /// `format` named parameter, else inferred from the path extension
  /// (.parquet -> parquet, .db/.duckdb -> duckdb).
  std::string format;
  /// duckdb-native only: schema that contains the table named by `name`. Defaults
  /// to "main"; the catalog search path (current/USE'd database) still applies.
  std::string schema = "main";
};

void pin_table_to(const PinTableArgs& args);

void unpin_table_to(const std::string& name);

// Test-only helpers used by unit tests to verify pin_table forwards the
// correct parameters to pin_table_to. Not part of the stable public API.
namespace pin_table_testing {
const std::vector<PinTableArgs>& recorded_calls();
void clear_recorded_calls();

const std::vector<std::string>& recorded_unpin_calls();
void clear_recorded_unpin_calls();
}  // namespace pin_table_testing

}  // namespace duckdb

namespace cudf {
class table;
}  // namespace cudf

namespace cucascade {
class host_data_representation;
}  // namespace cucascade

namespace sirius {
class compressed_host_representation;
class compressed_device_representation;
}  // namespace sirius

namespace cucascade::memory {
class memory_space;
}  // namespace cucascade::memory

namespace sirius {

namespace op::scan {
class gpu_ingestible;
class scan_info;
}  // namespace op::scan
namespace io {
class sirius_ioctx;
}  // namespace io

/// GPU-resident tables produced by driving a @c gpu_ingestible to completion, with
/// each table's GPU placement recorded in @c chunk_memory_spaces (parallel to
/// @c tables). Consumed by the pin_table path: fed directly into
/// @c sirius_scan_manager::insert_pinned_entry, or, after host conversion of each
/// table, into @c insert_pinned_entry_host.
struct materialized_pin {
  std::vector<std::unique_ptr<cudf::table>> tables;
  std::vector<cucascade::memory::memory_space*> chunk_memory_spaces;
  /// Row count of each materialized chunk (parallel to @c tables). For duckdb-native
  /// pins these become @c duckdb_mvcc_metadata::base_row_count_per_chunk — the
  /// positional chunk→rowid-range map query-time MVCC merge relies on.
  std::vector<std::size_t> base_row_count_per_chunk;
  /// Per-chunk zone-map capture: chunk_stats[c][i] = stats of batch column i of
  /// chunk c (null = none). Parallel to @c tables when capture ran; empty when
  /// capture was skipped (no pinned column types). Fed together with the
  /// pin-time column types into @c sirius_scan_manager::insert_pinned_entry.
  std::vector<std::vector<duckdb::unique_ptr<duckdb::BaseStatistics>>> chunk_stats;
};

/// Pin-time validation of the coalescer invariant the MVCC delta merge relies on
/// (#819): every duckdb-native batch must be a contiguous run of WHOLE row groups
/// starting at @p rows_before_chunk, and its row-group metadata must cover exactly
/// @p chunk_rows (the decoded cudf row count) — so the pinned chunks partition the
/// decoded rowid prefix [0, N_cache) at row-group boundaries and per-chunk
/// visibility masks can be assembled at row-group granularity. Throws
/// std::runtime_error on violation; batches that are not duckdb_native_scan_info
/// (parquet) are skipped. Called by the pin materialization driver on every
/// emitted batch; exposed here so its failure paths are unit-testable.
void validate_duckdb_pin_chunk(const op::scan::scan_info& batch,
                               std::size_t chunk_rows,
                               std::size_t rows_before_chunk);

/// Drive @p ingestible 's metadata walk + batch coalescer to completion on @p io_ctx,
/// materializing every emitted batch into a GPU-resident cudf::table and round-robining
/// placement across @p gpu_spaces. Single-threaded with deterministic placement, so
/// re-pinning the same source yields identical @c chunk_memory_spaces (required by
/// @c sirius_scan_manager::insert_pinned_entry 's merge path).
///
/// \param ingestible          Source ingestible (parquet or duckdb-native). Consumed by repeated
///                            next_split_provider / materialize calls.
/// \param gpu_spaces          Non-empty set of GPU memory spaces to round-robin across.
/// \param io_ctx              IO context the metadata reads run on (owned by the scan manager).
/// \param pinned_column_types Pin-time DuckDB type of each batch column, in batch-column
///                            (column_ids) order — drives the per-chunk zone-map capture
///                            (compute_pinned_chunk_stats). Empty skips capture (statless pin).
materialized_pin materialize_all_batches(
  op::scan::gpu_ingestible& ingestible,
  std::span<cucascade::memory::memory_space* const> gpu_spaces,
  io::sirius_ioctx& io_ctx,
  duckdb::vector<duckdb::LogicalType> const& pinned_column_types);

/// Result of driving a host-tier pin with optional Simpatico compression.
/// A result may be wholly compressed, wholly uncompressed, or mixed; each batch
/// lands in exactly one arm according to eligibility, outcome, and ratio policy.
struct host_pin_result {
  std::vector<std::shared_ptr<cucascade::host_data_representation>> host_chunks;
  std::vector<std::shared_ptr<sirius::compressed_host_representation>> compressed_chunks;
  /// Per-logical-chunk arm+slot, in emission order (parallel to
  /// base_row_count_per_chunk). Populated only for a mixed result; homogeneous
  /// results leave it empty and use their sole non-empty storage arm.
  std::vector<sirius::scan_manager::chunk_source> logical_order;
  /// Row count of each materialized batch, in emission order (covers compressed
  /// and uncompressed chunks alike); becomes duckdb_mvcc_metadata::
  /// base_row_count_per_chunk for duckdb-format pins.
  std::vector<std::size_t> base_row_count_per_chunk;
  /// Per-chunk zone-map capture for the raw arm: chunk_stats[i] holds the per-column
  /// BaseStatistics of host_chunks[i] (positional with @c host_chunks, NOT with the
  /// compressed arm). Empty when zone-map capture was off (no column types supplied).
  /// A homogeneous uncompressed result feeds this straight into insert_pinned_entry_host;
  /// compressed and mixed results carry no zone maps, so it stays aligned with the raw arm.
  std::vector<std::vector<duckdb::unique_ptr<duckdb::BaseStatistics>>> chunk_stats;
};

/// Optional compression settings for @ref materialize_pin_to_host_with_compression
/// and @ref materialize_pin_to_device_with_compression.
struct compression_pin_config {
  bool enabled{false};
  std::string plan_dsl;
  std::size_t min_batch_size_bytes{0};
  /// Keep the compressed form only if header+payload <= this fraction of the
  /// batch's original device size; otherwise pin uncompressed. See
  /// compression_config::max_compressed_fraction. For a zero-byte original,
  /// only a zero-byte compressed form qualifies.
  double max_compressed_fraction{0.95};
  std::vector<std::string> column_names;
};

/// Result of driving a GPU-tier pin with optional Simpatico compression.
/// A result may be wholly compressed, wholly uncompressed, or mixed; raw tables
/// retain their per-chunk placement in @c tables / @c chunk_memory_spaces.
struct device_pin_result {
  std::vector<std::unique_ptr<cudf::table>> tables;
  std::vector<cucascade::memory::memory_space*> chunk_memory_spaces;
  std::vector<std::shared_ptr<sirius::compressed_device_representation>> compressed_chunks;
  /// Per-logical-chunk arm+slot, in emission order (parallel to
  /// base_row_count_per_chunk). Populated only for a mixed result; homogeneous
  /// results leave it empty and use their sole non-empty storage arm.
  std::vector<sirius::scan_manager::chunk_source> logical_order;
  /// Row count of each materialized batch, in emission order (covers compressed
  /// and uncompressed chunks alike); becomes duckdb_mvcc_metadata::
  /// base_row_count_per_chunk for duckdb-format pins.
  std::vector<std::size_t> base_row_count_per_chunk;
};

/// Drive @p ingestible to completion, streaming each emitted batch to pinned host memory
/// (peak GPU residency ~one batch), optionally compressing each batch with Simpatico first.
/// @p pinned_column_types (column_ids order) drives the raw arm's zone-map capture; empty
/// skips capture. Only uncompressed chunks receive statistics (positional with host_chunks).
host_pin_result materialize_pin_to_host_with_compression(
  op::scan::gpu_ingestible& ingestible,
  std::span<cucascade::memory::memory_space* const> gpu_spaces,
  const std::unordered_map<int, cucascade::memory::memory_space*>& host_space_by_gpu,
  io::sirius_ioctx& io_ctx,
  compression_pin_config const& compression,
  duckdb::vector<duckdb::LogicalType> const& pinned_column_types = {});

/// Drive @p ingestible to completion like @ref materialize_all_batches, optionally
/// compressing each batch with Simpatico and keeping the compressed payload in GPU
/// (device) memory. When compression is disabled or a batch does not qualify, the
/// uncompressed GPU table is retained instead (in @c device_pin_result::tables), so
/// the caller can fall back to the plain GPU pin.
device_pin_result materialize_pin_to_device_with_compression(
  op::scan::gpu_ingestible& ingestible,
  std::span<cucascade::memory::memory_space* const> gpu_spaces,
  io::sirius_ioctx& io_ctx,
  compression_pin_config const& compression);

}  // namespace sirius
