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
 * Owns all demand-read destinations through settlement and hybrid decode. Preparation publishes no
 * reads; destruction drains every remaining future before releasing buffers. Decoder spans retain
 * logical lengths while each allocation includes cuDF's eight-byte padding. The caller retains the
 * filter AST and current device, stream and allocator for this operation's lifetime.
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
    _files.reserve(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
      file_read file;
      file.source = sources[i];
      if (options.get_filter()) {
        file.reader = std::make_unique<hybrid_scan_reader>(*file.source.metadata, options);
        auto const surviving_groups =
          file.reader->filter_row_groups_with_stats(file.source.row_group_indices, options, stream);
        std::erase_if(file.source.row_group_indices, [&](auto group) {
          return std::find(surviving_groups.begin(), surviving_groups.end(), group) ==
                 surviving_groups.end();
        });
        if (file.source.row_group_indices.empty()) { continue; }
        file.ranges =
          file.reader->all_column_chunks_byte_ranges(file.source.row_group_indices, options);
      } else if (i < ranges.size() && !ranges[i].empty()) {
        file.ranges = ranges[i];
      } else {
        file.reader = std::make_unique<hybrid_scan_reader>(*file.source.metadata, options);
        file.ranges =
          file.reader->all_column_chunks_byte_ranges(file.source.row_group_indices, options);
      }
      for (auto const& range : file.ranges) {
        if (range.offset() < 0 || range.size() < 0) {
          throw std::invalid_argument("negative parquet column chunk range");
        }
        auto const size   = static_cast<std::size_t>(range.size());
        auto const offset = static_cast<std::size_t>(range.offset());
        if (size > std::numeric_limits<std::size_t>::max() - offset ||
            size > std::numeric_limits<std::size_t>::max() - file.expected_bytes) {
          throw std::overflow_error("parquet column chunk byte count overflow");
        }
        static_cast<void>(padded_chunk_size(size));
        file.expected_bytes += size;
      }
      _files.push_back(std::move(file));
    }
    if (_files.empty()) { return; }

    _groups.reserve(_files.size());
    std::size_t span_count = 0;
    std::vector<cudf::io::parquet::FileMetaData> metadatas;
    if (_files.size() > 1) { metadatas.reserve(_files.size()); }
    for (auto& file : _files) {
      _groups.push_back(file.source.row_group_indices);
      if (file.ranges.size() > std::numeric_limits<std::size_t>::max() - span_count) {
        throw std::overflow_error("parquet column chunk count overflow");
      }
      span_count += file.ranges.size();
      if (_files.size() > 1) {
        metadatas.push_back(*file.source.metadata);
        file.reader.reset();
      }
    }
    if (_files.size() > 1) {
      _multifile_reader = std::make_unique<cudf::io::parquet::experimental::hybrid_scan_multifile>(
        cudf::host_span<cudf::io::parquet::FileMetaData const>{metadatas.data(), metadatas.size()},
        options);
    } else if (!_files.front().reader) {
      _files.front().reader =
        std::make_unique<hybrid_scan_reader>(*_files.front().source.metadata, options);
    }
    _spans.reserve(span_count);
    for (auto& file : _files) {
      file.buffers.reserve(file.ranges.size());
      file.reads.reserve(file.ranges.size());
      for (auto const& range : file.ranges) {
        auto const size = static_cast<std::size_t>(range.size());
        auto& buffer    = file.buffers.emplace_back(padded_chunk_size(size), stream, mr);
        auto* data      = static_cast<std::uint8_t*>(buffer.data());
        if (buffer.size() != size) {
          CUDF_CUDA_TRY(cudaMemsetAsync(data + size, 0, buffer.size() - size, stream.get()));
        }
        _spans.emplace_back(data, size);
        if (size != 0) {
          file.reads.emplace_back(static_cast<std::size_t>(range.offset()), size, data);
        }
      }
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
  opts.set_row_groups(std::move(rg_per_src));

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
