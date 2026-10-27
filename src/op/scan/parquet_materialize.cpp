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

#include "op/scan/parquet_materialize.hpp"

#include "io/io_context.hpp"
#include "io/types.hpp"

#include <cudf/io/datasource.hpp>
#include <cudf/io/experimental/hybrid_scan_multifile.hpp>
#include <cudf/utilities/error.hpp>
#include <cudf/utilities/span.hpp>

#include <rmm/device_buffer.hpp>

#include <cuda_runtime_api.h>

#include <algorithm>
#include <exception>
#include <future>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace sirius::op::scan {

using hybrid_scan_reader = cudf::io::parquet::experimental::hybrid_scan_reader;

std::vector<cudf::io::text::byte_range_info> column_chunk_ranges(
  cudf::io::parquet::FileMetaData const& metadata,
  cudf::io::parquet_reader_options const& options,
  std::vector<cudf::size_type> const& row_group_indices)
{
  if (row_group_indices.empty()) { return {}; }
  hybrid_scan_reader reader(metadata, options);
  return reader.all_column_chunks_byte_ranges(
    cudf::host_span<cudf::size_type const>(row_group_indices.data(), row_group_indices.size()),
    options);
}

std::vector<cudf::io::text::byte_range_info> column_chunk_range_cache::ranges(
  std::shared_ptr<cudf::io::parquet::FileMetaData const> const& metadata,
  std::shared_ptr<cudf::io::parquet_reader_options const> const& options,
  std::vector<cudf::size_type> const& row_group_indices)
{
  if (row_group_indices.empty()) { return {}; }
  if (!_reader || _metadata != metadata || _options != options) {
    _reader.reset();
    _reader   = std::make_unique<hybrid_scan_reader>(*metadata, *options);
    _metadata = metadata;
    _options  = options;
  }
  return _reader->all_column_chunks_byte_ranges(
    cudf::host_span<cudf::size_type const>(row_group_indices.data(), row_group_indices.size()),
    *options);
}

void column_chunk_range_cache::clear() noexcept
{
  _reader.reset();
  _metadata.reset();
  _options.reset();
}

bool prefers_bulk_materialize(std::span<parquet_source const> sources,
                              cudf::io::parquet_reader_options const& options)
{
  if (sources.empty()) { return false; }
  cudf::io::parquet_reader_options const defaults;
  if (options.get_skip_rows() != 0 || options.get_num_rows() || options.get_skip_bytes() != 0 ||
      options.get_num_bytes() || !options.get_row_groups().empty() ||
      options.get_column_indices() || options.get_column_field_ids() ||
      options.get_column_schema() ||
      options.get_timestamp_type() != defaults.get_timestamp_type() ||
      options.get_decimal_width() != defaults.get_decimal_width() ||
      options.is_enabled_convert_strings_to_categories() !=
        defaults.is_enabled_convert_strings_to_categories() ||
      options.is_enabled_use_pandas_metadata() != defaults.is_enabled_use_pandas_metadata() ||
      options.is_enabled_use_arrow_schema() != defaults.is_enabled_use_arrow_schema() ||
      options.is_enabled_ignore_missing_columns() != defaults.is_enabled_ignore_missing_columns() ||
      options.is_enabled_case_sensitive_names() != defaults.is_enabled_case_sensitive_names() ||
      options.is_enabled_use_jit_filter() != defaults.is_enabled_use_jit_filter() ||
      options.is_enabled_allow_mismatched_pq_schemas() !=
        defaults.is_enabled_allow_mismatched_pq_schemas() ||
      options.is_enabled_prepend_source_index_column() ||
      options.is_enabled_prepend_row_index_column()) {
    return false;
  }
  return std::all_of(sources.begin(), sources.end(), [](parquet_source const& src) {
    return src.datasource && src.metadata && !src.row_group_indices.empty() &&
           (src.datasource->prefers_bulk_io() || src.datasource->uses_prefetching_cache());
  });
}

namespace {

char const* physical_type_name(cudf::io::parquet::Type type)
{
  switch (type) {
    case cudf::io::parquet::Type::BOOLEAN: return "BOOLEAN";
    case cudf::io::parquet::Type::INT32: return "INT32";
    case cudf::io::parquet::Type::INT64: return "INT64";
    case cudf::io::parquet::Type::INT96: return "INT96";
    case cudf::io::parquet::Type::FLOAT: return "FLOAT";
    case cudf::io::parquet::Type::DOUBLE: return "DOUBLE";
    case cudf::io::parquet::Type::BYTE_ARRAY: return "BYTE_ARRAY";
    case cudf::io::parquet::Type::FIXED_LEN_BYTE_ARRAY: return "FIXED_LEN_BYTE_ARRAY";
    default: return "group";
  }
}

std::string describe(cudf::io::parquet::SchemaElement const& element)
{
  return "'" + element.name + "' " + physical_type_name(element.type);
}

std::string path_of(parquet_source const& src)
{
  return src.datasource ? src.datasource->get_io_object().object_path() : std::string{"<unknown>"};
}

std::size_t padded_chunk_size(std::size_t size)
{
  constexpr std::size_t padding = 8;
  if (size > std::numeric_limits<std::size_t>::max() - (padding - 1)) {
    throw std::overflow_error("parquet column chunk allocation size overflow");
  }
  return (size + padding - 1) & ~(padding - 1);
}

/**
 * Logical byte total of `ranges`, rejecting a negative range and an end offset or total that
 * overflows. Rounding the total is a conservative guard for the per-run allocations: every run is a
 * subset of these chunks, so a total that rounds up without overflow guarantees that every run
 * does.
 */
std::size_t checked_total_bytes(std::span<cudf::io::text::byte_range_info const> ranges)
{
  std::size_t total = 0;
  for (auto const& range : ranges) {
    if (range.offset() < 0 || range.size() < 0) {
      throw std::invalid_argument("negative parquet column chunk range");
    }
    auto const size   = static_cast<std::size_t>(range.size());
    auto const offset = static_cast<std::size_t>(range.offset());
    if (size > std::numeric_limits<std::size_t>::max() - offset ||
        size > std::numeric_limits<std::size_t>::max() - total) {
      throw std::overflow_error("parquet column chunk byte count overflow");
    }
    total += size;
  }
  static_cast<void>(padded_chunk_size(total));
  return total;
}

/**
 * Owns all demand-read destinations through settlement and hybrid decode. Preparation publishes no
 * reads; destruction drains every remaining future before releasing buffers. Within one row group,
 * chunks that follow each other both in decode order and in the file share one allocation, laid
 * out back to back with cuDF's eight-byte padding at the end only, and are fetched by a single
 * read; decoder spans keep logical lengths. Per-chunk allocations and tail fills cost several
 * hundred extra CUDA calls per task on already saturated worker threads, while one allocation per
 * file measured just as slow, so runs are bounded to a row group. The caller retains the filter
 * AST and current device, stream and allocator for this operation's lifetime.
 */
class bulk_read_operation {
 public:
  bulk_read_operation(std::span<parquet_source const> sources,
                      cudf::io::parquet_reader_options const& options,
                      std::span<std::vector<cudf::io::text::byte_range_info> const> ranges,
                      ::cuda::stream_ref stream,
                      rmm::device_async_resource_ref mr)
    : _options{options}, _stream{stream}, _mr{mr}
  {
    require_same_parquet_schema(sources);
    select_sources(sources, ranges);
    if (_files.empty()) { return; }
    std::size_t span_count = 0;
    for (auto& file : _files) {
      file.expected_bytes = checked_total_bytes(file.ranges);
      if (file.ranges.size() > std::numeric_limits<std::size_t>::max() - span_count) {
        throw std::overflow_error("parquet column chunk count overflow");
      }
      span_count += file.ranges.size();
    }
    _groups.reserve(_files.size());
    _spans.reserve(span_count);
    for (auto& file : _files) {
      _groups.push_back(file.source.row_group_indices);
      lay_out(file);
    }
  }

  ~bulk_read_operation() noexcept
  {
    for (auto& file : _files) {
      if (file.completion.valid()) {
        try {
          static_cast<void>(file.completion.get());
        } catch (...) {
        }
      }
    }
  }

  bulk_read_operation(bulk_read_operation const&)            = delete;
  bulk_read_operation& operator=(bulk_read_operation const&) = delete;

  [[nodiscard]] bool empty() const noexcept { return _files.empty(); }

  void issue_and_settle()
  {
    std::exception_ptr error;
    try {
      for (auto& file : _files) {
        if (!file.reads.empty()) {
          file.completion = file.source.datasource->device_read_ranges_async(file.reads, _stream);
        }
      }
    } catch (...) {
      error = std::current_exception();
    }
    for (auto& file : _files) {
      if (!file.completion.valid()) { continue; }
      try {
        if (file.completion.get() != file.expected_bytes) {
          throw std::runtime_error("short parquet column chunk read");
        }
      } catch (...) {
        if (!error) { error = std::current_exception(); }
      }
    }
    if (error) { std::rethrow_exception(error); }
  }

  [[nodiscard]] std::unique_ptr<cudf::table> decode()
  {
    auto spans =
      cudf::host_span<cudf::device_span<std::uint8_t const> const>{_spans.data(), _spans.size()};
    if (_multifile_reader) {
      auto result = _multifile_reader->materialize_all_columns(
        cudf::host_span<std::vector<cudf::size_type> const>{_groups.data(), _groups.size()},
        spans,
        _options,
        _stream,
        _mr);
      return std::move(result.tbl);
    }
    auto result = _files.front().reader->materialize_all_columns(
      _groups.front(), spans, _options, _stream, _mr);
    return std::move(result.tbl);
  }

 private:
  struct file_read {
    parquet_source source;
    std::vector<cudf::io::text::byte_range_info> ranges;
    std::unique_ptr<hybrid_scan_reader> reader;
    std::vector<rmm::device_buffer> buffers;
    std::vector<io::slice> reads;
    std::size_t expected_bytes{0};
    std::future<std::size_t> completion;
  };

  /**
   * Prune `sources` with the final filter, choose the reader and fix each survivor's column chunk
   * ranges, appending one `file_read` per survivor to `_files` in source order. A multi-source task
   * uses one multifile reader for pruning, range enumeration and decode; it is rebuilt over the
   * survivors when a source loses all its row groups, and a single survivor falls through to the
   * single-file reader. Leaves `_files` empty when no row group survives.
   */
  void select_sources(std::span<parquet_source const> sources,
                      std::span<std::vector<cudf::io::text::byte_range_info> const> ranges)
  {
    bool const filtered = _options.get_filter().has_value();
    std::vector<parquet_source> selected(sources.begin(), sources.end());
    std::unique_ptr<hybrid_scan_reader> single_reader;
    auto groups_of_selected = [&] {
      std::vector<std::vector<cudf::size_type>> groups;
      groups.reserve(selected.size());
      for (auto const& src : selected) {
        groups.push_back(src.row_group_indices);
      }
      return groups;
    };
    // The multifile reader keeps its own footer copies, so this staging vector is needed only while
    // constructing it; it outlives the first construction so a rebuild over the surviving sources
    // does not copy the footers from the sources again.
    std::vector<cudf::io::parquet::FileMetaData> metadatas;
    auto build_multifile = [&] {
      return std::make_unique<cudf::io::parquet::experimental::hybrid_scan_multifile>(
        cudf::host_span<cudf::io::parquet::FileMetaData const>{metadatas.data(), metadatas.size()},
        _options);
    };
    if (selected.size() > 1) {
      metadatas.reserve(selected.size());
      for (auto const& src : selected) {
        metadatas.push_back(*src.metadata);
      }
      _multifile_reader = build_multifile();
      if (filtered) {
        auto const groups    = groups_of_selected();
        auto const surviving = _multifile_reader->filter_row_groups_with_stats(
          cudf::host_span<std::vector<cudf::size_type> const>{groups.data(), groups.size()},
          _options,
          _stream);
        if (surviving.size() != selected.size()) {
          throw std::runtime_error("parquet statistics pruning returned the wrong source count");
        }
        std::vector<parquet_source> kept;
        std::vector<cudf::io::parquet::FileMetaData> kept_metadatas;
        for (std::size_t i = 0; i < selected.size(); ++i) {
          if (surviving[i].empty()) { continue; }
          selected[i].row_group_indices = surviving[i];
          kept.push_back(std::move(selected[i]));
          kept_metadatas.push_back(std::move(metadatas[i]));
        }
        bool const rebuild = kept.size() != selected.size();
        selected           = std::move(kept);
        metadatas          = std::move(kept_metadatas);
        if (rebuild) {
          _multifile_reader.reset();
          if (selected.size() > 1) { _multifile_reader = build_multifile(); }
        }
      }
    } else if (filtered) {
      single_reader = std::make_unique<hybrid_scan_reader>(*selected.front().metadata, _options);
      auto& groups  = selected.front().row_group_indices;
      groups        = single_reader->filter_row_groups_with_stats(groups, _options, _stream);
      if (groups.empty()) { selected.clear(); }
    }
    if (selected.empty()) { return; }

    // Cached ranges are valid only for an unfiltered task that supplies one nonempty entry per
    // source; every filtered task recomputes them from the surviving groups.
    bool const reuse_ranges = !filtered && ranges.size() == sources.size() &&
                              std::ranges::all_of(ranges, [](auto const& r) { return !r.empty(); });
    if (selected.size() == 1) {
      _multifile_reader.reset();
      file_read file;
      file.source = std::move(selected.front());
      file.reader = single_reader
                      ? std::move(single_reader)
                      : std::make_unique<hybrid_scan_reader>(*file.source.metadata, _options);
      file.ranges = reuse_ranges ? ranges.front()
                                 : file.reader->all_column_chunks_byte_ranges(
                                     file.source.row_group_indices, _options);
      _files.push_back(std::move(file));
      return;
    }
    std::vector<std::vector<cudf::io::text::byte_range_info>> per_source;
    if (reuse_ranges) {
      per_source.assign(ranges.begin(), ranges.end());
    } else {
      auto const groups      = groups_of_selected();
      auto [flat, source_of] = _multifile_reader->all_column_chunks_byte_ranges(
        cudf::host_span<std::vector<cudf::size_type> const>{groups.data(), groups.size()},
        _options);
      per_source.resize(selected.size());
      for (std::size_t k = 0; k < flat.size(); ++k) {
        auto const s = static_cast<std::size_t>(source_of[k]);
        if (s >= selected.size()) { throw std::runtime_error("parquet chunk source out of range"); }
        per_source[s].push_back(flat[k]);
      }
    }
    _files.reserve(selected.size());
    for (std::size_t i = 0; i < selected.size(); ++i) {
      file_read file;
      file.source = std::move(selected[i]);
      file.ranges = std::move(per_source[i]);
      _files.push_back(std::move(file));
    }
  }

  /**
   * Allocate `file`'s destinations: one padded buffer and one read per maximal run of chunks that
   * are adjacent both in decode order and in the file within one row group, appending the logical
   * span of every chunk to `_spans` in decode order. The reader lists chunks row group by row group
   * in projection order, so a projection whose order differs from the file's simply yields more
   * runs.
   */
  void lay_out(file_read& file)
  {
    auto const groups = file.source.row_group_indices.size();
    if (groups == 0 || file.ranges.size() % groups != 0) {
      throw std::runtime_error("parquet column chunk count does not match the selected row groups");
    }
    auto const chunks_per_group = file.ranges.size() / groups;
    file.buffers.reserve(file.ranges.size());
    file.reads.reserve(file.ranges.size());
    std::size_t first = 0;
    while (first < file.ranges.size()) {
      auto const group_end  = first - first % chunks_per_group + chunks_per_group;
      std::size_t next      = first + 1;
      std::size_t run_bytes = static_cast<std::size_t>(file.ranges[first].size());
      auto const run_offset = static_cast<std::size_t>(file.ranges[first].offset());
      while (next < group_end &&
             static_cast<std::size_t>(file.ranges[next].offset()) == run_offset + run_bytes) {
        run_bytes += static_cast<std::size_t>(file.ranges[next].size());
        ++next;
      }
      auto& buffer     = file.buffers.emplace_back(padded_chunk_size(run_bytes), _stream, _mr);
      auto* const base = static_cast<std::uint8_t*>(buffer.data());
      if (buffer.size() != run_bytes) {
        CUDF_CUDA_TRY(
          cudaMemsetAsync(base + run_bytes, 0, buffer.size() - run_bytes, _stream.get()));
      }
      std::size_t placed = 0;
      for (std::size_t chunk = first; chunk < next; ++chunk) {
        auto const size = static_cast<std::size_t>(file.ranges[chunk].size());
        _spans.emplace_back(base + placed, size);
        placed += size;
      }
      if (run_bytes != 0) { file.reads.emplace_back(run_offset, run_bytes, base); }
      first = next;
    }
  }

  cudf::io::parquet_reader_options const& _options;
  ::cuda::stream_ref _stream;
  rmm::device_async_resource_ref _mr;
  std::vector<file_read> _files;
  std::vector<std::vector<cudf::size_type>> _groups;
  std::vector<cudf::device_span<std::uint8_t const>> _spans;
  std::unique_ptr<cudf::io::parquet::experimental::hybrid_scan_multifile> _multifile_reader;
};

std::unique_ptr<cudf::table> materialize_bulk(
  std::span<parquet_source const> sources,
  cudf::io::parquet_reader_options const& options,
  std::span<std::vector<cudf::io::text::byte_range_info> const> ranges,
  ::cuda::stream_ref stream,
  rmm::device_async_resource_ref mr)
{
  bulk_read_operation operation{sources, options, ranges, stream, mr};
  if (operation.empty()) {
    cudf::io::parquet_reader_options empty_options;
    if (options.get_column_names()) { empty_options.set_column_names(*options.get_column_names()); }
    empty_options.set_num_rows(0);
    std::vector<std::unique_ptr<cudf::io::datasource>> cudf_sources;
    cudf_sources.push_back(cudf::io::datasource::create(sources.front().datasource.get()));
    std::vector<cudf::io::parquet::FileMetaData> metadatas{*sources.front().metadata};
    auto result = cudf::io::read_parquet(
      std::move(cudf_sources), std::move(metadatas), empty_options, stream, mr);
    return std::move(result.tbl);
  }
  operation.issue_and_settle();
  return operation.decode();
}

/// General route: let cudf read as it decodes, over whatever sources there are.
std::unique_ptr<cudf::table> materialize_general(std::span<parquet_source const> sources,
                                                 cudf::io::parquet_reader_options const& options,
                                                 ::cuda::stream_ref stream,
                                                 rmm::device_async_resource_ref mr)
{
  std::vector<std::unique_ptr<cudf::io::datasource>> cudf_sources;
  std::vector<cudf::io::parquet::FileMetaData> metadatas;
  std::vector<std::vector<cudf::size_type>> rg_per_src;
  cudf_sources.reserve(sources.size());
  metadatas.reserve(sources.size());
  rg_per_src.reserve(sources.size());

  for (auto const& src : sources) {
    cudf_sources.push_back(cudf::io::datasource::create(src.datasource.get()));
    metadatas.push_back(*src.metadata);
    rg_per_src.push_back(src.row_group_indices);
  }

  auto opts = options;
  if (std::all_of(
        rg_per_src.begin(), rg_per_src.end(), [](auto const& groups) { return groups.empty(); })) {
    opts.set_num_rows(0);
  } else {
    opts.set_row_groups(std::move(rg_per_src));
  }

  auto [table, _] =
    cudf::io::read_parquet(std::move(cudf_sources), std::move(metadatas), opts, stream, mr);
  return std::move(table);
}

}  // namespace

void require_same_parquet_schema(std::span<parquet_source const> sources)
{
  if (sources.size() < 2) { return; }
  auto const& first = sources.front().metadata->schema;
  for (auto const& src : sources.subspan(1)) {
    auto const& schema = src.metadata->schema;
    if (schema == first) { continue; }
    std::string const prefix = "[parquet_materialize] All sources must have the same schema: '" +
                               path_of(sources.front()) + "' and '" + path_of(src) + "' ";
    if (schema.size() != first.size()) {
      throw std::runtime_error(prefix + "have " + std::to_string(first.size()) + " and " +
                               std::to_string(schema.size()) +
                               " schema elements (including the root)");
    }
    auto const [lhs, rhs] = std::mismatch(first.begin(), first.end(), schema.begin(), schema.end());
    std::string detail    = describe(*lhs) + " vs " + describe(*rhs);
    if (lhs->name == rhs->name && lhs->type == rhs->type) {
      detail += "; they differ in annotation, width or nesting";
    }
    throw std::runtime_error(prefix + "differ at schema element " +
                             std::to_string(lhs - first.begin()) + " (" + detail + ")");
  }
}

std::unique_ptr<cudf::table> materialize_parquet(
  std::span<parquet_source const> sources,
  cudf::io::parquet_reader_options const& options,
  std::span<std::vector<cudf::io::text::byte_range_info> const> ranges,
  ::cuda::stream_ref stream,
  rmm::device_async_resource_ref mr)
{
  if (sources.empty()) { return nullptr; }
  if (prefers_bulk_materialize(sources, options)) {
    return materialize_bulk(sources, options, ranges, stream, mr);
  }
  return materialize_general(sources, options, stream, mr);
}

}  // namespace sirius::op::scan
