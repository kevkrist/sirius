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

// sirius
#include <data/data_batch_utils.hpp>
#include <data/host_parquet_representation.hpp>
#include <op/scan/parquet_scan_task.hpp>
#include <op/sirius_physical_parquet_scan.hpp>
#include <pipeline/sirius_pipeline.hpp>

// cucascade
#include <cucascade/data/data_batch.hpp>
#include <cucascade/data/data_repository.hpp>
#include <cucascade/memory/fixed_size_host_memory_resource.hpp>
#include <cucascade/memory/memory_reservation.hpp>
#include <cucascade/memory/memory_reservation_manager.hpp>

// duckdb
#include <duckdb/common/multi_file/multi_file_states.hpp>
#include <duckdb/common/types.hpp>
#include <duckdb/main/config.hpp>

// cudf
#include <cudf/ast/expressions.hpp>
#include <cudf/cudf_utils.hpp>
#include <cudf/io/datasource.hpp>
#include <cudf/io/experimental/hybrid_scan.hpp>
#include <cudf/io/parquet.hpp>
#include <cudf/io/parquet_schema.hpp>
#include <cudf/io/text/byte_range_info.hpp>
#if CUDF_VERSION_NUM >= 2604
#include <cudf/io/parquet_io_utils.hpp>
#endif

// standard library
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace sirius::op::scan {

// Define CUDF_NIGHTLY tp 1 if we're on a cuDF version new enough to support filter pushdown with
// the hybrid_scan_reader. This is not currently released in a cuDF version, so a CUDF_VERSION_NUM
// guard is not sufficient.
#define CUDF_NIGHTLY 1

#if CUDF_VERSION_NUM < 2604
namespace {
// Fallback for cudf < 26.04 which lacks cudf::io::parquet::fetch_footer_to_host.
// Reads the Parquet footer: last 8 bytes = [4-byte footer_len LE][4-byte "PAR1"],
// then reads footer_len bytes before that.
std::unique_ptr<cudf::io::datasource::buffer> fetch_footer_to_host_fallback(
  cudf::io::datasource& datasource)
{
  constexpr size_t PARQUET_MAGIC_SIZE = 4;
  constexpr size_t FOOTER_LEN_SIZE    = 4;
  constexpr size_t TAIL_SIZE          = PARQUET_MAGIC_SIZE + FOOTER_LEN_SIZE;

  auto const file_size = datasource.size();
  if (file_size < TAIL_SIZE + PARQUET_MAGIC_SIZE) {
    throw std::runtime_error("File too small to be a valid Parquet file");
  }

  // Read the last 8 bytes to get footer length
  auto tail_buf    = datasource.host_read(file_size - TAIL_SIZE, TAIL_SIZE);
  auto const* tail = tail_buf->data();

  // Footer length is a little-endian uint32 at offset 0
  uint32_t footer_len = tail[0] | (tail[1] << 8) | (tail[2] << 16) | (tail[3] << 24);

  // Read the footer bytes
  auto const footer_offset = file_size - TAIL_SIZE - footer_len;
  return datasource.host_read(footer_offset, footer_len);
}
}  // namespace
#endif

namespace detail {

bool projected_columns_are_flat(cudf::io::parquet::FileMetaData const& meta,
                                std::vector<size_t> const& selected_column_indices)
{
  // Empty files are effectively "flat" for our purposes here.
  if (meta.row_groups.empty()) { return true; }
  auto const& cols = meta.row_groups.front().columns;

  // Flat leaf column => path length == 1.
  // For projections, we only need this property to hold for the projected (selected) leaf columns.
  return std::all_of(
    selected_column_indices.begin(), selected_column_indices.end(), [&cols](auto col_idx) {
      return col_idx < cols.size() && cols[col_idx].meta_data.path_in_schema.size() == 1;
    });
}

#if CUDF_NIGHTLY
// For filter pushdown with hybrid_scan_reader, we need to add pure filter columns to the column set
// in the reader options.
std::tuple<std::vector<size_t>, std::vector<size_t>> make_selected_column_indices(
  sirius_physical_parquet_scan const& scan_op, std::vector<duckdb::idx_t> const& filter_ids)
{
  // Deduplication set
  std::unordered_set<size_t> seen;
  std::vector<size_t> selected_column_indices;

  // In case there are duplicate columns in the projection list, we deduplicate, in order
  auto push_unique = [&selected_column_indices, &seen](auto col_idx) {
    if (duckdb::IsVirtualColumn(col_idx)) { return; }
    if (seen.insert(col_idx).second) {
      // Insert successful (not yet seen)
      selected_column_indices.push_back(col_idx);
    }
  };

  if (scan_op.projection_ids.empty()) {
    //===----------No Projection: Select All Columns----------===//
    std::for_each(scan_op.column_ids.begin(),
                  scan_op.column_ids.end(),
                  [&push_unique](duckdb::ColumnIndex const& column_id) {
                    push_unique(column_id.GetPrimaryIndex());
                  });
    return {selected_column_indices, {}};
  }

  //===----------Projection Applied: Select Projected Columns Only----------===//
  // Collect the set of column_ids positions that are referenced by projection_ids,
  // then iterate in column_ids order (not projection_ids order).
  // This ensures the parquet reader produces columns in the same order that
  // the TABLE_SCAN filter expects (column_ids order), since the filter's
  // BoundReferenceExpression indices are offsets into column_ids.
  //
  // filter_ids contains positions in column_ids (same as projection_ids).
  // Pure filter columns are those referenced by the filter but absent from the projection.
  // We track their output positions in selected_column_indices so the converter can prune them.
  std::unordered_set<duckdb::idx_t> projected_set(scan_op.projection_ids.begin(),
                                                  scan_op.projection_ids.end());
  std::unordered_set<duckdb::idx_t> pure_filter_set;
  for (auto filter_pos : filter_ids) {
    if (!projected_set.contains(filter_pos)) { pure_filter_set.insert(filter_pos); }
  }

  std::vector<size_t> pure_filter_output_positions;
  for (duckdb::idx_t i = 0; i < scan_op.column_ids.size(); i++) {
    bool const in_projection = projected_set.contains(i);
    bool const is_pure_filter = pure_filter_set.contains(i);
    if (in_projection || is_pure_filter) {
      auto const output_pos = selected_column_indices.size();
      push_unique(scan_op.column_ids[i].GetPrimaryIndex());
      // Record the output position only if the column was actually inserted (not a duplicate)
      if (is_pure_filter && selected_column_indices.size() > output_pos) {
        pure_filter_output_positions.push_back(output_pos);
      }
    }
  }
  return {selected_column_indices, pure_filter_output_positions};
}
#else
std::vector<size_t> make_selected_column_indices(sirius_physical_parquet_scan const& scan_op)
{
  // Deduplication set
  std::unordered_set<size_t> seen;
  std::vector<size_t> selected_column_indices;

  // In case there are duplicate columns in the projection list, we deduplicate, in order
  auto push_unique = [&selected_column_indices, &seen](auto col_idx) {
    if (duckdb::IsVirtualColumn(col_idx)) { return; }
    if (seen.insert(col_idx).second) {
      // Insert successful (not yet seen)
      selected_column_indices.push_back(col_idx);
    }
  };

  if (scan_op.projection_ids.empty()) {
    //===----------No Projection: Select All Columns----------===//
    std::for_each(scan_op.column_ids.begin(),
                  scan_op.column_ids.end(),
                  [&push_unique](duckdb::ColumnIndex const& column_id) {
                    push_unique(column_id.GetPrimaryIndex());
                  });
    return selected_column_indices;
  }

  //===----------Projection Applied: Select Projected Columns Only----------===//
  // Collect the set of column_ids indices that are referenced by projection_ids,
  // then iterate in column_ids order (not projection_ids order).
  // This ensures the parquet reader produces columns in the same order that
  // the TABLE_SCAN filter expects (column_ids order), since the filter's
  // BoundReferenceExpression indices are offsets into column_ids.
  std::unordered_set<duckdb::idx_t> projected_set(scan_op.projection_ids.begin(),
                                                  scan_op.projection_ids.end());
  for (duckdb::idx_t i = 0; i < scan_op.column_ids.size(); i++) {
    if (projected_set.count(i)) { push_unique(scan_op.column_ids[i].GetPrimaryIndex()); }
  }
  return selected_column_indices;
}
#endif

std::vector<byte_range_info> merge_byte_ranges(std::vector<byte_range_info> const& byte_ranges)
{
  if (byte_ranges.empty()) { return {}; }

  std::vector<byte_range_info> merged;
  merged.reserve(byte_ranges.size());

  auto current_start = byte_ranges[0].offset();
  auto current_end   = current_start + byte_ranges[0].size();

  for (auto const& range : byte_ranges) {
    auto const range_start = range.offset();
    auto const range_end   = range_start + range.size();

    if (range_start <= current_end) {
      // Ranges overlap or are contiguous, extend the current range
      current_end = std::max(current_end, range_end);
    } else {
      // No overlap, push the current range and start a new one
      merged.emplace_back(current_start, current_end - current_start);
      current_start = range_start;
      current_end   = range_end;
    }
  }
  // Push the final range
  merged.emplace_back(current_start, current_end - current_start);

  return merged;
}

}  // namespace detail

//===----------------------------------------------------------------------===//
// Parquet Scan Task Global State
//===----------------------------------------------------------------------===//
parquet_scan_task_global_state::parquet_scan_task_global_state(
  duckdb::shared_ptr<pipeline::sirius_pipeline> pipeline,
  sirius_physical_parquet_scan* scan_op,
  size_t approximate_batch_size)
  : pipeline::sirius_pipeline_task_global_state(pipeline),
    _scan_op(scan_op),
    _approximate_batch_size(approximate_batch_size)
{
  if (scan_op->function.in_out_function) {
    throw std::runtime_error(
      "[parquet_scan_task_global_state] In-out table functions are not supported in sirius "
      "parquet scans.");
  }

  // Filter pushdown is not supported
  if (scan_op->dynamic_filters) {
    throw std::runtime_error(
      "[parquet_scan_task_global_state] Dynamic table filters are not supported in sirius "
      "parquet scans.");
  }

  // Expect parquet_scan to be bound through the multi-file reader
  auto& bind_data = scan_op->bind_data->Cast<duckdb::MultiFileBindData>();
  if (!bind_data.file_list || bind_data.file_list->IsEmpty()) {
    throw std::runtime_error("[parquet_scan_task_global_state] No input files to scan");
  }
  auto files = bind_data.file_list->GetAllFiles();
  _file_paths.reserve(files.size());
  std::for_each(
    files.begin(), files.end(), [this](auto const& file) { _file_paths.push_back(file.path); });

  // Construct the io_sources and read the footers
  std::vector<std::unique_ptr<cudf::io::datasource>> datasources;
  std::vector<std::unique_ptr<cudf::io::datasource::buffer>> footer_buffers;
  datasources.reserve(files.size());
  footer_buffers.reserve(files.size());
  std::for_each(
    _file_paths.begin(), _file_paths.end(), [&datasources, &footer_buffers](auto const& file_path) {
      auto datasource = cudf::io::datasource::create(file_path);
      datasources.push_back(std::move(datasource));
#if CUDF_VERSION_NUM >= 2604
      footer_buffers.push_back(cudf::io::parquet::fetch_footer_to_host(*datasources.back()));
#else
      footer_buffers.push_back(fetch_footer_to_host_fallback(*datasources.back()));
#endif
    });

  // Initialize reader options for applying projections
  _reader_options = cudf::io::parquet_reader_options::builder().build();

// If filtering or projecting with hybrid_scan_reader, we need column names
#if CUDF_NIGHTLY
  bool const try_filter = scan_op->table_filters && !scan_op->table_filters->filters.empty();
#else
  // cuDF has a bug in filter pushdown with decimal columns. Fixed by this PR:
  // https://github.com/rapidsai/cudf/pull/21447
  bool const try_filter = false;
#endif
  bool const is_projected = !scan_op->projection_ids.empty();
  if (is_projected || try_filter) {
    if (scan_op->names.empty()) {
      throw std::runtime_error(
        "[parquet_scan_task_global_state] Cannot apply projection or filter: scan has no column "
        "names");
    }
  }

#if CUDF_NIGHTLY
  // Try to apply table filter
  std::vector<duckdb::idx_t> filter_idxs;  // Positions in column_ids referenced by the filter
  if (try_filter) {
    auto duckdb_expr = _scan_op->get_table_filter_expression();
    if (duckdb_expr) {
      // Name resolver: BoundReferenceExpression::index is a position in column_ids.
      // We collect ref_index (not primary_idx) to match with domain of projection_ids.
      auto name_resolver = [&scan_op, &filter_idxs](duckdb::idx_t ref_index) -> std::string {
        auto const primary_idx = scan_op->column_ids[ref_index].GetPrimaryIndex();
        filter_idxs.push_back(ref_index);
        return scan_op->names[primary_idx];
      };
      gpu_expression_translator translator(rmm::cuda_stream_default,
                                           cudf::get_current_device_resource_ref());
      auto translated = translator.translate_expression_with_names(*duckdb_expr, name_resolver);
      if (translated) {
        _translated_filter = std::make_shared<gpu_expression_translator::translated_expression>(
          std::move(*translated));
        _reader_options.set_filter(_translated_filter->back());
      }
    }
  }

  // Get the selected column indices for projection
  auto [selected_column_indices, pure_filter_positions] =
    detail::make_selected_column_indices(*scan_op, filter_idxs);
  _pure_filter_ids = std::move(pure_filter_positions);
  std::unordered_set<size_t> pure_filter_pos_set(_pure_filter_ids.begin(),
                                                 _pure_filter_ids.end());
#else
  auto selected_column_indices = detail::make_selected_column_indices(*scan_op);
#endif

  // Apply projections by column name using DuckDB's bound column names.
  if (is_projected) {
    std::vector<std::string> selected_columns;
    std::for_each(selected_column_indices.begin(),
                  selected_column_indices.end(),
                  [&scan_op, &selected_columns](size_t col_idx) {
                    selected_columns.push_back(scan_op->names[col_idx]);
                  });
#if CUDF_VERSION_NUM >= 2604
    _reader_options.set_column_names(std::move(selected_columns));
#else
    _reader_options.set_columns(std::move(selected_columns));
#endif
  }

  // Construct the file readers and parse the metadata
  std::vector<std::unique_ptr<cudf::io::parquet::experimental::hybrid_scan_reader>> readers;
  _file_metadatas.reserve(files.size());
  readers.reserve(files.size());
  std::for_each(footer_buffers.begin(),
                footer_buffers.end(),
                [&readers, is_projected, &selected_column_indices, this](auto& footer_buffer) {
                  auto reader =
                    std::make_unique<cudf::io::parquet::experimental::hybrid_scan_reader>(
                      cudf::host_span<uint8_t const>(footer_buffer->data(), footer_buffer->size()),
                      _reader_options);
                  auto meta = reader->parquet_metadata();
                  if (is_projected) {
                    // We currently only support flat schemas for parquet scans with projections.
                    // This is only because we need the set of needed column indices for
                    // partitioning the row groups, and determining the full set of column indices
                    // for a nested type is more complex.
                    /// TODO: Support nested schemas for projected scans
                    if (!detail::projected_columns_are_flat(meta, selected_column_indices)) {
                      throw std::runtime_error(
                        "[parquet_scan_task_global_state] Parquet scans with projections currently "
                        "only support "
                        "flat projected columns");
                    }
                  }
                  _file_metadatas.push_back(std::move(meta));
                  readers.push_back(std::move(reader));
                });

  // Partition the row groups
  for (size_t file_idx = 0; file_idx < _file_paths.size(); ++file_idx) {
    auto row_group_indices = readers[file_idx]->all_row_groups(_reader_options);
    if (_translated_filter) {
      SIRIUS_LOG_INFO(
        "[parquet_scan_task_global_state] Before filtering, file {} has {} row groups",
        _file_paths[file_idx],
        row_group_indices.size());
      row_group_indices = readers[file_idx]->filter_row_groups_with_stats(
        row_group_indices, _reader_options, rmm::cuda_stream_default);
      SIRIUS_LOG_INFO("[parquet_scan_task_global_state] After filtering, file {} has {} row groups",
                      _file_paths[file_idx],
                      row_group_indices.size());
    }
    auto const& file_metadata = _file_metadatas[file_idx];

    size_t partition_uncompressed_bytes = 0;
    size_t partition_compressed_bytes   = 0;
    std::vector<cudf::size_type> partition_rg_indices;
    partition_rg_indices.reserve(row_group_indices.size());

    auto flush_partition = [&]() {
      if (partition_rg_indices.empty()) { return; }
      _row_group_partitions.emplace_back(file_idx,
                                         std::move(partition_rg_indices),
                                         partition_uncompressed_bytes,
                                         partition_compressed_bytes);
      partition_rg_indices.clear();
      partition_uncompressed_bytes = 0;
      partition_compressed_bytes   = 0;
    };

    for (auto const rg_idx : row_group_indices) {
      auto const& row_group = file_metadata.row_groups[rg_idx];
      partition_rg_indices.push_back(rg_idx);

      for (size_t selected_pos = 0; selected_pos < selected_column_indices.size(); ++selected_pos) {
        auto const col_idx          = selected_column_indices[selected_pos];
        auto const& column_metadata = row_group.columns[col_idx].meta_data;
        // To reflect the fact that pure filter columns are not part of the decompression result,
        // we omit them from the uncompressed byte count.
        if (column_metadata.total_uncompressed_size > 0 &&
            !pure_filter_pos_set.contains(selected_pos)) {
          partition_uncompressed_bytes +=
            static_cast<size_t>(column_metadata.total_uncompressed_size);
        }
        if (column_metadata.total_compressed_size > 0) {
          partition_compressed_bytes += static_cast<size_t>(column_metadata.total_compressed_size);
        }
      }

      if (partition_uncompressed_bytes >= _approximate_batch_size) { flush_partition(); }
    }

    // Emit any trailing partition smaller than the target size.
    flush_partition();
  }
}

//===----------------------------------------------------------------------===//
// Parquet Scan Task Local State
//===----------------------------------------------------------------------===//
parquet_scan_task_local_state::parquet_scan_task_local_state(
  parquet_scan_task_global_state::row_group_range partition)
  : _partition(std::move(partition))
{
}

std::unique_ptr<parquet_scan_task_local_state::multiple_blocks_allocation>
parquet_scan_task_local_state::make_allocation()
{
  auto& mem_space = _reservation->get_memory_space();
  auto* allocator =
    mem_space.get_memory_resource_as<cucascade::memory::fixed_size_host_memory_resource>();
  if (allocator == nullptr) {
    throw std::runtime_error(
      "[parquet_scan_task_local_state] Failed to get fixed_size_host_memory_resource allocator "
      "for HOST memory space");
  }
  return allocator->allocate_multiple_blocks(get_reserved_compressed_bytes(), _reservation.get());
}

parquet_scan_task::~parquet_scan_task()
{
  if (_global_state != nullptr) {
    auto& g_state = this->_global_state->cast<parquet_scan_task_global_state>();
    if (auto pipeline = g_state.get_operator().get_pipeline()) { pipeline->mark_task_completed(); }
  }
}
//===----------------------------------------------------------------------===//
// Parquet Scan Task
//===----------------------------------------------------------------------===//
std::unique_ptr<op::operator_data> parquet_scan_task::compute_task(
  rmm::cuda_stream_view /* stream */)
{
  auto& l_state = this->_local_state->cast<parquet_scan_task_local_state>();
  auto& g_state = this->_global_state->cast<parquet_scan_task_global_state>();
  auto reader   = g_state.make_reader(l_state.get_file_idx());

  // Make the allocation and accessor
  auto allocation = l_state.make_allocation();
  memory::multiple_blocks_allocation_accessor<uint8_t> data_accessor;
  data_accessor.initialize(0, allocation);

  // Get the byte ranges for the range of row groups assigned to this task.
  auto byte_ranges =
    reader->all_column_chunks_byte_ranges(l_state.get_rg_indices(), g_state.get_options());
  std::cout << "\n\tBYTE RANGES READ\n" << std::endl;

  // Read each byte range into the allocation asynchronously
  std::vector<cudf::io::text::byte_range_info> new_byte_ranges;
  new_byte_ranges.reserve(byte_ranges.size());
  int64_t new_offset = 0;
  std::vector<std::future<std::size_t>> read_futures;
  for (auto const& range : byte_ranges) {
    read_range_into_allocation(
      range.offset(), range.size(), data_accessor, allocation, read_futures);
    new_byte_ranges.emplace_back(new_offset, range.size());
    new_offset += range.size();
  }
  std::for_each(read_futures.begin(), read_futures.end(), [](auto& future) { future.get(); });

  if (new_offset != l_state.get_reserved_compressed_bytes()) {
    // Metadata / file data mismatch
    throw std::runtime_error(
      "[parquet_scan_task] Error in reading byte ranges: total bytes read does not match reserved "
      "compressed bytes");
  }

  // Create a data batch with the column chunks
  auto parquet_representation =
    std::make_unique<host_parquet_representation>(l_state.get_memory_space(),
                                                  std::move(allocation),
                                                  std::move(reader),
                                                  g_state.get_options(),
                                                  std::move(l_state.get_rg_indices()),
                                                  std::move(new_byte_ranges),
                                                  l_state.get_reserved_compressed_bytes(),
                                                  l_state.get_reserved_uncompressed_bytes(),
                                                  g_state.get_filter_expression_pin(),
                                                  g_state.get_pure_filter_ids());
  auto data_batch =
    std::make_shared<cucascade::data_batch>(get_next_batch_id(), std::move(parquet_representation));
  return std::make_unique<op::operator_data>(
    std::vector<std::shared_ptr<cucascade::data_batch>>{data_batch});
}

void parquet_scan_task::publish_output(op::operator_data& output_data,
                                       rmm::cuda_stream_view /* stream */)
{
  for (auto& batch : output_data.get_data_batches()) {
    _data_repo->add_data_batch(std::move(batch));
  }
}

void parquet_scan_task::read_range_into_allocation(
  size_t file_offset,
  size_t n_bytes,
  multiple_blocks_allocation_accessor& data_blocks_accessor,
  std::unique_ptr<multiple_blocks_allocation>& allocation,
  std::vector<std::future<std::size_t>>& read_futures)
{
  auto remaining_bytes = n_bytes;
  auto current_offset  = file_offset;

  while (remaining_bytes > 0) {
    auto const bytes_to_read = std::min(
      remaining_bytes, data_blocks_accessor.block_size - data_blocks_accessor.offset_in_block);
    auto buffer_ptr =
      reinterpret_cast<uint8_t*>(allocation->get_blocks()[data_blocks_accessor.block_index]) +
      data_blocks_accessor.offset_in_block;
    read_futures.push_back(_datasource->host_read_async(current_offset, bytes_to_read, buffer_ptr));
    remaining_bytes -= bytes_to_read;
    current_offset += bytes_to_read;
    data_blocks_accessor.offset_in_block += bytes_to_read;
    // Do we need to advance to the next block?
    if (data_blocks_accessor.offset_in_block == data_blocks_accessor.block_size) {
      ++data_blocks_accessor.block_index;
      data_blocks_accessor.offset_in_block = 0;
    }
  }
}

}  // namespace sirius::op::scan
