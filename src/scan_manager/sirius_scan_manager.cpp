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

#include "scan_manager/sirius_scan_manager.hpp"

#include "data/data_batch_utils.hpp"
#include "exec/thread_pool.hpp"
#include "io/cache/prefetching_cache.hpp"
#include "io/io_context.hpp"
#include "io/parquet_helpers.hpp"
#include "io/sirius_datasource.hpp"
#include "log/logging.hpp"
#include "memory/topology_index.hpp"
#include "op/scan/duckdb_native_gpu_ingestible.hpp"
#include "op/scan/gpu_ingestible.hpp"
#include "op/scan/parquet_gpu_ingestible.hpp"
#include "op/scan/parquet_metadata.hpp"
#include "op/scan/sirius_gpu_scan_operator.hpp"
#include "op/scan/sirius_gpu_scan_operator_data.hpp"
#include "op/sirius_physical_operator_type.hpp"
#include "planner/query.hpp"
#include "scan_manager/round_robin_strategy.hpp"

#include <cudf/column/column_view.hpp>
#include <cudf/io/datasource.hpp>
#include <cudf/io/experimental/hybrid_scan.hpp>
#include <cudf/io/parquet.hpp>
#include <cudf/io/parquet_io_utils.hpp>
#include <cudf/io/parquet_schema.hpp>
#include <cudf/table/table.hpp>
#include <cudf/table/table_view.hpp>
#include <cudf/types.hpp>
#include <cudf/utilities/span.hpp>

#include <rmm/cuda_device.hpp>

#include <cucascade/cudf/gpu_data_representation.hpp>
#include <cucascade/memory/fixed_size_host_memory_resource.hpp>
#include <cucascade/memory/memory_reservation_manager.hpp>
#include <cucascade/memory/memory_space.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace sirius::scan_manager {

namespace {

class cached_databatch_provider final : public databatch_provider {
 public:
  cached_databatch_provider(pinned_entry const& entry,
                            std::span<const std::size_t> selected_columns,
                            cached_scan_plan plan,
                            const telemetry::batch_telemetry_info& telemetry_info)
    : _tier(entry.tier),
      _logical_order(entry.logical_order),
      _chunk_memory_spaces(entry.chunk_memory_spaces),
      _host_chunks(entry.host_chunks),
      _compressed_host_chunks(entry.compressed_host_chunks),
      _compressed_device_chunks(entry.compressed_device_chunks),
      _plan(std::move(plan)),
      _telemetry_info(telemetry_info)
  {
    // A provider snapshots the entry's shared owners instead of borrowing the map
    // element, so an in-flight scan survives a concurrent re-pin/unpin. Which chunks
    // it serves — and in what order after zone-map pruning — is precomputed in _plan.
    auto const& entry_column_names = entry.cache_info.column_names();
    _column_indices.assign(selected_columns.begin(), selected_columns.end());

    if (_tier == cucascade::memory::Tier::GPU && !entry.data_batches_by_column.empty()) {
      _raw_device_columns.reserve(selected_columns.size());
      for (std::size_t const index : selected_columns) {
        _raw_device_columns.push_back(
          entry.data_batches_by_column.at(entry_column_names.at(index)));
      }
    }
  }

  std::shared_ptr<cucascade::data_batch> get_next_batch() override
  {
    // The atomic cursor walks survivor positions (chunks that survived zone-map
    // pruning), each mapping to a chunk index into the snapshotted arms.
    auto const cursor = _index.fetch_add(1);
    if (cursor >= _plan.survivor_chunk_indices.size()) { return nullptr; }
    auto const index = _plan.survivor_chunk_indices[cursor];
    return _tier == cucascade::memory::Tier::GPU ? get_device_databatch(index)
                                                 : get_host_databatch(index);
  }

 private:
  std::shared_ptr<cucascade::data_batch> get_host_databatch(std::size_t index)
  {
    if (!_logical_order.empty()) {
      auto const source = _logical_order.at(index);
      switch (source.kind) {
        case chunk_kind::compressed: return serve_compressed_host(source.arm_index);
        case chunk_kind::raw: return serve_raw_host(source.arm_index);
      }
      throw std::runtime_error("pinned entry has an invalid chunk kind");
    }
    return !_compressed_host_chunks.empty() ? serve_compressed_host(index) : serve_raw_host(index);
  }

  std::shared_ptr<cucascade::data_batch> get_device_databatch(std::size_t index)
  {
    if (!_logical_order.empty()) {
      auto const source = _logical_order.at(index);
      switch (source.kind) {
        case chunk_kind::compressed: return serve_compressed_device(source.arm_index);
        case chunk_kind::raw: return serve_raw_device(source.arm_index);
      }
      throw std::runtime_error("pinned entry has an invalid chunk kind");
    }
    return !_compressed_device_chunks.empty() ? serve_compressed_device(index)
                                              : serve_raw_device(index);
  }

  std::shared_ptr<cucascade::data_batch> serve_compressed_host(std::size_t arm_index)
  {
    auto projected = _compressed_host_chunks.at(arm_index)->select_columns(_column_indices);
    return cucascade::data_batch::make(get_next_batch_id(), std::move(projected));
  }

  std::shared_ptr<cucascade::data_batch> serve_raw_host(std::size_t arm_index)
  {
    auto data_rep       = _host_chunks.at(arm_index)->slice(_column_indices);
    auto const batch_id = get_next_batch_id();
    return cucascade::data_batch::make(
      batch_id,
      std::move(data_rep),
      telemetry::quent_data_batch_probe::create(_telemetry_info, batch_id));
  }

  std::shared_ptr<cucascade::data_batch> serve_compressed_device(std::size_t arm_index)
  {
    auto projected = _compressed_device_chunks.at(arm_index)->select_columns(_column_indices);
    return cucascade::data_batch::make(get_next_batch_id(), std::move(projected));
  }

  std::shared_ptr<cucascade::data_batch> serve_raw_device(std::size_t arm_index)
  {
    std::vector<std::shared_ptr<cudf::column>> columns;
    std::vector<cudf::column_view> column_views;
    columns.reserve(_raw_device_columns.size());
    column_views.reserve(_raw_device_columns.size());
    std::size_t alloc_size = 0;
    for (auto const& column_chunks : _raw_device_columns) {
      columns.push_back(column_chunks.at(arm_index));
      column_views.emplace_back(columns.back()->view());
      alloc_size += columns.back()->alloc_size();
    }
    cudf::table_view view(column_views);
    auto gpu_repr =
      std::make_unique<::cucascade::gpu_table_representation>(view,
                                                              std::move(columns),
                                                              alloc_size,
                                                              *_chunk_memory_spaces.at(arm_index),
                                                              rmm::cuda_stream_view{});
    auto const batch_id = ::sirius::get_next_batch_id();
    return ::cucascade::data_batch::make(
      batch_id,
      std::move(gpu_repr),
      telemetry::quent_data_batch_probe::create(_telemetry_info, batch_id));
  }

  cucascade::memory::Tier _tier{cucascade::memory::Tier::GPU};
  std::vector<std::size_t> _column_indices;
  std::vector<chunk_source> _logical_order;
  std::vector<std::vector<std::shared_ptr<cudf::column>>> _raw_device_columns;
  std::vector<cucascade::memory::memory_space*> _chunk_memory_spaces;
  std::vector<std::shared_ptr<cucascade::host_data_representation>> _host_chunks;
  std::vector<std::shared_ptr<sirius::compressed_host_representation>> _compressed_host_chunks;
  std::vector<std::shared_ptr<sirius::compressed_device_representation>> _compressed_device_chunks;
  cached_scan_plan _plan;
  telemetry::batch_telemetry_info _telemetry_info;
  std::atomic<std::size_t> _index{0};
};

/// Filter view extracted from the scan's ingestible info: the pushed-down TableFilterSet plus the
/// scan's column_ids its keys index into.
struct scan_filter_view {
  duckdb::TableFilterSet const* table_filters{nullptr};
  duckdb::vector<duckdb::ColumnIndex> const* column_ids{nullptr};
};

scan_filter_view extract_scan_filters(op::scan::ingestible_table_info const& info)
{
  if (auto const* p = dynamic_cast<op::scan::parquet_ingestible_table_info const*>(&info)) {
    return {p->table_filters.get(), &p->column_ids};
  } else if (auto const* p =
               dynamic_cast<op::scan::duckdb_native_ingestible_table_info const*>(&info)) {
    return {p->table_filters.get(), &p->column_ids};
  }
  return {};
}

/// Build a serving provider that walks @p plan's survivor chunks. The public overload below
/// (declared in the header) is the identity-plan entry point used by tests and non-pruning
/// callers; this one takes an explicit plan so prepare_for_query can apply zone-map pruning.
std::unique_ptr<databatch_provider> make_provider_for_pinned_entry(
  pinned_entry const& entry,
  std::span<const std::size_t> selected_columns,
  cached_scan_plan plan,
  const telemetry::batch_telemetry_info& telemetry_info)
{
  return std::make_unique<cached_databatch_provider>(
    entry, selected_columns, std::move(plan), telemetry_info);
}

/// Strip a leading "file://" scheme (case-insensitive) so the path can be
/// resolved by a local-file backend.
std::string normalize_path(std::string const& p)
{
  static constexpr std::string_view kFile = "file://";
  if (p.size() > kFile.size()) {
    bool is_file_uri = true;
    for (std::size_t i = 0; i < kFile.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(p[i])) != static_cast<unsigned char>(kFile[i])) {
        is_file_uri = false;
        break;
      }
    }
    if (is_file_uri) { return p.substr(kFile.size()); }
  }
  return p;
}

// Transpose a run of per-chunk GPU tables into the entry's per-column dense arm
// (data_batches_by_column), preserving chunk order. Each table must expose exactly
// column_names.size() columns in column_ids order. Consumes the tables — release()
// empties each one. Shared by the plain GPU rebuild and the mixed device-compressed
// insert's raw arm.
void release_tables_into(pinned_entry& entry,
                         std::vector<std::unique_ptr<cudf::table>>& data_tables,
                         std::span<const std::string> column_names)
{
  for (auto& table : data_tables) {
    if (!table) {
      throw std::invalid_argument("[sirius_scan_manager] raw GPU chunk table must be non-null");
    }
    if (static_cast<std::size_t>(table->num_columns()) != column_names.size()) {
      throw std::runtime_error(
        "[sirius_scan_manager] table column count " + std::to_string(table->num_columns()) +
        " does not match column_names size " + std::to_string(column_names.size()));
    }
    auto cols = table->release();
    for (std::size_t i = 0; i < cols.size(); ++i) {
      entry.data_batches_by_column[std::string{column_names[i]}].emplace_back(std::move(cols[i]));
    }
  }
}

void add_rows_checked(std::size_t& total, std::int64_t rows, std::string_view source)
{
  if (rows < 0) { throw std::runtime_error(std::string{source} + " has a negative row count"); }
  auto const count = static_cast<std::size_t>(rows);
  if (count > std::numeric_limits<std::size_t>::max() - total) {
    throw std::overflow_error(std::string{source} + " row-count total overflowed");
  }
  total += count;
}

[[nodiscard]] bool has_same_cache_source(cache_entry_info const& lhs, cache_entry_info const& rhs)
{
  return lhs.resolved_file_paths == rhs.resolved_file_paths &&
         lhs.catalog_name == rhs.catalog_name && lhs.schema_name == rhs.schema_name &&
         lhs.table_name == rhs.table_name;
}

void validate_mvcc_publication(cache_entry_info const& cache_info,
                               std::unique_ptr<duckdb_mvcc_metadata> const& mvcc)
{
  bool const any_duckdb_identity = !cache_info.catalog_name.empty() ||
                                   !cache_info.schema_name.empty() ||
                                   !cache_info.table_name.empty();
  bool const duckdb_identity = !cache_info.catalog_name.empty() &&
                               !cache_info.schema_name.empty() && !cache_info.table_name.empty();
  bool const parquet_identity = !cache_info.resolved_file_paths.empty();
  if (any_duckdb_identity && !duckdb_identity) {
    throw std::invalid_argument("DuckDB-native pinned entries require a complete cache identity");
  }
  if (duckdb_identity && parquet_identity) {
    throw std::invalid_argument("pinned cache identity cannot be both DuckDB and parquet");
  }
  if (duckdb_identity != static_cast<bool>(mvcc)) {
    throw std::invalid_argument(
      duckdb_identity ? "DuckDB-native pinned entries require MVCC metadata at publication"
                      : "MVCC metadata is valid only for DuckDB-native pinned entries");
  }
}

void swap_cache_entry_info(cache_entry_info& lhs, cache_entry_info& rhs) noexcept
{
  lhs.resolved_file_paths.swap(rhs.resolved_file_paths);
  lhs.catalog_name.swap(rhs.catalog_name);
  lhs.schema_name.swap(rhs.schema_name);
  lhs.table_name.swap(rhs.table_name);
  lhs.column_ids.swap(rhs.column_ids);
  lhs.names.swap(rhs.names);
}

void swap_pinned_entries(pinned_entry& lhs, pinned_entry& rhs) noexcept
{
  using std::swap;
  swap_cache_entry_info(lhs.cache_info, rhs.cache_info);
  lhs.data_batches_by_column.swap(rhs.data_batches_by_column);
  lhs.chunk_memory_spaces.swap(rhs.chunk_memory_spaces);
  lhs.host_chunks.swap(rhs.host_chunks);
  lhs.compressed_host_chunks.swap(rhs.compressed_host_chunks);
  lhs.compressed_device_chunks.swap(rhs.compressed_device_chunks);
  lhs.logical_order.swap(rhs.logical_order);
  swap(lhs.tier, rhs.tier);
  swap(lhs.memory_space, rhs.memory_space);
  swap(lhs.num_rows, rhs.num_rows);
  // zone_maps is move-only (owns per-chunk BaseStatistics); std::swap moves it. A replace
  // must install the incoming sidecar and a same-row-count merge must keep the freshly
  // extended one — omitting this leaves the published entry with a stale or moved-from
  // sidecar, which then mis-prunes (drops matching chunks) or silently stops pruning.
  swap(lhs.zone_maps, rhs.zone_maps);
  lhs.mvcc.swap(rhs.mvcc);
}

void publish_pinned_entry(std::unordered_map<std::string, pinned_entry>& entries,
                          std::string const& name,
                          pinned_entry& entry)
{
  auto const existing = entries.find(name);
  if (existing == entries.end()) {
    entries.emplace(name, std::move(entry));
    return;
  }
  swap_pinned_entries(existing->second, entry);
}

void validate_entry_before_install(pinned_entry const& entry)
{
  std::vector<std::size_t> all_columns;
  all_columns.reserve(entry.cache_info.column_names().size());
  for (std::size_t index = 0; index < entry.cache_info.column_names().size(); ++index) {
    all_columns.push_back(index);
  }
  validate_pinned_entry_for_serving(entry, all_columns);
}

}  // namespace

std::unique_ptr<databatch_provider> make_provider_for_pinned_entry(
  pinned_entry const& entry,
  std::span<const std::size_t> selected_columns,
  const telemetry::batch_telemetry_info& telemetry_info)
{
  validate_pinned_entry_for_serving(entry, selected_columns);
  // No pushed-down filters: serve every chunk (identity plan).
  return make_provider_for_pinned_entry(
    entry, selected_columns, build_cached_scan_plan(entry, nullptr, nullptr), telemetry_info);
}

sirius_scan_manager::sirius_scan_manager(
  const scan_manager_config& config,
  cucascade::memory::memory_reservation_manager& reservation_manager,
  std::shared_ptr<const sirius::memory::topology_index> topology_index)
  : _config(config),
    _reservation_manager(reservation_manager),
    _topology_index(std::move(topology_index)),
    _thread_pool(_config.thread_pool.num_threads + 1,
                 _config.thread_pool.thread_name_prefix,
                 _config.thread_pool.cpu_affinity_list),
    _dispatcher(
      std::make_unique<exec::scoped_dispatcher>(_thread_pool, _thread_pool.num_threads())),
    _ioctx_registry(config, reservation_manager)
{
  if (!_topology_index) {
    throw std::invalid_argument("[sirius_scan_manager] topology_index must be non-null");
  }

  // scan_manager always owns an io_ctx: sirius_datasource (uring) on the
  // fast path, kvikio_context as the universal fallback so the rest of the
  // scan path (parquet_split_provider, scan tasks) always has an ioctx to
  // talk to.  kvikio_context wraps cudf::io::datasource so the read path
  // is identical from the caller's point of view.  Both are built by the
  // ioctx registry, which sources the reactor staging resource from the
  // reservation manager it was constructed with.
  if (_config.use_sirius_datasource) {
    _io_ctx = _ioctx_registry.make_ioctx(sirius::io::io_context_type::uring);
    if (!_io_ctx) {
      throw std::runtime_error("[sirius_scan_manager] failed to create uring io_context");
    }
    SIRIUS_LOG_DEBUG("[sirius_scan_manager] sirius_datasource enabled (uring_ioctx n_reactors={})",
                     _config.uring_n_reactors);
  } else {
    if (_topology_index->gpu_ids().size() > 1) {
      throw std::runtime_error(
        "[sirius_scan_manager] kvikio_context fallback (use_sirius_datasource=false) "
        "does not support multi-GPU; topology reports " +
        std::to_string(_topology_index->gpu_ids().size()) +
        " GPUs.  Enable use_sirius_datasource for multi-GPU runs.");
    }
    _io_ctx = _ioctx_registry.make_ioctx(sirius::io::io_context_type::kvikio);
    if (!_io_ctx) {
      throw std::runtime_error("[sirius_scan_manager] failed to create kvikio io_context");
    }
    SIRIUS_LOG_DEBUG(
      "[sirius_scan_manager] sirius_datasource disabled — using kvikio_context fallback");
  }

  // Build the prefetching cache on the ioctx.  Budget=0 keeps the
  // cache unarmed (no background threads); we pass that whenever the
  // user has disabled prefetching so the construction is always
  // unconditional and there's no "is the cache present" branch to
  // worry about in callers.
  if (_config.enable_prefetch_cache && _io_ctx->can_use_prefetching_cache()) {
    _io_ctx->initialize_cache(reservation_manager, _config.cache, _topology_index);
  }

  // Reactors are built parked; start() launches their worker threads and
  // allocates per-reactor staging.  No-op for the kvikio fallback (no reactors).
  _io_ctx->start();
}

sirius_scan_manager::~sirius_scan_manager()
{
  if (_io_ctx && _io_ctx->cache()) {
    SIRIUS_LOG_INFO("[sirius_scan_manager] cache summary: {}", _io_ctx->cache()->summary());
  }
  // Drain the dispatcher (and the worker pool) first so no in-flight
  // metadata-scan / sequencer task can still be reaching into the
  // cache via _io_ctx when we tear it down below.
  stop();
  // Tear down the cache (which owns its buffer_pool).  shutdown_cache drains
  // in-flight IO before the pool is destroyed, so callbacks release their
  // chunks safely.
  if (_io_ctx) { _io_ctx->shutdown_cache(); }
  // Same drain for any path-routed ioctxs; their reactors stop when the
  // shared_ptrs in _routed_io_ctxs are released (member destruction below).
  std::lock_guard lk{_routed_io_ctxs_mtx};
  for (auto& [type, io_ctx] : _routed_io_ctxs) {
    if (io_ctx) { io_ctx->shutdown_cache(); }
  }
}

parquet_bind_result sirius_scan_manager::describe_parquet(std::string const& uri)
{
  // Footer-probe only when we will actually read + parse the footer.  On a warm
  // re-bind the metadata_store already holds the parsed footer, so a suffix GET
  // would download footer bytes we won't reuse — a plain HEAD resolves the size.
  auto const cache_key     = normalize_path(uri);
  auto const io_ctx        = ioctx_for_path(uri);
  bool const footer_cached = io_ctx && io_ctx->metadata_store().get_metadata(cache_key) != nullptr;
  auto const hint =
    footer_cached ? sirius::io::open_hint::generic : sirius::io::open_hint::parquet_footer_probe;

  auto datasource = create_datasource(uri, hint);
  if (!datasource) {
    throw std::runtime_error("[sirius_scan_manager::describe_parquet] no backend supports URI: " +
                             uri);
  }

  // Reuse a previously parsed footer when present — a prior bind or scan of the
  // same file parks it in the ioctx metadata store, which lives for the ioctx's
  // lifetime. On a miss, fetch + Thrift-parse the footer once and park it so the
  // subsequent scan reuses it. Mirrors parquet_gpu_ingestible::build_file_scan_info,
  // so the footer is parsed exactly once per file per process.
  std::shared_ptr<cudf::io::parquet::FileMetaData const> file_metadata;
  if (auto cached = datasource->metadata()) {
    if (auto pm = std::dynamic_pointer_cast<op::scan::parquet_metadata>(std::move(cached))) {
      file_metadata = pm->file_metadata();
    }
  }
  if (!file_metadata) {
    auto footer_buffer         = cudf::io::parquet::fetch_footer_to_host(*datasource);
    auto const footer_byte_len = footer_buffer->size();
    auto reader_options        = cudf::io::parquet_reader_options::builder().build();
    cudf::io::parquet::experimental::hybrid_scan_reader reader{
      cudf::host_span<std::uint8_t const>(footer_buffer->data(), footer_buffer->size()),
      reader_options};
    file_metadata =
      std::make_shared<cudf::io::parquet::FileMetaData const>(reader.parquet_metadata());
    [[maybe_unused]] auto const stored = datasource->store_metadata(
      std::make_shared<op::scan::parquet_metadata>(file_metadata, footer_byte_len));
  }

  auto schema = sirius::io::parquet_helpers::extract_schema(*file_metadata);

  parquet_bind_result result;
  result.return_types   = std::move(schema.types);
  result.names          = std::move(schema.names);
  result.object_size    = datasource->size();
  result.total_num_rows = static_cast<std::size_t>(file_metadata->num_rows);
  return result;
}

void sirius_scan_manager::prepare_for_query(const sirius::planner::query& query,
                                            bool enable_pinned_zone_map_pruning)
{
  _pruning_enabled = enable_pinned_zone_map_pruning;
  reset();

  if (_io_ctx && _io_ctx->cache()) {
    SIRIUS_LOG_INFO("[sirius_scan_manager] cache summary: {}", _io_ctx->cache()->summary());
    _io_ctx->cache()->prepare_for_query(query);
  }

  // Routed ioctxs (e.g. the restful context serving s3://) are built lazily and
  // reused across queries; advance their caches to this query too, or a routed
  // cache's epoch freezes at build time and a later query serves the prior
  // query's cached chunks as current.
  {
    std::lock_guard lk{_routed_io_ctxs_mtx};
    for (auto& [type, io_ctx] : _routed_io_ctxs) {
      if (io_ctx && io_ctx->cache()) { io_ctx->cache()->prepare_for_query(query); }
    }
  }

  auto const gpu_ids = _topology_index->gpu_ids();
  auto round_robin =
    std::make_shared<round_robin_strategy>(std::vector<int>(gpu_ids.begin(), gpu_ids.end()));

  _metadata_processor = std::make_unique<load_balancing_scan_batch_coalescer>();

  for (auto const& scan_op : query.get_scan_operators()) {
    if (scan_op->type != ::sirius::op::SiriusPhysicalOperatorType::GPU_SCAN) { continue; }
    auto* op = &scan_op->Cast<op::scan::sirius_gpu_scan_operator>();
    if (_providers_by_op.find(op) != _providers_by_op.end()) { continue; }
    _metadata_processor->register_pipeline(op, round_robin);
    // On a pinned-cache hit the coalescer serves this operator from the cached
    // batch_provider (process_cached_entries); skip the disk-reading
    // split_provider entirely so no read is issued for the cached scan.
    if (try_assign_cached_entries(op)) {
      _scan_op_order.push_back(op);
      continue;
    }
    auto provider = std::make_unique<split_provider>(
      op->get_ingestible(),
      [this](std::string_view file_path) -> std::shared_ptr<io::sirius_ioctx> {
        auto io_ctx = ioctx_for_path(file_path);
        if (!io_ctx) {
          throw std::runtime_error("scan_manager: no backend supports path: " +
                                   std::string(file_path));
        }
        return io_ctx;
      });
    _providers_by_op.emplace(op, std::move(provider));
    _scan_op_order.push_back(op);
  }

  if (_scan_op_order.empty()) {
    SIRIUS_LOG_WARN(
      "[sirius_scan_manager::prepare_for_query] no GPU scan operators found in query");
    return;
  }

  start_metadata_processing();
}

void sirius_scan_manager::start_metadata_processing()
{
  _metadata_processor->spawn_workers(*_dispatcher);
  for (auto* op : _scan_op_order) {
    auto it = _providers_by_op.find(op);
    if (it == _providers_by_op.end()) { continue; }
    it->second->run(*_dispatcher, _metadata_processor->get_split_provider_bridge(op));
  }
}

std::shared_ptr<sirius::io::sirius_datasource> sirius_scan_manager::create_datasource(
  std::string_view path, sirius::io::open_hint hint)
{
  auto file_path = normalize_path(std::string(path));
  auto io_ctx    = ioctx_for_path(file_path);
  if (!io_ctx) { return nullptr; }  // no backend supports the path
  // Real I/O / HEAD / auth / missing-object errors propagate as exceptions;
  // only "no backend" is reported as nullptr (callers map it to that message).
  return io_ctx->open_datasource(file_path, hint);
}

std::shared_ptr<sirius::io::sirius_ioctx> sirius_scan_manager::ioctx_for_path(std::string_view path)
{
  // Normalize here so every caller (incl. the scan resolver, which forwards raw
  // ingestible paths) routes `file://` the same way create_datasource does.
  auto file_path = normalize_path(std::string(path));
  auto type      = _ioctx_registry.lookup_path(file_path);
  if (!type) { return nullptr; }
  // The local default `_io_ctx` already serves uring/kvikio; only an off-default
  // backend (e.g. s3:// -> restful) needs a separate, lazily-built context.
  if (_io_ctx && _io_ctx->type() == *type) { return _io_ctx; }

  {
    std::lock_guard lk{_routed_io_ctxs_mtx};
    if (auto it = _routed_io_ctxs.find(*type); it != _routed_io_ctxs.end()) { return it->second; }
  }
  // Build outside the map mutex: make_ioctx/start spawn reactor threads and
  // initialize_cache allocates, so holding _routed_io_ctxs_mtx across them would
  // park every concurrent lookup behind one long critical section. The build
  // mutex serializes builders instead, so two first-touches of the same type
  // never construct twice (a losing ioctx would need drain/stop teardown).
  std::lock_guard build_lk{_routed_io_ctxs_build_mtx};
  {
    std::lock_guard lk{_routed_io_ctxs_mtx};
    if (auto it = _routed_io_ctxs.find(*type); it != _routed_io_ctxs.end()) { return it->second; }
  }
  auto io_ctx = _ioctx_registry.make_ioctx(*type);
  if (!io_ctx) { return nullptr; }
  io_ctx->start();
  if (_config.enable_prefetch_cache && io_ctx->can_use_prefetching_cache()) {
    io_ctx->initialize_cache(_reservation_manager, _config.cache, _topology_index);
  }
  std::lock_guard lk{_routed_io_ctxs_mtx};
  auto [it, inserted] = _routed_io_ctxs.emplace(*type, std::move(io_ctx));
  return it->second;
}

void sirius_scan_manager::reset()
{
  _dispatcher->request_stop();
  _dispatcher->wait_for_all();
  _scan_op_order.clear();
  _providers_by_op.clear();
  _metadata_processor.reset();
  _dispatcher = std::make_unique<exec::scoped_dispatcher>(_thread_pool, _thread_pool.num_threads());
}

void sirius_scan_manager::start() {}

void sirius_scan_manager::stop()
{
  reset();
  _thread_pool.stop();
}

namespace {

// Gather positions into @p cached_ids for each requested primary (storage) index, in the
// given order. Empty when any requested column is absent — i.e. the cache is not a superset.
std::vector<std::size_t> gather_by_primary_index(
  duckdb::vector<duckdb::ColumnIndex> const& cached_ids,
  std::vector<std::size_t> const& requested_primary_indices)
{
  std::unordered_map<duckdb::idx_t, std::size_t> pos;
  pos.reserve(cached_ids.size());
  for (std::size_t i = 0; i < cached_ids.size(); ++i) {
    pos.emplace(cached_ids[i].GetPrimaryIndex(), i);
  }
  std::vector<std::size_t> projection;
  projection.reserve(requested_primary_indices.size());
  for (auto const primary_idx : requested_primary_indices) {
    auto it = pos.find(primary_idx);
    if (it == pos.end()) { return {}; }  // cache lacks a requested column
    projection.push_back(it->second);
  }
  return projection;
}

// Gather projection that lets a cache holding @p cached_ids (by primary/storage
// index) serve a scan requesting @p requested_ids: for each requested column,
// its position within @p cached_ids, in the requested order. Empty when any
// requested column is absent — i.e. the cache is not a column superset.
std::vector<std::size_t> column_superset_projection(
  duckdb::vector<duckdb::ColumnIndex> const& cached_ids,
  duckdb::vector<duckdb::ColumnIndex> const& requested_ids)
{
  std::vector<std::size_t> requested_primary_indices;
  requested_primary_indices.reserve(requested_ids.size());
  for (auto const& c : requested_ids) {
    requested_primary_indices.push_back(c.GetPrimaryIndex());
  }
  return gather_by_primary_index(cached_ids, requested_primary_indices);
}

// column_ids-aligned names: for each column_ids[i], the full-schema name at its
// primary (storage) index — the keys data_batches_by_column / the gather use.
std::vector<std::string> aligned_column_names(duckdb::vector<std::string> const& full_names,
                                              duckdb::vector<duckdb::ColumnIndex> const& column_ids)
{
  std::vector<std::string> out;
  out.reserve(column_ids.size());
  for (auto const& c : column_ids) {
    auto const p = static_cast<std::size_t>(c.GetPrimaryIndex());
    out.push_back(p < full_names.size() ? full_names[p] : std::string{});
  }
  return out;
}

}  // namespace

cache_entry_info cache_entry_info::from(const op::scan::ingestible_table_info& info)
{
  cache_entry_info ci;
  if (auto const* p = dynamic_cast<op::scan::parquet_ingestible_table_info const*>(&info)) {
    ci.resolved_file_paths = p->resolved_file_paths;
    ci.column_ids          = p->column_ids;
    ci.names               = aligned_column_names(p->names, p->column_ids);
  } else if (auto const* d =
               dynamic_cast<op::scan::duckdb_native_ingestible_table_info const*>(&info)) {
    ci.catalog_name = d->catalog_name;
    ci.schema_name  = d->schema_name;
    ci.table_name   = d->table_name;
    ci.column_ids   = d->column_ids;
    ci.names        = aligned_column_names(d->names, d->column_ids);
  }
  return ci;
}

std::vector<std::size_t> cache_entry_info::can_serve_with_columns(
  const op::scan::ingestible_table_info& other) const
{
  // A parquet pin serves a parquet scan over the same file set; a duckdb pin
  // serves a duckdb scan over the same catalog.schema.table. A cache of one format
  // never serves a scan of the other — the identity check below falls through (a
  // duckdb cache has empty resolved_file_paths; a parquet cache has an empty table_name).
  if (auto const* p = dynamic_cast<op::scan::parquet_ingestible_table_info const*>(&other)) {
    if (resolved_file_paths.size() != p->resolved_file_paths.size()) { return {}; }
    auto these_files = resolved_file_paths;
    auto those_files = p->resolved_file_paths;
    std::sort(these_files.begin(), these_files.end());
    std::sort(those_files.begin(), those_files.end());
    if (these_files != those_files) { return {}; }
    return column_superset_projection(column_ids, p->column_ids);
  }
  if (auto const* d = dynamic_cast<op::scan::duckdb_native_ingestible_table_info const*>(&other)) {
    // Same duckdb table by qualified name (catalog.schema.table), derived on both
    // pin and query sides from the resolved DuckTableEntry — so the stored casing is
    // the table's canonical (case-preserved) name on both sides and a byte-exact
    // compare is correct. (If a future site ever populates these from parsed input
    // rather than the resolved entry, switch to a case-insensitive compare.)
    // A parquet cache has an empty table_name, so it never matches a duckdb scan.
    if (table_name.empty()) { return {}; }
    if (catalog_name != d->catalog_name || schema_name != d->schema_name ||
        table_name != d->table_name) {
      return {};
    }
    return column_superset_projection(column_ids, d->column_ids);
  }
  return {};
}

void sirius_scan_manager::insert_pinned_entry(
  const std::string& name,
  cache_entry_info cache_info,
  std::vector<std::unique_ptr<cudf::table>> data_tables,
  std::vector<cucascade::memory::memory_space*> chunk_memory_spaces,
  std::unique_ptr<duckdb_mvcc_metadata> mvcc,
  duckdb::vector<duckdb::LogicalType> column_types,
  std::vector<std::vector<duckdb::unique_ptr<duckdb::BaseStatistics>>> chunk_stats)
{
  validate_mvcc_publication(cache_info, mvcc);
  // chunk_memory_spaces is parallel to data_tables — the caller
  // (PinTableFunction) emits one memory_space* per
  // chunked_parquet_reader::read_chunk() result, and there is exactly one
  // cudf::table per chunk in data_tables. Reject any misalignment loudly
  // rather than silently aliasing chunks to the wrong GPU.
  if (chunk_memory_spaces.size() != data_tables.size()) {
    throw std::invalid_argument(
      "[sirius_scan_manager::insert_pinned_entry] chunk_memory_spaces.size() (" +
      std::to_string(chunk_memory_spaces.size()) + ") must equal data_tables.size() (" +
      std::to_string(data_tables.size()) + ")");
  }

  // Column names (aligned with the cached column_ids) key data_batches_by_column.
  // Copied out before cache_info is moved into the entry below.
  std::vector<std::string> column_names = cache_info.column_names();

  // column_ids and names within cache_info are built aligned 1:1 by
  // cache_entry_info::from; the merge path below indexes column_ids by the same
  // position as the column names, so reject any misalignment loudly rather than
  // risk an out-of-bounds access.
  if (cache_info.column_ids.size() != column_names.size()) {
    throw std::invalid_argument(
      "[sirius_scan_manager::insert_pinned_entry] cache_info.column_ids.size() (" +
      std::to_string(cache_info.column_ids.size()) + ") must equal column_names size (" +
      std::to_string(column_names.size()) + ")");
  }

  // Validate every parallel input before a merge can mutate an existing entry.
  // Accumulate rows here too, while each table is still intact.
  std::size_t new_num_rows = 0;
  for (std::size_t index = 0; index < data_tables.size(); ++index) {
    auto const& table = data_tables[index];
    if (!table) {
      throw std::invalid_argument("[sirius_scan_manager::insert_pinned_entry] data_tables[" +
                                  std::to_string(index) + "] must be non-null");
    }
    if (static_cast<std::size_t>(table->num_columns()) != column_names.size()) {
      throw std::invalid_argument("[sirius_scan_manager::insert_pinned_entry] data_tables[" +
                                  std::to_string(index) +
                                  "] column count must equal cache_info column count");
    }
    auto const* space = chunk_memory_spaces[index];
    if (space == nullptr || space->get_tier() != cucascade::memory::Tier::GPU) {
      throw std::invalid_argument(
        "[sirius_scan_manager::insert_pinned_entry] chunk_memory_spaces[" + std::to_string(index) +
        "] must be a non-null GPU-tier memory space");
    }
    add_rows_checked(new_num_rows, table->num_rows(), "raw GPU chunk");
  }

  // Normalize the optional zone-map capture before cache_info is moved into the entry.
  bool const stats_supplied = !column_types.empty() && !chunk_stats.empty();
  auto pin_zone_maps        = pinned_zone_maps::from_capture(std::move(column_types),
                                                      std::move(chunk_stats),
                                                      cache_info.column_ids.size(),
                                                      data_tables.size());
  if (stats_supplied && !pin_zone_maps.has_stats()) {
    SIRIUS_LOG_WARN(
      "[sirius_scan_manager::insert_pinned_entry] zone-map capture failure; pinning '{}' without "
      "statistics",
      name);
  }

  pinned_entry incoming;
  incoming.cache_info          = std::move(cache_info);
  incoming.chunk_memory_spaces = std::move(chunk_memory_spaces);
  incoming.tier                = cucascade::memory::Tier::GPU;
  incoming.num_rows            = new_num_rows;
  incoming.memory_space =
    incoming.chunk_memory_spaces.empty() ? nullptr : incoming.chunk_memory_spaces.front();
  incoming.mvcc      = std::move(mvcc);
  incoming.zone_maps = std::move(pin_zone_maps);
  release_tables_into(incoming, data_tables, column_names);
  validate_entry_before_install(incoming);

  std::lock_guard pinned_entries_lock{_pinned_entries_mtx};
  auto existing_it = _pinned_entries.find(name);
  if (existing_it != _pinned_entries.end()) {
    auto const& existing = existing_it->second;
    bool const mergeable_raw_gpu =
      existing.tier == cucascade::memory::Tier::GPU && existing.logical_order.empty() &&
      existing.compressed_device_chunks.empty() && existing.compressed_host_chunks.empty() &&
      existing.host_chunks.empty() && !existing.data_batches_by_column.empty();
    // Column merge is defined only for an existing homogeneous raw-GPU entry.
    // A compressed or mixed entry has a different chunk model and must be replaced,
    // even when its total row count happens to match the incoming raw materialization.
    if (mergeable_raw_gpu && !incoming.data_batches_by_column.empty() &&
        existing.num_rows == incoming.num_rows &&
        has_same_cache_source(existing.cache_info, incoming.cache_info)) {
      // Same-row-count merge MUST preserve per-chunk memory_space alignment
      // between existing and new entry. The round-robin counter restarts at
      // chunk 0 → GPU 0 per pin_table call, and chunks at index i across all
      // columns share a memory_space because they came from the same
      // chunked_parquet_reader::read_chunk() call. Two pin_table calls of the
      // same file_paths with the same chunk_read_limit MUST therefore produce
      // identical chunk_memory_spaces vectors. Reject any mismatch loudly
      // rather than silently aliasing.
      auto& entry = existing_it->second;
      if (entry.chunk_memory_spaces.size() != incoming.chunk_memory_spaces.size()) {
        throw std::runtime_error(
          "[sirius_scan_manager::insert_pinned_entry] merge mismatch — "
          "existing.chunk_memory_spaces.size() (" +
          std::to_string(entry.chunk_memory_spaces.size()) +
          ") != new chunk_memory_spaces.size() (" +
          std::to_string(incoming.chunk_memory_spaces.size()) + ")");
      }
      for (std::size_t i = 0; i < incoming.chunk_memory_spaces.size(); ++i) {
        if (entry.chunk_memory_spaces[i] != incoming.chunk_memory_spaces[i]) {
          throw std::runtime_error(
            "[sirius_scan_manager::insert_pinned_entry] merge mismatch — "
            "chunk_memory_spaces[" +
            std::to_string(i) + "] differs between existing and new entry");
        }
      }
      // Per-chunk ROW counts must match too: the byte-budget coalescer can slice
      // the same table at different row-group boundaries for a different column
      // set (varchar byte budgets are data-dependent), which still yields equal
      // chunk counts and — because round-robin placement is a function of chunk
      // index alone — identical memory_spaces. Merging such chunks would corrupt
      // the entry positionally (columns disagreeing on chunk boundaries) and
      // invalidate the per-chunk MVCC row-count map published with the entry.
      auto const& existing_chunks =
        entry.data_batches_by_column.at(entry.cache_info.column_names().front());
      auto const& incoming_chunks =
        incoming.data_batches_by_column.at(incoming.cache_info.column_names().front());
      if (existing_chunks.size() != incoming_chunks.size()) {
        throw std::runtime_error(
          "[sirius_scan_manager::insert_pinned_entry] merge mismatch — existing entry has " +
          std::to_string(existing_chunks.size()) + " chunks but the new materialization has " +
          std::to_string(incoming_chunks.size()));
      }
      for (std::size_t i = 0; i < incoming_chunks.size(); ++i) {
        if (existing_chunks[i]->size() != incoming_chunks[i]->size()) {
          throw std::runtime_error(
            "[sirius_scan_manager::insert_pinned_entry] merge mismatch — chunk " +
            std::to_string(i) + " has " + std::to_string(existing_chunks[i]->size()) +
            " rows in the existing entry but " + std::to_string(incoming_chunks[i]->size()) +
            " in the new materialization (same total, different chunk boundaries)");
        }
      }

      // Stage every allocation and ownership transfer away from the published
      // map element. Failure leaves the prior entry intact; the final swap cannot throw.
      pinned_entry merged;
      merged.cache_info             = entry.cache_info;
      merged.data_batches_by_column = entry.data_batches_by_column;
      merged.chunk_memory_spaces    = entry.chunk_memory_spaces;
      merged.tier                   = entry.tier;
      merged.memory_space           = entry.memory_space;
      merged.num_rows               = entry.num_rows;
      merged.mvcc                   = std::move(incoming.mvcc);
      // zone_maps owns move-only per-chunk statistics, so the merged entry takes the existing
      // sidecar over rather than copying it, then extends it with the newly merged columns.
      bool const entry_had_stats = entry.zone_maps.has_stats();
      merged.zone_maps           = std::move(entry.zone_maps);

      for (std::size_t i = 0; i < column_names.size(); ++i) {
        auto const& column_name = column_names[i];
        if (merged.data_batches_by_column.contains(column_name)) { continue; }
        auto incoming_column = incoming.data_batches_by_column.find(column_name);
        if (incoming_column == incoming.data_batches_by_column.end()) { continue; }
        merged.data_batches_by_column.emplace(column_name, std::move(incoming_column->second));
        merged.cache_info.column_ids.push_back(incoming.cache_info.column_ids[i]);
        merged.cache_info.names.push_back(column_name);
        // Extend the zone-map sidecar with this newly merged column's statistics.
        merged.zone_maps.append_column_from(incoming.zone_maps, i);
      }
      if (entry_had_stats && !merged.zone_maps.has_stats()) {
        // append_column_from cleared the sidecar because a new column's zone-map shape
        // disagreed with the existing entry's shape. Warn but still pin the entry.
        SIRIUS_LOG_WARN(
          "[sirius_scan_manager::insert_pinned_entry] merge appended columns without "
          "statistics; dropping zone-map statistics for entry '{}'",
          name);
      }
      // A statless entry adopts the incoming capture when it covers every entry column (by
      // primary index) — the re-pin recovery path for entries pinned while capture was off.
      // Safe because the merge guards above proved identical chunk boundaries.
      if (!merged.zone_maps.has_stats() && incoming.zone_maps.has_stats()) {
        std::unordered_map<duckdb::idx_t, std::size_t> incoming_pos_by_primary;
        incoming_pos_by_primary.reserve(incoming.cache_info.column_ids.size());
        for (std::size_t i = 0; i < incoming.cache_info.column_ids.size(); ++i) {
          incoming_pos_by_primary.emplace(incoming.cache_info.column_ids[i].GetPrimaryIndex(), i);
        }
        std::vector<std::size_t> incoming_pos_by_entry_pos;
        incoming_pos_by_entry_pos.reserve(merged.cache_info.column_ids.size());
        for (auto const& col : merged.cache_info.column_ids) {
          auto it = incoming_pos_by_primary.find(col.GetPrimaryIndex());
          if (it == incoming_pos_by_primary.end()) { break; }
          incoming_pos_by_entry_pos.push_back(it->second);
        }
        if (incoming_pos_by_entry_pos.size() == merged.cache_info.column_ids.size()) {
          merged.zone_maps =
            pinned_zone_maps::remap(std::move(incoming.zone_maps), incoming_pos_by_entry_pos);
        }
      }
      validate_entry_before_install(merged);
      swap_pinned_entries(entry, merged);
      return;
    }
    // Non-mergeable, different-source, or different-row-count entries are replaced below.
  }

  publish_pinned_entry(_pinned_entries, name, incoming);
}

void sirius_scan_manager::insert_pinned_entry_host(
  const std::string& name,
  cache_entry_info cache_info,
  std::vector<std::shared_ptr<cucascade::host_data_representation>> host_chunks,
  cucascade::memory::memory_space& memory_space,
  std::unique_ptr<duckdb_mvcc_metadata> mvcc,
  duckdb::vector<duckdb::LogicalType> column_types,
  std::vector<std::vector<duckdb::unique_ptr<duckdb::BaseStatistics>>> chunk_stats)
{
  validate_mvcc_publication(cache_info, mvcc);
  // The host-tier path captures one chunk per emitted batch; each chunk holds every
  // pinned column. Re-insert always replaces — there is no per-column merge analog
  // to the GPU path because the chunk-vs-column dimensions are flipped.
  std::size_t new_num_rows = 0;
  for (auto const& chunk : host_chunks) {
    if (!chunk) { continue; }
    auto const& host_table = chunk->get_host_table();
    if (host_table && !host_table->columns.empty()) {
      add_rows_checked(new_num_rows, host_table->columns.front().num_rows, "raw HOST chunk");
    }
  }

  // Normalize the optional zone-map capture
  bool const stats_supplied = !column_types.empty() && !chunk_stats.empty();
  auto pin_zone_maps        = pinned_zone_maps::from_capture(std::move(column_types),
                                                      std::move(chunk_stats),
                                                      cache_info.column_ids.size(),
                                                      host_chunks.size());
  if (stats_supplied && !pin_zone_maps.has_stats()) {
    // Only reason stats failed to append is a shape mismatch between the captured stats and the
    // pinned entry; warn but still pin the entry.
    SIRIUS_LOG_WARN(
      "[sirius_scan_manager::insert_pinned_entry_host] zone-map capture shape mismatch; "
      "pinning '{}' without statistics",
      name);
  }

  pinned_entry entry;
  entry.cache_info   = std::move(cache_info);
  entry.tier         = cucascade::memory::Tier::HOST;
  entry.memory_space = &memory_space;
  entry.num_rows     = new_num_rows;
  entry.host_chunks  = std::move(host_chunks);
  entry.mvcc         = std::move(mvcc);
  entry.zone_maps    = std::move(pin_zone_maps);
  validate_entry_before_install(entry);

  std::lock_guard pinned_entries_lock{_pinned_entries_mtx};
  publish_pinned_entry(_pinned_entries, name, entry);
}

void sirius_scan_manager::insert_pinned_entry_host_compressed(
  const std::string& name,
  cache_entry_info cache_info,
  host_pinned_chunks chunks,
  cucascade::memory::memory_space& memory_space,
  std::unique_ptr<duckdb_mvcc_metadata> mvcc)
{
  validate_mvcc_publication(cache_info, mvcc);
  auto compressed_chunks = std::move(chunks.compressed);
  auto raw_chunks        = std::move(chunks.raw);
  auto logical_order     = std::move(chunks.logical_order);
  std::size_t new_num_rows{0};
  for (auto const& chunk : compressed_chunks) {
    if (!chunk) {
      throw std::invalid_argument(
        "[sirius_scan_manager::insert_pinned_entry_host_compressed] null compressed chunk");
    }
    add_rows_checked(new_num_rows, chunk->num_rows(), "compressed HOST chunk");
  }
  for (auto const& chunk : raw_chunks) {
    if (!chunk) {
      throw std::invalid_argument(
        "[sirius_scan_manager::insert_pinned_entry_host_compressed] null raw chunk");
    }
    auto const& host_table = chunk->get_host_table();
    if (!host_table) {
      throw std::invalid_argument(
        "[sirius_scan_manager::insert_pinned_entry_host_compressed] raw chunk has no table");
    }
    if (!host_table->columns.empty()) {
      add_rows_checked(new_num_rows, host_table->columns.front().num_rows, "raw HOST chunk");
    }
  }

  pinned_entry entry;
  entry.cache_info             = std::move(cache_info);
  entry.tier                   = cucascade::memory::Tier::HOST;
  entry.memory_space           = &memory_space;
  entry.num_rows               = new_num_rows;
  entry.compressed_host_chunks = std::move(compressed_chunks);
  entry.host_chunks            = std::move(raw_chunks);
  entry.logical_order          = std::move(logical_order);
  entry.mvcc                   = std::move(mvcc);
  validate_entry_before_install(entry);

  SIRIUS_LOG_DEBUG(
    "[sirius_scan_manager::insert_pinned_entry_host_compressed] '{}' compressed={} raw={} rows={}",
    name,
    entry.compressed_host_chunks.size(),
    entry.host_chunks.size(),
    new_num_rows);

  std::lock_guard pinned_entries_lock{_pinned_entries_mtx};
  publish_pinned_entry(_pinned_entries, name, entry);
}

void sirius_scan_manager::insert_pinned_entry_device_compressed(
  const std::string& name,
  cache_entry_info cache_info,
  device_pinned_chunks chunks,
  cucascade::memory::memory_space& memory_space,
  std::unique_ptr<duckdb_mvcc_metadata> mvcc)
{
  validate_mvcc_publication(cache_info, mvcc);
  auto compressed_chunks       = std::move(chunks.compressed);
  auto raw_tables              = std::move(chunks.raw);
  auto raw_chunk_memory_spaces = std::move(chunks.raw_memory_spaces);
  auto logical_order           = std::move(chunks.logical_order);
  if (raw_chunk_memory_spaces.size() != raw_tables.size()) {
    throw std::invalid_argument(
      "[sirius_scan_manager::insert_pinned_entry_device_compressed] "
      "raw_chunk_memory_spaces and raw_tables must have equal size");
  }
  for (auto const* space : raw_chunk_memory_spaces) {
    if (space == nullptr) {
      throw std::invalid_argument(
        "[sirius_scan_manager::insert_pinned_entry_device_compressed] "
        "null raw memory_space");
    }
    if (space->get_tier() != cucascade::memory::Tier::GPU) {
      throw std::invalid_argument(
        "[sirius_scan_manager::insert_pinned_entry_device_compressed] "
        "raw memory_space must be GPU tier");
    }
  }

  std::vector<std::string> column_names = cache_info.column_names();
  std::size_t new_num_rows{0};
  for (auto const& chunk : compressed_chunks) {
    if (!chunk) {
      throw std::invalid_argument(
        "[sirius_scan_manager::insert_pinned_entry_device_compressed] null compressed chunk");
    }
    add_rows_checked(new_num_rows, chunk->num_rows(), "compressed GPU chunk");
  }
  for (auto const& table : raw_tables) {
    if (!table) {
      throw std::invalid_argument(
        "[sirius_scan_manager::insert_pinned_entry_device_compressed] null raw table");
    }
    add_rows_checked(new_num_rows, table->num_rows(), "raw GPU chunk");
  }

  pinned_entry entry;
  entry.cache_info               = std::move(cache_info);
  entry.tier                     = cucascade::memory::Tier::GPU;
  entry.memory_space             = &memory_space;
  entry.num_rows                 = new_num_rows;
  entry.compressed_device_chunks = std::move(compressed_chunks);
  entry.chunk_memory_spaces      = std::move(raw_chunk_memory_spaces);
  entry.logical_order            = std::move(logical_order);
  entry.mvcc                     = std::move(mvcc);
  release_tables_into(entry, raw_tables, column_names);
  validate_entry_before_install(entry);

  SIRIUS_LOG_DEBUG(
    "[sirius_scan_manager::insert_pinned_entry_device_compressed] '{}' compressed={} raw={} "
    "rows={}",
    name,
    entry.compressed_device_chunks.size(),
    entry.chunk_memory_spaces.size(),
    new_num_rows);

  std::lock_guard pinned_entries_lock{_pinned_entries_mtx};
  publish_pinned_entry(_pinned_entries, name, entry);
}

void sirius_scan_manager::attach_mvcc_metadata(const std::string& name,
                                               duckdb_mvcc_metadata metadata)
{
  auto replacement = std::make_unique<duckdb_mvcc_metadata>(std::move(metadata));

  std::lock_guard pinned_entries_lock{_pinned_entries_mtx};
  auto it = _pinned_entries.find(name);
  if (it == _pinned_entries.end()) {
    throw std::invalid_argument("[attach_mvcc_metadata] no pinned entry named '" + name + "'");
  }
  validate_mvcc_publication(it->second.cache_info, replacement);

  auto previous   = std::move(it->second.mvcc);
  it->second.mvcc = std::move(replacement);
  try {
    validate_entry_before_install(it->second);
  } catch (...) {
    it->second.mvcc = std::move(previous);
    throw;
  }
}

void sirius_scan_manager::remove_pinned_entry(const std::string& name)
{
  std::lock_guard pinned_entries_lock{_pinned_entries_mtx};
  _pinned_entries.erase(name);
}

void sirius_scan_manager::visit_pinned_entries(
  const std::function<bool(std::string_view, const pinned_entry&)>& visitor) const
{
  // Iterate the live registry and hand each entry to the visitor by reference. The lock is
  // intentionally not held across the callback: the visitor may re-enter pinned-entry
  // operations (e.g. remove_pinned_entry), which would otherwise deadlock, and it inspects
  // move-only entry state (zone_maps) that a copy could not carry. Returning false stops the
  // walk before the iterator can be invalidated by such a re-entrant mutation.
  for (auto const& [name, entry] : _pinned_entries) {
    if (!visitor(name, entry)) { break; }
  }
}

void validate_pinned_entry_for_serving(pinned_entry const& entry,
                                       std::span<const std::size_t> selected_columns)
{
  auto const& column_names = entry.cache_info.column_names();
  if (entry.cache_info.column_ids.size() != column_names.size()) {
    throw std::runtime_error("pinned entry's column ids and names are not aligned");
  }

  bool const any_duckdb_identity = !entry.cache_info.catalog_name.empty() ||
                                   !entry.cache_info.schema_name.empty() ||
                                   !entry.cache_info.table_name.empty();
  bool const duckdb_identity = !entry.cache_info.catalog_name.empty() &&
                               !entry.cache_info.schema_name.empty() &&
                               !entry.cache_info.table_name.empty();
  bool const parquet_identity = !entry.cache_info.resolved_file_paths.empty();
  if (any_duckdb_identity && !duckdb_identity) {
    throw std::runtime_error("pinned entry has an incomplete DuckDB cache identity");
  }
  if (duckdb_identity && parquet_identity) {
    throw std::runtime_error("pinned entry cannot have both DuckDB and parquet identities");
  }
  if (duckdb_identity != static_cast<bool>(entry.mvcc)) {
    throw std::runtime_error(duckdb_identity ? "DuckDB-native pinned entry has no MVCC metadata"
                                             : "non-DuckDB pinned entry carries MVCC metadata");
  }

  for (std::size_t const index : selected_columns) {
    if (index >= column_names.size()) {
      throw std::runtime_error("pinned entry selection contains an out-of-range column index");
    }
  }

  bool const gpu  = entry.tier == cucascade::memory::Tier::GPU;
  bool const host = entry.tier == cucascade::memory::Tier::HOST;
  if (!gpu && !host) { throw std::runtime_error("pinned entry has an unsupported tier"); }
  bool const has_storage = !entry.data_batches_by_column.empty() ||
                           !entry.chunk_memory_spaces.empty() || !entry.host_chunks.empty() ||
                           !entry.compressed_host_chunks.empty() ||
                           !entry.compressed_device_chunks.empty();
  if (has_storage && entry.memory_space == nullptr) {
    throw std::runtime_error("pinned entry has a null representative memory_space");
  }
  if (entry.memory_space != nullptr && entry.memory_space->get_tier() != entry.tier) {
    throw std::runtime_error("pinned entry's representative memory_space has the wrong tier");
  }

  if (gpu && (!entry.host_chunks.empty() || !entry.compressed_host_chunks.empty())) {
    throw std::runtime_error("GPU pinned entry contains HOST-tier storage");
  }
  if (host && (!entry.data_batches_by_column.empty() || !entry.chunk_memory_spaces.empty() ||
               !entry.compressed_device_chunks.empty())) {
    throw std::runtime_error("HOST pinned entry contains GPU-tier storage");
  }

  std::vector<std::size_t> compressed_row_counts;
  auto validate_compressed =
    [&](auto const& chunks, char const* tier_name, cucascade::memory::Tier expected_tier) {
      for (auto const& chunk : chunks) {
        if (!chunk) {
          throw std::runtime_error(std::string{"pinned entry has a null compressed "} + tier_name +
                                   " chunk");
        }
        if (chunk->get_memory_space().get_tier() != expected_tier) {
          throw std::runtime_error(std::string{"pinned entry's compressed "} + tier_name +
                                   " chunk has the wrong memory tier");
        }
        if (chunk->selected_indices().has_value()) {
          throw std::runtime_error(std::string{"pinned entry stores a projected compressed "} +
                                   tier_name + " chunk");
        }
        if (chunk->column_names() != column_names) {
          throw std::runtime_error(std::string{"pinned entry's compressed "} + tier_name +
                                   " schema does not match its cache schema");
        }
        if (chunk->num_rows() < 0) {
          throw std::runtime_error(std::string{"pinned entry has a negative compressed "} +
                                   tier_name + " row count");
        }
        compressed_row_counts.push_back(static_cast<std::size_t>(chunk->num_rows()));
      }
    };
  validate_compressed(entry.compressed_host_chunks, "HOST", cucascade::memory::Tier::HOST);
  validate_compressed(entry.compressed_device_chunks, "GPU", cucascade::memory::Tier::GPU);

  std::size_t const n_compressed =
    gpu ? entry.compressed_device_chunks.size() : entry.compressed_host_chunks.size();
  std::size_t n_raw = 0;
  std::vector<std::size_t> raw_row_counts;

  if (gpu) {
    if (!entry.data_batches_by_column.empty() &&
        entry.data_batches_by_column.size() != column_names.size()) {
      throw std::runtime_error("GPU pinned entry's raw columns do not match its cache schema");
    }
    if (entry.data_batches_by_column.empty() && !entry.chunk_memory_spaces.empty()) {
      throw std::runtime_error("GPU pinned entry has raw placements but no raw columns");
    }

    n_raw = entry.chunk_memory_spaces.size();
    raw_row_counts.resize(n_raw);
    for (auto const* space : entry.chunk_memory_spaces) {
      if (space == nullptr) {
        throw std::runtime_error("pinned entry has a null chunk memory_space");
      }
      if (space->get_tier() != cucascade::memory::Tier::GPU) {
        throw std::runtime_error("pinned entry has a raw GPU chunk in a non-GPU memory space");
      }
    }
    for (std::size_t column_index = 0; column_index < column_names.size(); ++column_index) {
      auto const& name = column_names[column_index];
      auto const it    = entry.data_batches_by_column.find(name);
      if (it == entry.data_batches_by_column.end()) {
        if (n_raw == 0 && entry.data_batches_by_column.empty()) { continue; }
        throw std::runtime_error("pinned entry is missing raw column '" + name + "'");
      }
      if (it->second.size() != n_raw) {
        throw std::runtime_error("pinned column '" + name +
                                 "' does not cover every raw chunk of the entry");
      }
      for (std::size_t chunk_index = 0; chunk_index < n_raw; ++chunk_index) {
        auto const& chunk = it->second[chunk_index];
        if (!chunk) { throw std::runtime_error("pinned column '" + name + "' has a null chunk"); }
        auto const rows = static_cast<std::size_t>(chunk->size());
        if (column_index == 0) {
          raw_row_counts[chunk_index] = rows;
        } else if (raw_row_counts[chunk_index] != rows) {
          throw std::runtime_error("raw GPU chunk columns have different row counts at index " +
                                   std::to_string(chunk_index));
        }
      }
    }
  } else {
    n_raw = entry.host_chunks.size();
    raw_row_counts.reserve(n_raw);
    for (auto const& chunk : entry.host_chunks) {
      if (!chunk) { throw std::runtime_error("pinned entry has a null HOST chunk"); }
      auto const& table = chunk->get_host_table();
      if (chunk->get_memory_space().get_tier() != cucascade::memory::Tier::HOST) {
        throw std::runtime_error("pinned entry has a raw HOST chunk in a non-HOST memory space");
      }
      if (!table) { throw std::runtime_error("pinned entry has a HOST chunk with no table"); }
      if (table->columns.size() != column_names.size()) {
        throw std::runtime_error(
          "pinned entry's HOST chunk schema does not match its cache schema");
      }
      if (table->columns.empty()) {
        throw std::runtime_error("pinned entry has a HOST chunk with no row-count-bearing column");
      }
      auto const rows = table->columns.front().num_rows;
      if (rows < 0) { throw std::runtime_error("pinned entry has a negative HOST row count"); }
      for (auto const& column : table->columns) {
        if (column.num_rows != rows) {
          throw std::runtime_error("pinned entry's HOST chunk columns have different row counts");
        }
      }
      raw_row_counts.push_back(static_cast<std::size_t>(rows));
    }
  }

  std::vector<std::size_t> logical_chunk_rows;
  if (entry.logical_order.empty()) {
    if (n_compressed != 0 && n_raw != 0) {
      throw std::runtime_error("pinned entry populates both arms without a logical_order");
    }
    logical_chunk_rows = n_compressed != 0 ? compressed_row_counts : raw_row_counts;
  } else {
    if (n_compressed == 0 || n_raw == 0) {
      throw std::runtime_error("pinned entry has a logical_order but is not mixed");
    }
    if (n_compressed > std::numeric_limits<std::size_t>::max() - n_raw ||
        entry.logical_order.size() != n_compressed + n_raw) {
      throw std::runtime_error("pinned entry's logical_order does not densely cover both arms");
    }

    std::vector<bool> compressed_seen(n_compressed, false);
    std::vector<bool> raw_seen(n_raw, false);
    logical_chunk_rows.reserve(entry.logical_order.size());
    for (auto const& source : entry.logical_order) {
      std::vector<bool>* seen                    = nullptr;
      std::vector<std::size_t> const* row_counts = nullptr;
      switch (source.kind) {
        case chunk_kind::compressed:
          seen       = &compressed_seen;
          row_counts = &compressed_row_counts;
          break;
        case chunk_kind::raw:
          seen       = &raw_seen;
          row_counts = &raw_row_counts;
          break;
        default: throw std::runtime_error("pinned entry's logical_order has an invalid chunk kind");
      }
      if (source.arm_index >= seen->size() || (*seen)[source.arm_index]) {
        throw std::runtime_error(
          "pinned entry's logical_order is not a one-to-one cover of both arms");
      }
      (*seen)[source.arm_index] = true;
      logical_chunk_rows.push_back((*row_counts)[source.arm_index]);
    }
  }

  auto checked_sum = [](auto const& counts, std::string_view description) {
    std::size_t total = 0;
    for (std::size_t const count : counts) {
      if (count > std::numeric_limits<std::size_t>::max() - total) {
        throw std::overflow_error(std::string{description} + " row-count total overflowed");
      }
      total += count;
    }
    return total;
  };

  auto const stored_rows = checked_sum(logical_chunk_rows, "pinned storage");
  if (stored_rows != entry.num_rows) {
    throw std::runtime_error("pinned entry's stored row total does not match num_rows");
  }

  if (entry.mvcc) {
    auto const mvcc_rows =
      checked_sum(entry.mvcc->base_row_count_per_chunk, "pinned MVCC metadata");
    if (entry.mvcc->base_row_count_per_chunk != logical_chunk_rows) {
      throw std::runtime_error(
        "pinned entry's MVCC row counts do not match its logical chunk boundaries");
    }
    if (mvcc_rows != entry.num_rows) {
      throw std::runtime_error("pinned entry's MVCC row total does not match num_rows");
    }
  }
}

cached_scan_plan build_cached_scan_plan(pinned_entry const& entry,
                                        duckdb::TableFilterSet const* table_filters,
                                        duckdb::vector<duckdb::ColumnIndex> const* column_ids)
{
  // Chunk count mirrors cached_databatch_provider's serving cursor: mixed entries interleave
  // via logical_order, homogeneous compressed entries count their compressed arm, and raw
  // entries count their per-column batches. Compressed/mixed entries are statless, so they
  // fall through to the identity plan below.
  std::size_t n_chunks = 0;
  if (!entry.logical_order.empty()) {
    n_chunks = entry.logical_order.size();
  } else if (entry.tier == cucascade::memory::Tier::GPU) {
    if (!entry.compressed_device_chunks.empty()) {
      n_chunks = entry.compressed_device_chunks.size();
    } else if (!entry.data_batches_by_column.empty()) {
      n_chunks = entry.data_batches_by_column.begin()->second.size();
    }
  } else if (entry.tier == cucascade::memory::Tier::HOST) {
    n_chunks = !entry.compressed_host_chunks.empty() ? entry.compressed_host_chunks.size()
                                                     : entry.host_chunks.size();
  }

  // Identity plan (serve everything) until proven otherwise.
  cached_scan_plan plan;
  plan.survivor_chunk_indices.reserve(n_chunks);
  for (std::size_t c = 0; c < n_chunks; ++c) {
    plan.survivor_chunk_indices.push_back(c);
  }
  if (n_chunks == 0 || !entry.zone_maps.has_stats() || table_filters == nullptr ||
      table_filters->filters.empty() || column_ids == nullptr) {
    return plan;
  }

  // Filter keys are positions into the QUERY's column_ids (remapped at plan time by
  // create_table_filter_set); map each to its primary/storage index, then to the entry's positional
  // column.
  std::unordered_map<duckdb::idx_t, std::size_t> entry_pos_by_primary;
  entry_pos_by_primary.reserve(entry.cache_info.column_ids.size());
  for (std::size_t i = 0; i < entry.cache_info.column_ids.size(); ++i) {
    entry_pos_by_primary.emplace(entry.cache_info.column_ids[i].GetPrimaryIndex(), i);
  }

  struct usable_filter {
    duckdb::TableFilter const* filter;
    std::size_t entry_pos;
  };
  std::vector<usable_filter> usable;
  for (auto const& [col_idx, filter] : table_filters->filters) {
    if (!filter) { continue; }
    if (col_idx >= column_ids->size()) { continue; }  // defensive
    auto const& column_id = (*column_ids)[col_idx];
    // rowid / empty / virtual sentinels have no storage stats.
    if (!column_id.HasPrimaryIndex() || column_id.IsRowIdColumn() || column_id.IsEmptyColumn() ||
        column_id.IsVirtualColumn()) {
      continue;
    }
    auto it = entry_pos_by_primary.find(column_id.GetPrimaryIndex());
    if (it == entry_pos_by_primary.end()) { continue; }
    auto const pos = it->second;
    if (pos >= entry.zone_maps.column_count()) { continue; }  // absent for this column
    if (!filter_safe_for_stats(*filter, entry.zone_maps.column_type(pos))) { continue; }
    usable.push_back({filter.get(), pos});
  }
  if (usable.empty()) { return plan; }

  plan.survivor_chunk_indices.clear();
  for (std::size_t c = 0; c < n_chunks; ++c) {
    bool pruned = false;
    for (auto const& uf : usable) {
      auto const* cell = entry.zone_maps.cell(uf.entry_pos, c);
      if (cell != nullptr && chunk_provably_empty(*uf.filter, *cell)) {
        pruned = true;
        break;
      }
    }
    if (!pruned) { plan.survivor_chunk_indices.push_back(c); }
  }
  plan.pruned = n_chunks - plan.survivor_chunk_indices.size();

  // Sentinel chunk: an all-pruned scan must not become a zero-batch scan (zero
  // splits => zero tasks => pipeline completion never fires — the hazard both
  // disk paths guard against). Keep chunk 0; the GPU filter empties it.
  if (plan.survivor_chunk_indices.empty()) {
    plan.survivor_chunk_indices.push_back(0);
    plan.pruned = n_chunks - 1;
  }
  return plan;
}

bool sirius_scan_manager::try_assign_cached_entries(op::scan::sirius_gpu_scan_operator* op)
{
  const auto& table_info = op->get_ingestible().table_info();

  try {
    std::unique_ptr<databatch_provider> cached_provider;
    std::string cached_name;
    {
      std::lock_guard pinned_entries_lock{_pinned_entries_mtx};
      for (auto const& [pinned_name, entry] : _pinned_entries) {
        // Identity + serviceability gate: empty when this cache cannot serve the scan
        // (wrong format / file-set / table, or missing a requested column).
        if (entry.cache_info.can_serve_with_columns(table_info).empty()) { continue; }
        // Serve cached columns in the ingestible's materialized (disk-decode) order rather
        // than raw column_ids order, so post_filter_and_project's index-based filter and
        // projection bind to the same columns they would on the disk read path.
        auto cols = gather_by_primary_index(entry.cache_info.column_ids,
                                            op->get_ingestible().materialized_column_order());
        if (cols.empty()) { continue; }  // defensive: materialized set must be a cache subset
        // Serve-time defense: a malformed entry would end the cached batch stream
        // early (nullptr mid-stream reads as end-of-stream) and silently truncate
        // the scan; validate up front so it throws here and the catch below falls
        // back to the disk read instead.
        validate_pinned_entry_for_serving(entry, cols);
        // Zone-map survivor plan (built under the lock; the provider it feeds snapshots the
        // entry's owners, so serving can proceed after the lock is released).
        auto const filter_view =
          _pruning_enabled ? extract_scan_filters(table_info) : scan_filter_view{};
        auto plan =
          build_cached_scan_plan(entry, filter_view.table_filters, filter_view.column_ids);
        auto const total_chunks = plan.survivor_chunk_indices.size() + plan.pruned;
        if (plan.pruned > 0) {
          SIRIUS_LOG_INFO(
            "[sirius_scan_manager] zone-map pruning for pinned entry '{}' ({} tier): {}/{} chunks "
            "pruned, serving {}",
            pinned_name,
            entry.tier == cucascade::memory::Tier::GPU ? "GPU" : "HOST",
            plan.pruned,
            total_chunks,
            plan.survivor_chunk_indices.size());
        } else {
          SIRIUS_LOG_DEBUG(
            "[sirius_scan_manager] zone-map pruning for pinned entry '{}': nothing pruned ({} "
            "chunks served)",
            pinned_name,
            total_chunks);
        }
        cached_provider =
          make_provider_for_pinned_entry(entry, cols, std::move(plan), op->batch_telemetry());
        cached_name = pinned_name;
        break;
      }
    }
    if (cached_provider) {
      _metadata_processor->use_cached_entries_for_pipeline(op, std::move(cached_provider));
      SIRIUS_LOG_INFO("[sirius_scan_manager] assigned pinned entry '{}' to operator '{}'",
                      cached_name,
                      op->get_operator_id());
      return true;
    }
  } catch (std::exception const& e) {
    SIRIUS_LOG_ERROR(
      "[sirius_scan_manager] error while trying to assign cached entries to "
      "operator '{}': {}",
      op->get_operator_id(),
      e.what());
  } catch (...) {
    SIRIUS_LOG_ERROR(
      "[sirius_scan_manager] error while trying to assign cached entries to "
      "operator '{}'",
      op->get_operator_id());
  }
  return false;
}

}  // namespace sirius::scan_manager
