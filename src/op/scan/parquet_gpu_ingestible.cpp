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

// sirius
#include "op/scan/gpu_ingestible_types.hpp"

#include <expression/ast/from_duckdb.hpp>
#include <expression_executor/gpu_expression_executor.hpp>
#include <expression_executor/gpu_expression_translator_internal.hpp>
#include <io/io_context.hpp>
#include <io/sirius_datasource.hpp>
#include <log/logging.hpp>
#include <op/scan/parquet_gpu_ingestible.hpp>
#include <op/scan/parquet_schema_mapping.hpp>
#include <op/scan/scan_utils.hpp>
#include <op/scan/sirius_gpu_scan_operator_data.hpp>
#include <scan_manager/parquet_metadata.hpp>
#include <scan_manager/sirius_scan_manager.hpp>

// cudf
#include <cudf/io/datasource.hpp>
#include <cudf/io/parquet.hpp>
#include <cudf/io/parquet_io_utils.hpp>
#include <cudf/io/parquet_schema.hpp>
#include <cudf/table/table.hpp>
#include <cudf/utilities/default_stream.hpp>
#include <cudf/utilities/memory_resource.hpp>

// cucascade
#include <cucascade/memory/memory_space.hpp>

// duckdb
#include <duckdb/common/hive_partitioning.hpp>

// uring_reactor MUST be included last among sirius headers — see
// parquet_split_provider.cpp for the BLOCK_SIZE macro-collision rationale.
#include <io/uring/uring_reactor.hpp>

// standard library
#include <cctype>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sirius::op::scan {

namespace {

/// @brief Packs per-file parquet_split_info splits into decode-batch-sized, hive-partition-uniform
/// splits:
///  - byte cap on reserved_uncompressed_bytes
///  - a flush whenever partition_values change
class parquet_batch_coalecer : public batch_coalecer {
 public:
  explicit parquet_batch_coalecer(std::size_t cap) : _cap(cap) {}

  std::vector<std::unique_ptr<scan_info>> push(std::unique_ptr<scan_info> info) override
  {
    std::vector<std::unique_ptr<scan_info>> emitted;
    auto* split = dynamic_cast<parquet_split_info*>(info.get());
    if (split == nullptr) { return emitted; }

    if (!_have_template) {
      _reader_options          = split->reader_options;
      _plan                    = split->plan;
      _disable_filter_pushdown = split->disable_filter_pushdown;
      _needs_assembly          = split->needs_assembly;
      _have_template           = true;
    }
    if (!_acc.empty() && split->partition_values != _acc_partition_values) {
      emitted.push_back(emit_current());  // partition boundary
    }
    _acc_partition_values = split->partition_values;
    for (auto const& slice : split->rg_slices) {
      auto const slice_bytes = slice.reserved_uncompressed_bytes;
      if (!_acc.empty() && _cap > 0 && _acc_bytes + slice_bytes > _cap) {
        emitted.push_back(emit_current());  // byte cap
      }
      _acc_bytes += slice_bytes;
      _acc.push_back(std::move(slice));
    }
    return emitted;
  }

  std::vector<std::unique_ptr<scan_info>> flush() override
  {
    std::vector<std::unique_ptr<scan_info>> out;
    if (!_acc.empty()) { out.push_back(emit_current()); }
    return out;
  }

 private:
  std::unique_ptr<scan_info> emit_current()
  {
    auto split                     = std::make_unique<parquet_split_info>();
    split->rg_slices               = std::move(_acc);
    split->reader_options          = _reader_options;
    split->plan                    = _plan;
    split->disable_filter_pushdown = _disable_filter_pushdown;
    split->needs_assembly          = _needs_assembly;
    split->partition_values        = _acc_partition_values;
    _acc.clear();
    _acc_bytes = 0;
    return split;
  }

  const std::size_t _cap;
  std::vector<row_group_slice> _acc;
  std::size_t _acc_bytes = 0;
  std::vector<std::string> _acc_partition_values;

  bool _have_template =
    false;  ///< Whether we have a template (cached shared data) for the current batch
  std::shared_ptr<cudf::io::parquet_reader_options> _reader_options;
  std::shared_ptr<scan_plan const> _plan;
  bool _disable_filter_pushdown = false;
  bool _needs_assembly          = false;
};

bool has_uri_scheme(std::string const& p) { return p.find("://") != std::string::npos; }

}  // namespace

//===----------------------------------------------------------------------===//
// make_ingestible
//===----------------------------------------------------------------------===//
std::shared_ptr<parquet_gpu_ingestible> make_ingestible(
  std::unique_ptr<parquet_ingestible_table_info> info)
{
  return std::make_shared<parquet_gpu_ingestible>(std::move(info));
}

//===----------------------------------------------------------------------===//
// create_batch_coalescer
//===----------------------------------------------------------------------===//
std::unique_ptr<batch_coalecer> parquet_gpu_ingestible::create_batch_coalecer() const
{
  return std::make_unique<parquet_batch_coalecer>(_approximate_batch_size);
}

//===----------------------------------------------------------------------===//
// parquet_gpu_ingestible — construction
//===----------------------------------------------------------------------===//
parquet_gpu_ingestible::parquet_gpu_ingestible(std::unique_ptr<parquet_ingestible_table_info> info)
  : _info(std::move(info))
{
  auto const& bind = static_cast<parquet_ingestible_table_info const&>(table_info());

  // Any non-trivial scan shape — reader-side projection, filter pushdown, or hive-partition
  // injection — needs column names. Matches parquet_split_provider's ctor invariant.
  bool const needs_names = !bind.projection_ids.empty() ||
                           (bind.table_filters && !bind.table_filters->filters.empty()) ||
                           !bind.partition_indices.empty();
  if (needs_names && bind.names.empty()) {
    throw sirius::internal_exception(
      "[parquet_gpu_ingestible] Projection, filter pushdown, or hive partitions "
      "require column names to be provided.");
  }

  _plan = std::make_shared<scan_plan const>(build_scan_plan(bind.column_ids,
                                                            bind.projection_ids,
                                                            bind.names,
                                                            bind.returned_types,
                                                            bind.scan_output_arity,
                                                            bind.partition_indices));

  // AST translation deferred to materialize_table so a task-local stream is used.
  // Filters on hive-partition columns are dropped — those columns aren't in the
  // parquet file (DuckDB prunes them at the file-list level already).
  if (bind.table_filters && !bind.table_filters->filters.empty()) {
    auto duckdb_expression =
      sirius::op::convert_table_filters_to_expression(*bind.table_filters,
                                                      bind.column_ids,
                                                      bind.returned_types,
                                                      _plan->batch_position_by_column_id,
                                                      _plan->partition_primary_indices);
    if (duckdb_expression) { _duckdb_filter_expression = std::move(duckdb_expression); }
  }

  _file_paths             = bind.resolved_file_paths;
  _approximate_batch_size = bind.approximate_batch_size;
  _total_files            = _file_paths.size();
}

parquet_gpu_ingestible::~parquet_gpu_ingestible() = default;

//===----------------------------------------------------------------------===//
// split-provider interface
//===----------------------------------------------------------------------===//
bool parquet_gpu_ingestible::has_processed_all_metadata() const
{
  return _next_file_idx.load(std::memory_order_relaxed) >= _total_files;
}

std::function<std::unique_ptr<op::scan::scan_info>()> parquet_gpu_ingestible::next_split_provider(
  std::shared_ptr<io::sirius_ioctx> io_ctx)
{
  if (io_ctx == nullptr) {
    throw std::runtime_error("parquet_gpu_ingestible: no scan_manager is wired.");
  }
  ensure_reader_options(io_ctx);

  auto const idx = _next_file_idx.fetch_add(1, std::memory_order_relaxed);
  if (idx >= _total_files) { return nullptr; }
  auto file_path = _file_paths[idx];
  return [this, file_path = std::move(file_path), ctx = std::move(io_ctx)]() {
    return parse_file(file_path, ctx);
  };
}

//===----------------------------------------------------------------------===//
// ensure_reader_options -- build the shared reader options
//                          (column projection + filter pushdown)
//                          on the first split
//===----------------------------------------------------------------------===//
void parquet_gpu_ingestible::ensure_reader_options(std::shared_ptr<io::sirius_ioctx> const& io_ctx)
{
  std::call_once(_reader_init_flag, [this, io_ctx]() {
    auto stream                  = cudf::get_default_stream();
    auto const data_column_names = _plan->data_column_names();
    _reader_options              = std::make_shared<cudf::io::parquet_reader_options>(
      cudf::io::parquet_reader_options::builder().build());

    if (_plan->is_projected()) { _reader_options->set_column_names(_plan->data_column_names()); }

    if (!_duckdb_filter_expression) { return; }

    auto name_resolver = [this](duckdb::idx_t ref_index) -> std::string {
      return _plan->batch_column_name(ref_index);
    };
    gpu_expression_translator translator(stream, cudf::get_current_device_resource_ref());
    auto sirius_filter_ast = sirius::ast::from_duckdb(*_duckdb_filter_expression);
    auto ast_expression =
      translator.translate_expression_with_names(*sirius_filter_ast, name_resolver);
    if (!ast_expression) {
      SIRIUS_LOG_DEBUG("[parquet_gpu_ingestible] AST translation failed for row group pruning.");
      return;
    }

    // Fixed-length Byte Array (FLBA) decimal probe on the first file -- cuDF can't compare
    // row-group stats of a FLBA/BYTE_ARRAY decimal column against an AST literal, so pushdown is
    // disabled for such files.
    bool skip_flba = false;
    if (!_file_paths.empty()) {
      try {
        auto probe_ds = io_ctx->open_datasource(_file_paths.front());
        if (!probe_ds) {
          throw std::runtime_error("no backend supports path: " + _file_paths.front());
        }
        std::shared_ptr<cudf::io::parquet::FileMetaData const> probe_meta;
        if (auto cached = probe_ds->metadata()) {
          if (auto pm = std::dynamic_pointer_cast<scan_manager::parquet_metadata>(cached)) {
            probe_meta = pm->file_metadata();
          }
        }
        if (!probe_meta) {
          auto footer = cudf::io::parquet::fetch_footer_to_host(*probe_ds);
          hybrid_scan_reader probe_reader(
            cudf::host_span<uint8_t const>(footer->data(), footer->size()), *_reader_options);
          probe_meta = std::make_shared<cudf::io::parquet::FileMetaData const>(
            probe_reader.parquet_metadata());
        }
        for (auto const& elem : probe_meta->schema) {
          bool const is_decimal =
            (elem.converted_type.has_value() &&
             *elem.converted_type == cudf::io::parquet::ConvertedType::DECIMAL) ||
            (elem.logical_type.has_value() &&
             elem.logical_type->type == cudf::io::parquet::LogicalType::DECIMAL);
          if (!is_decimal) { continue; }
          if (elem.type == cudf::io::parquet::Type::FIXED_LEN_BYTE_ARRAY ||
              elem.type == cudf::io::parquet::Type::BYTE_ARRAY) {
            skip_flba = true;
            break;
          }
        }
      } catch (std::exception const& e) {
        SIRIUS_LOG_DEBUG("[parquet_gpu_ingestible] FLBA-decimal probe failed ({}); no pushdown",
                         e.what());
        skip_flba = true;
      }
    }
    _disable_filter_pushdown = skip_flba;
    if (!skip_flba) {
      _reader_options->set_filter(ast_expression->back());
      _pushdown_active = true;
    }
  });
}

//===----------------------------------------------------------------------===//
// parse_file
//===----------------------------------------------------------------------===//
std::unique_ptr<scan_info> parquet_gpu_ingestible::parse_file(
  std::string const& file_path, std::shared_ptr<io::sirius_ioctx> const& io_ctx)
{
  auto stream                  = cudf::get_default_stream();
  auto const data_column_names = _plan->data_column_names();

  std::shared_ptr<io::sirius_datasource> sirius_ds = io_ctx->open_datasource(file_path);
  if (!sirius_ds && has_uri_scheme(file_path)) {
    throw std::runtime_error("[parquet_gpu_ingestible] no backend supports path: " + file_path);
  }

  std::shared_ptr<cudf::io::parquet::FileMetaData const> file_metadata;
  std::unique_ptr<hybrid_scan_reader> reader_ptr;
  if (sirius_ds) {
    if (auto cached = sirius_ds->metadata()) {
      if (auto pm = std::dynamic_pointer_cast<scan_manager::parquet_metadata>(std::move(cached))) {
        file_metadata = pm->file_metadata();
        reader_ptr    = std::make_unique<hybrid_scan_reader>(*file_metadata, *_reader_options);
      }
    }
  }
  if (!reader_ptr) {
    auto footer = cudf::io::parquet::fetch_footer_to_host(*sirius_ds);
    reader_ptr  = std::make_unique<hybrid_scan_reader>(
      cudf::host_span<uint8_t const>(footer->data(), footer->size()), *_reader_options);
    file_metadata =
      std::make_shared<cudf::io::parquet::FileMetaData const>(reader_ptr->parquet_metadata());
  }
  auto& reader         = *reader_ptr;
  auto const& metadata = *file_metadata;

  std::vector<std::size_t> selected_chunk_indices;
  std::unordered_set<std::size_t> pure_filter_chunk_indices;
  if (_plan->is_projected()) {
    auto const pure_filter_positions = _plan->pure_filter_batch_positions();
    selected_chunk_indices.reserve(data_column_names.size());
    for (std::size_t k = 0; k < data_column_names.size(); ++k) {
      auto leaves = detail::leaf_indices_for_column(metadata, data_column_names[k]);
      if (leaves.empty()) {
        throw std::runtime_error("[parquet_gpu_ingestible] Projected column '" +
                                 data_column_names[k] +
                                 "' not found in parquet file: " + file_path);
      }
      bool const is_pure_filter = pure_filter_positions.count(k);
      for (auto const leaf : leaves) {
        selected_chunk_indices.push_back(leaf);
        if (is_pure_filter) { pure_filter_chunk_indices.insert(leaf); }
      }
    }
  }

  auto row_group_indices = reader.all_row_groups(*_reader_options);
  if (_pushdown_active) {
    row_group_indices =
      reader.filter_row_groups_with_stats(row_group_indices, *_reader_options, stream);
  }

  auto rg_contribution = [&](cudf::io::parquet::RowGroup const& row_group) {
    std::size_t rg_unc = 0;
    std::size_t rg_cmp = 0;
    auto add_chunk     = [&](cudf::io::parquet::ColumnChunk const& chunk, bool is_pure_filter) {
      auto const& cm = chunk.meta_data;
      if (!is_pure_filter) { rg_unc += static_cast<std::size_t>(cm.total_uncompressed_size); }
      rg_cmp += static_cast<std::size_t>(cm.total_compressed_size);
    };
    if (_plan->is_projected()) {
      for (auto const ci : selected_chunk_indices) {
        add_chunk(row_group.columns[ci], pure_filter_chunk_indices.contains(ci));
      }
    } else {
      for (auto const& chunk : row_group.columns) {
        add_chunk(chunk, false);
      }
    }
    return std::pair{rg_unc, rg_cmp};
  };

  std::size_t total_unc = 0;
  std::size_t total_cmp = 0;
  for (auto const rg_idx : row_group_indices) {
    auto const [u, c] = rg_contribution(metadata.row_groups[rg_idx]);
    total_unc += u;
    total_cmp += c;
  }

  std::vector<std::string> partition_values;
  if (!_plan->partition_columns.empty()) {
    auto parsed = duckdb::HivePartitioning::Parse(file_path);
    partition_values.reserve(_plan->partition_columns.size());
    for (auto const& pc : _plan->partition_columns) {
      auto it = parsed.find(pc.name);
      partition_values.push_back(it != parsed.end() ? it->second : std::string{});
    }
  }

  auto split                     = std::make_unique<parquet_split_info>();
  split->reader_options          = _reader_options;
  split->plan                    = _plan;
  split->disable_filter_pushdown = _disable_filter_pushdown;
  split->needs_assembly          = needs_output_assembly(*_plan);
  split->partition_values        = partition_values;
  if (!row_group_indices.empty()) {
    split->rg_slices.emplace_back(
      file_metadata, file_path, std::move(row_group_indices), total_unc, total_cmp, sirius_ds);
  }
  return split;
}

//===----------------------------------------------------------------------===//
// materialize_table
//===----------------------------------------------------------------------===//
filtered_table parquet_gpu_ingestible::materialize_metadata_to_table(
  op::scan::scan_info const& info,
  const cucascade::memory::memory_space& mem_space,
  rmm::cuda_stream_view stream)
{
  auto const& split = static_cast<parquet_split_info const&>(info);

  std::vector<std::unique_ptr<cudf::io::datasource>> sources;
  std::vector<cudf::io::parquet::FileMetaData> metadatas;
  std::vector<std::vector<cudf::size_type>> rg_per_src;
  sources.reserve(split.rg_slices.size());
  metadatas.reserve(split.rg_slices.size());
  rg_per_src.reserve(split.rg_slices.size());

  for (auto const& slice : split.rg_slices) {
    if (slice.datasource) {
      sources.push_back(cudf::io::datasource::create(slice.datasource.get()));
    } else {
      sources.push_back(cudf::io::datasource::create(slice.file_path));
    }
    metadatas.push_back(*slice.file_metadata);
    rg_per_src.push_back(slice.row_group_indices);
  }
  auto opts = *split.reader_options;
  opts.set_row_groups(std::move(rg_per_src));

  // Per-task AST translation. set_filter is gated on translation success AND on
  // the per-batch disable_filter_pushdown flag (set when the FLBA-decimal probe
  // failed). `sirius_filter_ast` is hoisted so the post-decode fallback can
  // reuse it on a pushdown miss.
  std::unique_ptr<sirius::ast::node> sirius_filter_ast;
  std::optional<gpu_expression_translator::translated_expression> ast_expression = std::nullopt;
  if (_duckdb_filter_expression) {
    sirius_filter_ast = sirius::ast::from_duckdb(*_duckdb_filter_expression);
    if (!split.disable_filter_pushdown) {
      auto name_resolver = [plan = split.plan](duckdb::idx_t ref_index) -> std::string {
        return plan->batch_column_name(ref_index);
      };
      gpu_expression_translator translator(stream, cudf::get_current_device_resource_ref());
      ast_expression =
        translator.translate_expression_with_names(*sirius_filter_ast, name_resolver);
      if (ast_expression) { opts.set_filter(ast_expression->back()); }
    }
  }

  rmm::device_async_resource_ref mr_ref(mem_space.get_default_allocator());
  auto [table, _] =
    cudf::io::read_parquet(std::move(sources), std::move(metadatas), opts, stream, mr_ref);

  SIRIUS_LOG_DEBUG(
    "[parquet_gpu_ingestible::materialize_table] Read {} file(s) (first: {}) — {} rows, {} "
    "columns",
    split.rg_slices.size(),
    split.rg_slices.empty() ? "<none>" : split.rg_slices.front().file_path,
    table->num_rows(),
    table->num_columns());

  // Determine filter state. When pushdown engaged the reader applied the
  // filter; otherwise we apply post-decode here. `sirius_filter_ast` must
  // outlive `exec` — the executor only borrows the AST.
  auto state = op::scan::filter_state::UNFILTERED;
  if (sirius_filter_ast) {
    if (!ast_expression) {
      sirius::gpu_expression_executor exec(
        sirius_filter_ast.get(), cudf::get_current_device_resource_ref(), stream);
      auto input = std::move(table);
      table      = exec.select(input->view());
      SIRIUS_LOG_DEBUG(
        "[parquet_gpu_ingestible::materialize_table] Applied duckdb filter expression "
        "post-decode.");
    }
    state = op::scan::filter_state::ROW_FILTERED;
  }

  // Reader-side pushdown succeeded and the plan needs assembly — inline it
  // here so the scan operator can skip post_filter_and_project entirely.
  // (post-decode fallback keeps assembly external because re-allocating after
  // exec.select is the same shape either way.)
  if (split.needs_assembly) {
    table = assemble_scan_output(*_plan, std::move(table), split.partition_values, stream);
    if (state == op::scan::filter_state::ROW_FILTERED) {
      state = op::scan::filter_state::ROW_FILTERED_AND_PROJECTED;
    }
  }
  return op::scan::filtered_table{std::move(table), state};
}

//===----------------------------------------------------------------------===//
// post_filter_and_project — assembly only
//===----------------------------------------------------------------------===//
std::unique_ptr<cudf::table> parquet_gpu_ingestible::post_filter_and_project(
  std::unique_ptr<cudf::table> input,
  filter_state state,
  op::scan::post_filter_and_projection_info const& info,
  ::cucascade::memory::memory_space const& /*mem_space*/,
  rmm::cuda_stream_view stream)
{
  auto const& pf = static_cast<parquet_post_filter_and_projection_info const&>(info);
  // The per-batch assembly call. The ingestible only emits a non-null
  // post_filter_and_projection_info when needs_output_assembly(*_plan) is true,
  // so this is unconditionally meaningful.
  auto out = assemble_scan_output(*_plan, std::move(input), pf.partition_values, stream);
  SIRIUS_LOG_DEBUG(
    "[parquet_gpu_ingestible::post_filter_and_project] Assembled scan output to plan layout.");
  return out;
}

}  // namespace sirius::op::scan
