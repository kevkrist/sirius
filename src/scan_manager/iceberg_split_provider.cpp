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

#include "scan_manager/iceberg_split_provider.hpp"

#include "log/logging.hpp"
#include "op/scan/iceberg_delete_filter.hpp"
#include "op/scan/parquet_scan_operator_data.hpp"
#include "scan_manager/sirius_scan_manager.hpp"

#include <io/sirius_datasource.hpp>
#include <io/uring/uring_reactor.hpp>

#include <cudf/io/datasource.hpp>
#include <cudf/io/parquet.hpp>
#include <cudf/io/parquet_io_utils.hpp>
#include <cudf/io/parquet_schema.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace sirius::scan_manager {

namespace {

/// Strip a leading @c "file://" (case-insensitive) prefix so the resulting
/// path can be passed to the local io_uring backend, which expects bare
/// absolute paths.  Mirrors the helper in @c parquet_split_provider::run_batch.
std::string normalize_path(std::string const& p)
{
  static constexpr std::string_view kFile = "file://";
  if (p.size() > kFile.size()) {
    bool is_file_uri = true;
    for (std::size_t i = 0; i < kFile.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(p[i])) !=
          static_cast<unsigned char>(kFile[i])) {
        is_file_uri = false;
        break;
      }
    }
    if (is_file_uri) { return p.substr(kFile.size()); }
  }
  return p;
}

/// True if @p p still carries a URI scheme after file:// stripping
/// (s3://, http://, …).
bool has_uri_scheme(std::string const& p) { return p.find("://") != std::string::npos; }

/// Resolve the ioctx for a one-shot footer read.  Mirrors the per-path
/// dispatch in @c parquet_split_provider::run_batch: non-local schemes go
/// through the scan_manager (which carries the s3 backend), local paths
/// prefer the planning gpu ioctx, falling back to the manager's local
/// backend when @p gpu_ioctxs is empty.
std::shared_ptr<sirius::io::sirius_ioctx> resolve_ioctx(
  std::string const& lookup_path,
  sirius_scan_manager& scan_manager,
  std::unordered_map<int, std::shared_ptr<sirius::io::sirius_ioctx>> const& gpu_ioctxs)
{
  if (has_uri_scheme(lookup_path)) {
    auto ctx = scan_manager.io_ctx_shared_for(lookup_path);
    if (!ctx) {
      throw std::runtime_error("[iceberg_split_provider] no backend supports path: " +
                               lookup_path);
    }
    return ctx;
  }
  if (!gpu_ioctxs.empty()) { return gpu_ioctxs.begin()->second; }
  return scan_manager.io_ctx_shared_for(lookup_path);
}

}  // namespace

//===----------------------------------------------------------------------===//
// widen_projection
//===----------------------------------------------------------------------===//

iceberg_split_provider::widened iceberg_split_provider::widen_projection(
  duckdb::vector<duckdb::ColumnIndex> const& column_ids,
  duckdb::vector<duckdb::idx_t> const& projection_ids,
  duckdb::vector<std::string> const& names,
  op::scan::IcebergDeleteData const* delete_data)
{
  widened w;
  w.column_ids     = column_ids;
  w.projection_ids = projection_ids;

  // Compute the currently-selected schema indices in scan_plan data_columns
  // order: unique primary indices in projection_ids order (or column_ids
  // order when projection_ids is empty).  Skip virtual columns since
  // scan_plan also skips them.
  auto walk_unique = [&](auto&& on_primary) {
    std::unordered_set<std::size_t> seen;
    auto consider = [&](std::size_t column_ids_pos) {
      auto const primary = column_ids.at(column_ids_pos).GetPrimaryIndex();
      if (duckdb::IsVirtualColumn(primary)) { return; }
      if (!seen.insert(primary).second) { return; }
      on_primary(primary);
    };
    if (projection_ids.empty()) {
      for (std::size_t c = 0; c < column_ids.size(); ++c) {
        consider(c);
      }
    } else {
      for (auto const pid : projection_ids) {
        consider(pid);
      }
    }
  };
  walk_unique(
    [&](std::size_t primary) { w.selected_schema_indices.push_back(primary); });

  // No widening when there are no equality-delete groups; the natural read
  // already covers every column we need.  Positional deletes don't require
  // any extra columns (they key on data_path + first_row).
  if (delete_data == nullptr || delete_data->equality_delete_groups.empty()) {
    return w;
  }

  // name → schema position lookup for equality-delete keys.
  std::unordered_map<std::string, std::size_t> name_to_idx;
  name_to_idx.reserve(names.size());
  for (std::size_t i = 0; i < names.size(); ++i) {
    name_to_idx.emplace(names[i], i);
  }

  std::unordered_set<std::size_t> already_selected(w.selected_schema_indices.begin(),
                                                   w.selected_schema_indices.end());

  // Collect new schema indices to add (deduplicated across groups).
  std::vector<std::size_t> to_add;
  for (auto const& group : delete_data->equality_delete_groups) {
    for (auto const& key_name : group.key_names) {
      auto it = name_to_idx.find(key_name);
      if (it == name_to_idx.end()) { continue; }
      if (already_selected.insert(it->second).second) { to_add.push_back(it->second); }
    }
  }

  if (to_add.empty()) { return w; }

  // Promote projection_ids from "empty = read all" to an explicit listing.
  // Once we append extras the read can no longer be "read all in column_ids
  // order"; the scan_plan factory needs to see every column it must include.
  if (w.projection_ids.empty()) {
    w.projection_ids.reserve(w.column_ids.size() + to_add.size());
    for (std::size_t c = 0; c < w.column_ids.size(); ++c) {
      w.projection_ids.push_back(c);
    }
  }

  // For each new schema index: ensure it has a column_ids entry, then append
  // its column_ids position to projection_ids.  Most planners hand us a
  // column_ids that already covers every schema column (the planner just
  // narrows projection_ids), so the existing-entry branch is the common case.
  std::unordered_map<std::size_t, std::size_t> primary_to_cid;
  primary_to_cid.reserve(w.column_ids.size());
  for (std::size_t c = 0; c < w.column_ids.size(); ++c) {
    auto const primary = w.column_ids[c].GetPrimaryIndex();
    if (duckdb::IsVirtualColumn(primary)) { continue; }
    primary_to_cid.emplace(primary, c);
  }
  for (auto const primary : to_add) {
    auto it = primary_to_cid.find(primary);
    std::size_t cid_pos;
    if (it != primary_to_cid.end()) {
      cid_pos = it->second;
    } else {
      cid_pos = w.column_ids.size();
      w.column_ids.emplace_back(primary);
      primary_to_cid.emplace(primary, cid_pos);
    }
    w.projection_ids.push_back(cid_pos);
    w.selected_schema_indices.push_back(primary);
  }
  return w;
}

//===----------------------------------------------------------------------===//
// build_hook
//===----------------------------------------------------------------------===//

op::scan::scan_post_decode_hook_t iceberg_split_provider::build_hook(
  std::vector<std::string> const& file_paths,
  duckdb::vector<std::string> const& names,
  std::vector<std::size_t> const& selected_schema_indices,
  sirius_scan_manager& scan_manager,
  std::unordered_map<int, std::shared_ptr<sirius::io::sirius_ioctx>> const& gpu_ioctxs,
  std::shared_ptr<const op::scan::IcebergDeleteData> const& delete_data)
{
  if (!delete_data || delete_data->empty()) { return {}; }

  op::scan::iceberg_delete_pipeline pipeline;

  // V2 positional deletes + V3 deletion vectors share the same map shape.
  if (!delete_data->positional_deletes.empty()) {
    pipeline.add_filter(std::make_shared<op::scan::positional_delete_filter>(delete_data));
  }

  // V2 equality deletes — resolve each group's key names to data-batch
  // positions.  Prefer Iceberg field-id matching (schema-evolution safe);
  // fall back to name matching when the data file doesn't carry field ids.
  if (!delete_data->equality_delete_groups.empty()) {
    if (file_paths.empty()) {
      throw std::runtime_error(
        "[iceberg_split_provider] equality-delete groups present but no data files to read.");
    }

    // Read the first data file's footer to extract the field_id map.  This
    // is a one-shot planning-time read; the prefetching cache will reuse the
    // result on the first parquet_split_provider::run_batch().
    auto const lookup_path  = normalize_path(file_paths.front());
    auto const file_io_ctx  = resolve_ioctx(lookup_path, scan_manager, gpu_ioctxs);
    auto file_io_object     = file_io_ctx->create_io_object(lookup_path);
    auto datasource         = file_io_ctx->make_datasource(file_io_object);
    auto footer_buffer      = cudf::io::parquet::fetch_footer_to_host(*datasource);
    auto first_file_meta    = cudf::io::parquet::experimental::hybrid_scan_reader(
                              cudf::host_span<uint8_t const>(footer_buffer->data(),
                                                              footer_buffer->size()),
                              cudf::io::parquet_reader_options::builder().build())
                              .parquet_metadata();

    auto const data_id_map = op::scan::extract_field_id_map(first_file_meta);

    // field_id → batch position (D) for selected columns.  Batch position
    // matches selected_schema_indices's order (== scan_plan's data_columns
    // order: unique primary indices in projection_ids order).
    std::unordered_map<int32_t, cudf::size_type> data_field_id_to_idx;
    for (std::size_t j = 0; j < selected_schema_indices.size(); ++j) {
      auto const& col_name = names[selected_schema_indices[j]];
      auto it              = data_id_map.find(col_name);
      if (it != data_id_map.end()) {
        data_field_id_to_idx[it->second] = static_cast<cudf::size_type>(j);
      }
    }

    for (std::size_t gi = 0; gi < delete_data->equality_delete_groups.size(); ++gi) {
      auto const& group = delete_data->equality_delete_groups[gi];

      std::vector<cudf::size_type> data_key_indices;
      data_key_indices.reserve(group.key_names.size());
      bool all_found = true;

      for (std::size_t k = 0; k < group.key_names.size(); ++k) {
        auto const& key_name = group.key_names[k];
        bool found           = false;

        if (k < group.key_field_ids.size() && group.key_field_ids[k].has_value()) {
          auto it = data_field_id_to_idx.find(group.key_field_ids[k].value());
          if (it != data_field_id_to_idx.end()) {
            data_key_indices.push_back(it->second);
            found = true;
            SIRIUS_LOG_DEBUG("[iceberg_split_provider] Matched equality key '{}' by field id {}.",
                             key_name,
                             group.key_field_ids[k].value());
          }
        }

        if (!found) {
          for (std::size_t j = 0; j < selected_schema_indices.size(); ++j) {
            if (names[selected_schema_indices[j]] == key_name) {
              data_key_indices.push_back(static_cast<cudf::size_type>(j));
              found = true;
              break;
            }
          }
        }

        if (!found) {
          SIRIUS_LOG_WARN(
            "[iceberg_split_provider] Equality-delete key '{}' not found in scan output — "
            "skipping group {}.",
            key_name,
            gi);
          all_found = false;
          break;
        }
      }

      if (all_found) {
        pipeline.add_filter(std::make_shared<op::scan::equality_delete_filter>(
          delete_data, gi, std::move(data_key_indices)));
      }
    }
  }

  if (pipeline.empty()) { return {}; }

  // assemble_scan_output drops trailing pure-filter columns via
  // scan_output_arity; the pipeline's own tail-strip is therefore disabled.
  pipeline.set_extra_column_count(0);
  return pipeline.build_hook();
}

//===----------------------------------------------------------------------===//
// constructors
//===----------------------------------------------------------------------===//

iceberg_split_provider::iceberg_split_provider(
  duckdb::vector<sirius::logical_type> const& returned_types,
  std::vector<std::string> const& file_paths,
  duckdb::vector<duckdb::ColumnIndex> const& column_ids,
  duckdb::vector<duckdb::idx_t> const& projection_ids,
  duckdb::vector<std::string> const& names,
  std::size_t scan_output_arity,
  duckdb::unique_ptr<duckdb::TableFilterSet> table_filter_set,
  duckdb::vector<duckdb::HivePartitioningIndex> const& partition_indices,
  std::size_t approximate_batch_size,
  sirius_scan_manager& scan_manager,
  std::unordered_map<int, std::shared_ptr<sirius::io::sirius_ioctx>> const& gpu_ioctxs,
  std::shared_ptr<const op::scan::IcebergDeleteData> delete_data)
  : iceberg_split_provider(returned_types,
                           file_paths,
                           widen_projection(column_ids, projection_ids, names, delete_data.get()),
                           names,
                           scan_output_arity,
                           std::move(table_filter_set),
                           partition_indices,
                           approximate_batch_size,
                           scan_manager,
                           gpu_ioctxs,
                           std::move(delete_data))
{
}

iceberg_split_provider::iceberg_split_provider(
  duckdb::vector<sirius::logical_type> const& returned_types,
  std::vector<std::string> const& file_paths,
  widened w,
  duckdb::vector<std::string> const& names,
  std::size_t scan_output_arity,
  duckdb::unique_ptr<duckdb::TableFilterSet> table_filter_set,
  duckdb::vector<duckdb::HivePartitioningIndex> const& partition_indices,
  std::size_t approximate_batch_size,
  sirius_scan_manager& scan_manager,
  std::unordered_map<int, std::shared_ptr<sirius::io::sirius_ioctx>> const& gpu_ioctxs,
  std::shared_ptr<const op::scan::IcebergDeleteData> delete_data)
  : parquet_split_provider(returned_types,
                           file_paths,
                           w.column_ids,
                           w.projection_ids,
                           names,
                           scan_output_arity,
                           std::move(table_filter_set),
                           partition_indices,
                           approximate_batch_size,
                           /*max_file_processed=*/1,
                           scan_manager,
                           gpu_ioctxs)
{
  _hook = build_hook(
    file_paths, names, w.selected_schema_indices, scan_manager, gpu_ioctxs, delete_data);
  if (_hook) {
    SIRIUS_LOG_DEBUG(
      "[iceberg_split_provider] Built delete-filter hook ({} positional file(s), {} eq group(s)).",
      delete_data ? delete_data->positional_deletes.size() : 0,
      delete_data ? delete_data->equality_delete_groups.size() : 0);
  } else {
    SIRIUS_LOG_DEBUG(
      "[iceberg_split_provider] No delete-filter hook installed; running as plain parquet scan.");
  }
}

iceberg_split_provider::~iceberg_split_provider() = default;

//===----------------------------------------------------------------------===//
// next_split_provider
//===----------------------------------------------------------------------===//

std::function<std::vector<std::unique_ptr<op::operator_data>>()>
iceberg_split_provider::next_split_provider()
{
  auto base = parquet_split_provider::next_split_provider();
  if (!base || !_hook) { return base; }
  return [base = std::move(base), hook = _hook]() {
    auto out = base();
    for (auto& item : out) {
      if (auto* psd = dynamic_cast<op::scan::parquet_scan_data*>(item.get())) {
        psd->post_decode_hook = hook;
      }
    }
    return out;
  };
}

}  // namespace sirius::scan_manager
