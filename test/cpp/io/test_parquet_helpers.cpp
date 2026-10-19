/*
 * Copyright 2026, Sirius Contributors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * See the LICENSE file at the repo root for the full text.
 */

#include "catch.hpp"
#include "io/cache/controlled_backend.hpp"
#include "io/parquet_helpers.hpp"
#include "io/templated_ioctx.hpp"
#include "io/uring/uring_ioctx.hpp"
#include "op/scan/parquet_materialize.hpp"
#include "scan/test_utils.hpp"
#include "utils/test_validation_utility.hpp"
#include "utils/utils.hpp"

#include <cudf/ast/expressions.hpp>
#include <cudf/io/datasource.hpp>
#include <cudf/io/experimental/hybrid_scan.hpp>
#include <cudf/io/experimental/hybrid_scan_multifile.hpp>
#include <cudf/io/parquet.hpp>
#include <cudf/scalar/scalar.hpp>
#include <cudf/utilities/default_stream.hpp>
#include <cudf/utilities/memory_resource.hpp>
#include <cudf/utilities/span.hpp>

#include <rmm/cuda_stream.hpp>
#include <rmm/device_buffer.hpp>

#include <duckdb.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

void require_ok(std::unique_ptr<duckdb::QueryResult> result)
{
  REQUIRE(result);
  INFO((result->HasError() ? result->GetError() : ""));
  REQUIRE_FALSE(result->HasError());
}

fs::path fresh_tmp_dir(std::string const& tag)
{
  auto dir = fs::temp_directory_path() / ("sirius_pr6_schema_" + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  return dir;
}

fs::path parquet_fixture(std::string_view file_name)
{
  return fs::path{SIRIUS_PROJECT_ROOT} / "test" / "cpp" / "integration" / "data" / "parquet" /
         file_name;
}

fs::path write_parquet(duckdb::Connection& con,
                       fs::path const& dir,
                       std::string const& table,
                       std::string const& create_sql)
{
  require_ok(con.Query("DROP TABLE IF EXISTS " + table));
  require_ok(con.Query(create_sql));

  auto path = dir / (table + ".parquet");
  require_ok(con.Query("COPY " + table + " TO '" + path.string() + "' (FORMAT PARQUET)"));
  return path;
}

std::unique_ptr<cudf::io::datasource::buffer> read_parquet_footer(cudf::io::datasource& source)
{
  auto constexpr footer_tail_size = sizeof(cudf::io::parquet::file_ender_s);
  auto const file_size            = source.size();
  REQUIRE(file_size >= footer_tail_size);

  auto tail = source.host_read(file_size - footer_tail_size, footer_tail_size);

  std::uint32_t footer_size = 0;
  std::memcpy(&footer_size, tail->data(), sizeof(footer_size));
  REQUIRE(file_size >= footer_tail_size + footer_size);

  return source.host_read(file_size - footer_tail_size - footer_size, footer_size);
}

cudf::io::parquet::FileMetaData read_metadata(fs::path const& path)
{
  auto source = cudf::io::datasource::create(path.string());
  auto footer = read_parquet_footer(*source);
  auto opts   = cudf::io::parquet_reader_options::builder().build();
  cudf::io::parquet::experimental::hybrid_scan_reader reader{
    cudf::host_span<std::uint8_t const>(footer->data(), footer->size()), opts};
  return reader.parquet_metadata();
}

std::vector<duckdb::LogicalType> expected_flat_types()
{
  return {duckdb::LogicalType::INTEGER,
          duckdb::LogicalType::BIGINT,
          duckdb::LogicalType::DOUBLE,
          duckdb::LogicalType::BOOLEAN,
          duckdb::LogicalType::VARCHAR};
}

void require_struct_child(duckdb::LogicalType const& type,
                          duckdb::idx_t index,
                          std::string const& name,
                          duckdb::LogicalType const& child_type)
{
  REQUIRE(type.id() == duckdb::LogicalTypeId::STRUCT);
  REQUIRE(duckdb::StructType::GetChildCount(type) > index);
  CHECK(duckdb::StructType::GetChildName(type, index) == name);
  CHECK(duckdb::StructType::GetChildType(type, index) == child_type);
}

}  // namespace

TEST_CASE("parquet_helpers extract_schema maps flat parquet leaves", "[parquet_helpers][schema]")
{
  auto const dir       = fresh_tmp_dir("flat");
  auto [db_owner, con] = sirius::make_test_db_and_connection();
  auto const path      = write_parquet(con,
                                  dir,
                                  "flat_types",
                                  "CREATE TABLE flat_types AS SELECT "
                                       "42::INTEGER AS i32_col, "
                                       "9000000000::BIGINT AS i64_col, "
                                       "1.25::DOUBLE AS double_col, "
                                       "true::BOOLEAN AS bool_col, "
                                       "'hello'::VARCHAR AS utf8_col");

  auto meta = read_metadata(path);
  auto info = sirius::io::parquet_helpers::extract_schema(meta);

  CHECK(info.names ==
        std::vector<std::string>{"i32_col", "i64_col", "double_col", "bool_col", "utf8_col"});
  REQUIRE(info.types.size() == expected_flat_types().size());
  auto expected = expected_flat_types();
  for (std::size_t i = 0; i < expected.size(); ++i) {
    CHECK(info.types[i] == expected[i]);
  }
}

TEST_CASE("parquet_helpers extract_schema maps decimal date and timestamp annotations",
          "[parquet_helpers][schema]")
{
  auto const dir       = fresh_tmp_dir("annotations");
  auto [db_owner, con] = sirius::make_test_db_and_connection();
  auto const path      = write_parquet(con,
                                  dir,
                                  "annotated_types",
                                  "CREATE TABLE annotated_types AS SELECT "
                                       "12.34::DECIMAL(12,2) AS amount, "
                                       "DATE '2024-01-02' AS day_value, "
                                       "TIMESTAMP '2024-01-02 03:04:05' AS ts_value");

  auto meta = read_metadata(path);
  auto info = sirius::io::parquet_helpers::extract_schema(meta);

  CHECK(info.names == std::vector<std::string>{"amount", "day_value", "ts_value"});
  REQUIRE(info.types.size() == 3);
  CHECK(info.types[0] == duckdb::LogicalType::DECIMAL(12, 2));
  CHECK(info.types[1] == duckdb::LogicalType::DATE);
  CHECK(info.types[2] == duckdb::LogicalType::TIMESTAMP);
}

TEST_CASE("parquet_helpers extract_schema maps top-level struct columns",
          "[parquet_helpers][schema][nested]")
{
  auto meta = read_metadata(parquet_fixture("nested_struct.parquet"));
  auto info = sirius::io::parquet_helpers::extract_schema(meta);

  REQUIRE(info.names == std::vector<std::string>{"id", "payload"});
  REQUIRE(info.types.size() == 2);
  CHECK(info.types[0] == duckdb::LogicalType::INTEGER);
  REQUIRE(info.types[1].id() == duckdb::LogicalTypeId::STRUCT);
  REQUIRE(duckdb::StructType::GetChildCount(info.types[1]) == 2);
  require_struct_child(info.types[1], 0, "a", duckdb::LogicalType::INTEGER);
  require_struct_child(info.types[1], 1, "b", duckdb::LogicalType::VARCHAR);
}

TEST_CASE("parquet_helpers extract_schema maps top-level list columns",
          "[parquet_helpers][schema][nested]")
{
  auto meta = read_metadata(parquet_fixture("nested_list.parquet"));
  auto info = sirius::io::parquet_helpers::extract_schema(meta);

  REQUIRE(info.names == std::vector<std::string>{"id", "items"});
  REQUIRE(info.types.size() == 2);
  CHECK(info.types[0] == duckdb::LogicalType::INTEGER);
  REQUIRE(info.types[1].id() == duckdb::LogicalTypeId::LIST);
  CHECK(duckdb::ListType::GetChildType(info.types[1]) == duckdb::LogicalType::BIGINT);
}

TEST_CASE("parquet_helpers extract_schema maps parquet map columns to DuckDB MAP",
          "[parquet_helpers][schema][nested]")
{
  auto meta = read_metadata(parquet_fixture("nested_map.parquet"));
  auto info = sirius::io::parquet_helpers::extract_schema(meta);

  REQUIRE(info.names == std::vector<std::string>{"id", "attrs"});
  REQUIRE(info.types.size() == 2);
  CHECK(info.types[0] == duckdb::LogicalType::INTEGER);
  REQUIRE(info.types[1].id() == duckdb::LogicalTypeId::MAP);
  CHECK(duckdb::MapType::KeyType(info.types[1]) == duckdb::LogicalType::VARCHAR);
  CHECK(duckdb::MapType::ValueType(info.types[1]) == duckdb::LogicalType::INTEGER);
}

TEST_CASE("parquet_helpers extract_schema maps deep nested columns and resumes at next scalar",
          "[parquet_helpers][schema][nested]")
{
  auto meta = read_metadata(parquet_fixture("nested_deep.parquet"));
  auto info = sirius::io::parquet_helpers::extract_schema(meta);

  REQUIRE(info.names == std::vector<std::string>{"id", "struct_of_list", "list_of_struct", "tail"});
  REQUIRE(info.types.size() == 4);
  CHECK(info.types[0] == duckdb::LogicalType::INTEGER);
  CHECK(info.types[3] == duckdb::LogicalType::INTEGER);

  REQUIRE(info.types[1].id() == duckdb::LogicalTypeId::STRUCT);
  REQUIRE(duckdb::StructType::GetChildCount(info.types[1]) == 1);
  CHECK(duckdb::StructType::GetChildName(info.types[1], 0) == "s");
  auto const& struct_list_child = duckdb::StructType::GetChildType(info.types[1], 0);
  REQUIRE(struct_list_child.id() == duckdb::LogicalTypeId::LIST);
  CHECK(duckdb::ListType::GetChildType(struct_list_child) == duckdb::LogicalType::INTEGER);

  REQUIRE(info.types[2].id() == duckdb::LogicalTypeId::LIST);
  auto const& list_struct_child = duckdb::ListType::GetChildType(info.types[2]);
  REQUIRE(list_struct_child.id() == duckdb::LogicalTypeId::STRUCT);
  REQUIRE(duckdb::StructType::GetChildCount(list_struct_child) == 1);
  require_struct_child(list_struct_child, 0, "x", duckdb::LogicalType::DOUBLE);
}

namespace {

/// One file's column chunks staged on the device, which is the shape the bulk
/// materialize route hands the hybrid scan readers.
struct staged_chunks {
  std::vector<rmm::device_buffer> buffers;
  std::vector<cudf::device_span<std::uint8_t const>> spans;
};

staged_chunks stage_column_chunks(cudf::io::datasource& source,
                                  std::span<cudf::io::text::byte_range_info const> ranges,
                                  ::cuda::stream_ref stream)
{
  staged_chunks staged;
  staged.buffers.reserve(ranges.size());
  for (auto const& range : ranges) {
    auto const host = source.host_read(static_cast<std::size_t>(range.offset()),
                                       static_cast<std::size_t>(range.size()));
    staged.buffers.emplace_back(host->data(), host->size(), stream);
  }
  staged.spans.reserve(staged.buffers.size());
  for (auto const& buffer : staged.buffers) {
    staged.spans.emplace_back(static_cast<std::uint8_t const*>(buffer.data()), buffer.size());
  }
  return staged;
}

/// `l_orderkey < 100000` over lineitem.parquet: 100382 of 600572 rows, spread
/// over all 5 row groups, so an equal count proves row-level filtering.
struct orderkey_filter {
  cudf::numeric_scalar<std::int64_t> limit{100000};
  cudf::ast::literal value{limit};
  cudf::ast::column_name_reference column{"l_orderkey"};
  cudf::ast::operation expression{cudf::ast::ast_operator::LESS, column, value};
};

cudf::io::parquet_reader_options filtered_options(std::vector<std::string> const& paths,
                                                  std::vector<std::string> columns,
                                                  cudf::ast::operation const& filter)
{
  auto options =
    cudf::io::parquet_reader_options::builder(cudf::io::source_info{paths}).filter(filter).build();
  options.set_column_names(std::move(columns));
  return options;
}

/// Rows the single-file bulk route produces for @p options.
cudf::size_type bulk_row_count(fs::path const& path,
                               cudf::io::parquet_reader_options const& options,
                               ::cuda::stream_ref stream)
{
  auto source = cudf::io::datasource::create(path.string());
  auto footer = read_parquet_footer(*source);
  cudf::io::parquet::experimental::hybrid_scan_reader reader{
    cudf::host_span<std::uint8_t const>(footer->data(), footer->size()), options};

  auto const row_groups = reader.all_row_groups(options);
  auto const ranges     = reader.all_column_chunks_byte_ranges(row_groups, options);
  auto const staged     = stage_column_chunks(*source, ranges, stream);

  return reader
    .materialize_all_columns(
      row_groups, staged.spans, options, stream, cudf::get_current_device_resource_ref())
    .tbl->num_rows();
}

/// An I/O object for metadata-only bulk-backend fixtures.
class bulk_object final : public sirius::io::io_object {
 public:
  explicit bulk_object(std::string path = "bulk", std::size_t size = 0)
    : _path(std::move(path)), _size(size)
  {
  }

  [[nodiscard]] std::string const& raw_file_cache_id() const noexcept override { return _path; }
  [[nodiscard]] std::string const& object_path() const noexcept override { return _path; }
  [[nodiscard]] std::size_t size() const noexcept override { return _size; }

 private:
  std::string _path;
  std::size_t _size;
};

class bulk_reactor {
 public:
  struct config {
    [[nodiscard]] std::size_t min_alignment_requirement() const noexcept { return 1; }
    [[nodiscard]] std::size_t merge_gap_size() const noexcept { return 0; }
    std::size_t n_max_concurrent_scans{0};
  };

  using io_object_type                  = bulk_object;
  using reactor_config_type             = config;
  static constexpr bool prefers_bulk_io = true;

  [[nodiscard]] config const& get_config() const noexcept { return _config; }
  void enqueue(std::unique_ptr<sirius::io::grouped_io_request>) noexcept {}
  [[nodiscard]] std::size_t queued_bytes() const noexcept { return 0; }
  [[nodiscard]] std::size_t staging_block_size() const noexcept { return 0; }
  std::size_t host_read(bulk_object const&, std::size_t, std::size_t size, std::uint8_t*) const
  {
    return size;
  }
  void start() {}
  void shutdown() {}
  void interrupt() {}
  [[nodiscard]] static std::unique_ptr<bulk_object> create_io_object(std::string)
  {
    return std::make_unique<bulk_object>();
  }
  [[nodiscard]] static bool supports(std::string_view) { return true; }
  [[nodiscard]] static std::vector<cudf::io::text::byte_range_info> align_and_coalesce(
    std::span<cudf::io::text::byte_range_info const> ranges, std::optional<std::size_t>)
  {
    return {ranges.begin(), ranges.end()};
  }

 private:
  config _config;
};

class bulk_context final : public sirius::io::templated_ioctx<bulk_reactor> {
 public:
  using templated_ioctx::templated_ioctx;

  [[nodiscard]] sirius::io::io_context_type type() const noexcept override
  {
    return sirius::io::io_context_type::restful;
  }
};

struct cached_file_context {
  cached_file_context() : memory{initialize_memory_manager(1)}
  {
    auto* host = sirius::scan_test_utils::get_space(*memory, cucascade::memory::Tier::HOST);
    REQUIRE(host != nullptr);
    auto reactor_context = std::make_shared<sirius::io::uring::uring_reactor::reactor_context>(
      sirius::io::uring::config{}, host->get_memory_resource_of<cucascade::memory::Tier::HOST>());
    context = std::make_shared<sirius::io::uring::uring_ioctx>(1, std::move(reactor_context));
    sirius::io::cache::config config;
    config.mode = sirius::io::cache::cache_mode::sirius;
    config.apply_mode();
    context->initialize_cache(*memory, config, sirius::test::controlled::single_gpu_topology());
    REQUIRE(context->uses_prefetching_cache());
    context->start();
  }

  ~cached_file_context()
  {
    context->shutdown_cache();
    context->shutdown();
  }

  std::unique_ptr<sirius::memory::sirius_memory_reservation_manager> memory;
  std::shared_ptr<sirius::io::uring::uring_ioctx> context;
};

std::vector<sirius::op::scan::parquet_source> cached_sources(
  cached_file_context& fixture,
  std::vector<std::string> const& paths,
  cudf::io::parquet_reader_options const& options)
{
  std::vector<sirius::op::scan::parquet_source> sources;
  for (auto const& path : paths) {
    auto metadata = std::make_shared<cudf::io::parquet::FileMetaData const>(read_metadata(path));
    cudf::io::parquet::experimental::hybrid_scan_reader reader{*metadata, options};
    auto groups     = reader.all_row_groups(options);
    auto ranges     = reader.all_column_chunks_byte_ranges(groups, options);
    auto datasource = fixture.context->open_datasource(path);
    datasource->fadvise(ranges, 0);
    REQUIRE(datasource->prepare_prefetch(false) == sirius::io::prepare_result::prepared);
    sources.push_back({std::move(datasource), std::move(metadata), std::move(groups)});
  }
  return sources;
}

void check_cpu_parity(duckdb::Connection& connection,
                      cudf::table_view table,
                      std::vector<std::string> const& names,
                      std::string const& expected_sql,
                      fs::path const& output,
                      ::cuda::stream_ref stream)
{
  cudf::io::table_input_metadata metadata{table};
  for (std::size_t i = 0; i < names.size(); ++i) {
    metadata.column_metadata[i].set_name(names[i]);
    if (names[i] == "amount") { metadata.column_metadata[i].set_decimal_precision(24); }
    if (names[i] == "nested") {
      metadata.column_metadata[i].child(0).set_name("a");
      metadata.column_metadata[i].child(1).set_name("b");
    }
  }
  auto writer =
    cudf::io::parquet_writer_options::builder(cudf::io::sink_info{output.string()}, table)
      .metadata(metadata)
      .utc_timestamps(false)
      .build();
  cudf::io::write_parquet(writer, stream);
  auto const actual_sql = "SELECT * FROM read_parquet('" + output.string() + "')";
  auto expected         = connection.Query(expected_sql);
  auto actual           = connection.Query(actual_sql);
  REQUIRE_FALSE(expected->HasError());
  REQUIRE_FALSE(actual->HasError());
  REQUIRE(expected->types.size() == actual->types.size());
  for (std::size_t i = 0; i < expected->types.size(); ++i) {
    INFO("column " << names[i] << ": expected " << expected->types[i].ToString() << ", exported "
                   << actual->types[i].ToString());
    CHECK(expected->types[i] == actual->types[i]);
  }
  auto differences =
    connection.Query("SELECT count(*) FROM ((" + expected_sql + " EXCEPT ALL " + actual_sql +
                     ") UNION ALL (" + actual_sql + " EXCEPT ALL " + expected_sql + ")) d");
  REQUIRE_FALSE(differences->HasError());
  CHECK(differences->GetValue(0, 0).GetValue<std::int64_t>() == 0);
}

sirius::op::scan::parquet_source schema_source(fs::path const& path,
                                               std::shared_ptr<bulk_context> const& ioctx)
{
  return {std::make_shared<sirius::io::sirius_datasource>(
            ioctx, std::make_shared<bulk_object>(path.string(), fs::file_size(path))),
          std::make_shared<cudf::io::parquet::FileMetaData const>(read_metadata(path)),
          {}};
}

}  // namespace

TEST_CASE("filtered options still take the bulk materialize route", "[scan][parquet][bulk_filter]")
{
  auto const path = parquet_fixture("lineitem.parquet");
  orderkey_filter const filter;
  auto const options =
    filtered_options({path.string()}, {"l_orderkey", "l_quantity"}, filter.expression);

  auto source = cudf::io::datasource::create(path.string());
  auto footer = read_parquet_footer(*source);
  cudf::io::parquet::experimental::hybrid_scan_reader reader{
    cudf::host_span<std::uint8_t const>(footer->data(), footer->size()), options};

  auto ioctx = std::make_shared<bulk_context>(std::vector<std::unique_ptr<bulk_reactor>>{});
  std::vector<sirius::op::scan::parquet_source> sources;
  sources.push_back(sirius::op::scan::parquet_source{
    std::make_shared<sirius::io::sirius_datasource>(ioctx, std::make_shared<bulk_object>()),
    std::make_shared<cudf::io::parquet::FileMetaData const>(reader.parquet_metadata()),
    reader.all_row_groups(options)});

  CHECK(sirius::op::scan::prefers_bulk_materialize(sources, options));
}

TEST_CASE("bulk materialize rejects sources whose schemas differ before reading",
          "[scan][parquet][bulk_schema]")
{
  auto const dir       = fresh_tmp_dir("bulk_schema");
  auto [db_owner, con] = sirius::make_test_db_and_connection();
  auto const path_a =
    write_parquet(con, dir, "drift_a", "CREATE TABLE drift_a AS SELECT 1::INTEGER AS x, 'a' AS y");
  auto const path_b =
    write_parquet(con, dir, "drift_b", "CREATE TABLE drift_b AS SELECT 2.5::DOUBLE AS x, 'b' AS y");
  auto const options = cudf::io::parquet_reader_options::builder().build();
  auto ioctx         = std::make_shared<bulk_context>(std::vector<std::unique_ptr<bulk_reactor>>{});
  std::vector<sirius::op::scan::parquet_source> sources;
  for (auto const& path : {path_a, path_b}) {
    auto metadata = std::make_shared<cudf::io::parquet::FileMetaData const>(read_metadata(path));
    cudf::io::parquet::experimental::hybrid_scan_reader reader{*metadata, options};
    sources.push_back(sirius::op::scan::parquet_source{
      std::make_shared<sirius::io::sirius_datasource>(
        ioctx, std::make_shared<bulk_object>(path.string(), fs::file_size(path))),
      std::move(metadata),
      reader.all_row_groups(options)});
  }

  REQUIRE(sirius::op::scan::prefers_bulk_materialize(sources, options));
  CHECK_THROWS_WITH(
    sirius::op::scan::materialize_parquet(
      sources, options, {}, cudf::get_default_stream(), cudf::get_current_device_resource_ref()),
    Catch::Matchers::ContainsSubstring("All sources must have the same schema") &&
      Catch::Matchers::ContainsSubstring("drift_a") &&
      Catch::Matchers::ContainsSubstring("drift_b") && Catch::Matchers::ContainsSubstring("'x'"));
}

TEST_CASE("bulk materialization accepts only the supported reader options",
          "[scan][parquet][bulk_route]")
{
  auto context = std::make_shared<bulk_context>(std::vector<std::unique_ptr<bulk_reactor>>{});
  std::vector<sirius::op::scan::parquet_source> sources{
    {std::make_shared<sirius::io::sirius_datasource>(context, std::make_shared<bulk_object>()),
     std::make_shared<cudf::io::parquet::FileMetaData>(),
     {0}}};
  cudf::io::parquet_reader_options options;
  REQUIRE(sirius::op::scan::prefers_bulk_materialize(sources, options));
  SECTION("row skip") { options.set_skip_rows(1); }
  SECTION("row limit") { options.set_num_rows(1); }
  SECTION("byte skip") { options.set_skip_bytes(1); }
  SECTION("byte limit") { options.set_num_bytes(1); }
  SECTION("options row groups") { options.set_row_groups({{0}}); }
  SECTION("column indices") { options.set_column_indices({0}); }
  SECTION("column field IDs") { options.set_column_field_ids({1}); }
  SECTION("custom column schema") { options.set_column_schema({}); }
  SECTION("timestamp cast")
  {
    options.set_timestamp_type(cudf::data_type{cudf::type_id::TIMESTAMP_MILLISECONDS});
  }
  SECTION("decimal cast") { options.set_decimal_width(cudf::type_id::DECIMAL128); }
  SECTION("categorical strings") { options.enable_convert_strings_to_categories(true); }
  SECTION("pandas metadata") { options.enable_use_pandas_metadata(false); }
  SECTION("Arrow metadata") { options.enable_use_arrow_schema(false); }
  SECTION("missing columns") { options.enable_ignore_missing_columns(false); }
  SECTION("case insensitive names") { options.enable_case_sensitive_names(false); }
  SECTION("JIT filter") { options.enable_use_jit_filter(true); }
  SECTION("mismatched schemas") { options.enable_allow_mismatched_pq_schemas(true); }
  SECTION("source indices") { options.enable_prepend_source_index_column(true); }
  SECTION("row indices") { options.enable_prepend_row_index_column(true); }
  CHECK_FALSE(sirius::op::scan::prefers_bulk_materialize(sources, options));
}

TEST_CASE("bulk materialization requires every source to qualify",
          "[scan][parquet][bulk_route][gpu_execution]")
{
  cached_file_context cached;
  auto uncached = std::make_shared<sirius::test::controlled::controlled_context>(
    std::vector<std::unique_ptr<sirius::test::controlled::controlled_reactor>>{});
  auto bulk = std::make_shared<bulk_context>(std::vector<std::unique_ptr<bulk_reactor>>{});
  cudf::io::parquet_reader_options options;
  auto metadata = std::make_shared<cudf::io::parquet::FileMetaData>();
  std::vector<sirius::op::scan::parquet_source> sources{
    {std::make_shared<sirius::io::sirius_datasource>(cached.context,
                                                     std::make_shared<bulk_object>()),
     metadata,
     {0}}};
  REQUIRE_FALSE(sources.front().datasource->prefers_bulk_io());
  REQUIRE(sirius::op::scan::prefers_bulk_materialize(sources, options));
  sources.push_back(
    {std::make_shared<sirius::io::sirius_datasource>(bulk, std::make_shared<bulk_object>()),
     metadata,
     {0}});
  CHECK(sirius::op::scan::prefers_bulk_materialize(sources, options));
  sources.push_back({uncached->open_datasource("controlled://not-bulk"), metadata, {0}});
  CHECK_FALSE(sirius::op::scan::prefers_bulk_materialize(sources, options));
  sources.pop_back();
  SECTION("missing datasource") { sources.front().datasource.reset(); }
  SECTION("missing metadata") { sources.front().metadata.reset(); }
  SECTION("no initial groups") { sources.front().row_group_indices.clear(); }
  SECTION("no sources") { sources.clear(); }
  CHECK_FALSE(sirius::op::scan::prefers_bulk_materialize(sources, options));
}

TEST_CASE("cached bulk materialization preserves flat and nested values on cold and retained reads",
          "[scan][parquet][bulk_parity][gpu_execution]")
{
  auto const compression = GENERATE("UNCOMPRESSED", "SNAPPY", "ZSTD");
  auto const file_count  = GENERATE(1, 2);
  CAPTURE(compression, file_count);
  auto const directory = fresh_tmp_dir("bulk_parity");
  duckdb::DuckDB database{nullptr};
  duckdb::Connection connection{database};
  require_ok(connection.Query(
    "CREATE TABLE input AS SELECT i::BIGINT id, "
    "CASE WHEN i%7=0 THEN NULL ELSE 'value_' || (i%23)::VARCHAR END payload, "
    "(i::DECIMAL(24,4)/16)::DECIMAL(24,4) amount, "
    "TIMESTAMP '2025-01-01' + i * INTERVAL 1 MICROSECOND ts, "
    "struct_pack(a := i::INTEGER, b := CASE WHEN i%5=0 THEN NULL ELSE 's' END) nested, "
    "[i::BIGINT, NULL, (i+1)::BIGINT] items FROM range(8192) t(i)"));
  std::vector<std::string> paths;
  std::string file_list = "[";
  for (int i = 0; i < file_count; ++i) {
    auto path = (directory / ("input" + std::to_string(i) + ".parquet")).string();
    require_ok(connection.Query(
      "COPY (SELECT * FROM input WHERE id >= " + std::to_string(i * 4096) + " AND id < " +
      std::to_string((i + 1) * 4096) + ") TO '" + path +
      "' (FORMAT PARQUET, ROW_GROUP_SIZE 2048, COMPRESSION '" + compression + "')"));
    if (i != 0) { file_list += ","; }
    file_list += "'" + path + "'";
    paths.push_back(std::move(path));
  }
  file_list += "]";
  cached_file_context fixture;
  rmm::cuda_stream stream;
  std::vector<std::string> names{"items", "payload", "nested", "amount", "ts", "id"};
  auto options = cudf::io::parquet_reader_options::builder(cudf::io::source_info{paths}).build();
  options.set_column_names(names);
  auto sources = cached_sources(fixture, paths, options);
  REQUIRE(sirius::op::scan::prefers_bulk_materialize(sources, options));
  auto expected = cudf::io::read_parquet(options, stream).tbl;
  for (int pass = 0; pass < 2; ++pass) {
    CAPTURE(pass);
    auto actual = sirius::op::scan::materialize_parquet(
      sources, options, {}, stream, cudf::get_current_device_resource_ref());
    REQUIRE(actual != nullptr);
    CHECK(sirius::test::expect_tables_equivalent_impl(actual->view(), expected->view(), stream));
    check_cpu_parity(
      connection,
      actual->view(),
      names,
      "SELECT items, payload, nested, amount, ts, id FROM read_parquet(" + file_list + ")",
      directory / "result.parquet",
      stream);
  }
}

TEST_CASE("final bulk filters ignore split ranges and prune files before uploading",
          "[scan][parquet][bulk_filter][bulk_parity][gpu_execution]")
{
  auto const all_pruned = GENERATE(false, true);
  auto const directory  = fresh_tmp_dir("bulk_final_filter");
  duckdb::DuckDB database{nullptr};
  duckdb::Connection connection{database};
  std::vector<std::string> paths;
  for (int i = 0; i < 2; ++i) {
    auto path = (directory / ("input" + std::to_string(i) + ".parquet")).string();
    require_ok(
      connection.Query("COPY (SELECT i::BIGINT id, 'row_' || i::VARCHAR payload "
                       "FROM range(" +
                       std::to_string(i * 8192) + "," + std::to_string((i + 1) * 8192) +
                       ") t(i)) TO '" + path + "' (FORMAT PARQUET, ROW_GROUP_SIZE 2048)"));
    paths.push_back(std::move(path));
  }
  cached_file_context fixture;
  rmm::cuda_stream stream;
  auto initial_options =
    cudf::io::parquet_reader_options::builder(cudf::io::source_info{paths}).build();
  initial_options.set_column_names({"payload"});
  auto sources = cached_sources(fixture, paths, initial_options);
  std::vector<std::vector<cudf::io::text::byte_range_info>> split_ranges;
  for (auto const& source : sources) {
    split_ranges.push_back(sirius::op::scan::column_chunk_ranges(
      *source.metadata, initial_options, source.row_group_indices));
  }
  cudf::numeric_scalar<std::int64_t> limit{all_pruned ? 20000 : 12287};
  cudf::ast::literal literal{limit};
  cudf::ast::column_name_reference column{"id"};
  cudf::ast::operation filter{cudf::ast::ast_operator::GREATER, column, literal};
  auto options = initial_options;
  options.set_filter(filter);
  auto expected = cudf::io::read_parquet(options, stream).tbl;
  for (int pass = 0; pass < 2; ++pass) {
    CAPTURE(all_pruned, pass);
    auto actual = sirius::op::scan::materialize_parquet(
      sources, options, split_ranges, stream, cudf::get_current_device_resource_ref());
    REQUIRE(actual != nullptr);
    CHECK(sirius::test::expect_tables_equivalent_impl(actual->view(), expected->view(), stream));
    CHECK(actual->num_rows() == (all_pruned ? 0 : 4096));
    check_cpu_parity(connection,
                     actual->view(),
                     {"payload"},
                     "SELECT payload FROM read_parquet(['" + paths[0] + "','" + paths[1] +
                       "']) WHERE id > " + std::to_string(all_pruned ? 20000 : 12287),
                     directory / "result.parquet",
                     stream);
  }
}

TEST_CASE("require_same_parquet_schema accepts a shared schema", "[scan][parquet][bulk_schema]")
{
  auto const dir       = fresh_tmp_dir("bulk_schema_shared");
  auto [db_owner, con] = sirius::make_test_db_and_connection();
  auto const path_a =
    write_parquet(con, dir, "shared_a", "CREATE TABLE shared_a AS SELECT 1::INTEGER AS x");
  auto const path_b =
    write_parquet(con, dir, "shared_b", "CREATE TABLE shared_b AS SELECT 2::INTEGER AS x");
  auto ioctx = std::make_shared<bulk_context>(std::vector<std::unique_ptr<bulk_reactor>>{});
  std::vector<sirius::op::scan::parquet_source> const sources{schema_source(path_a, ioctx),
                                                              schema_source(path_b, ioctx)};

  REQUIRE(sources[0].metadata != sources[1].metadata);
  CHECK_NOTHROW(sirius::op::scan::require_same_parquet_schema(sources));
  CHECK_NOTHROW(sirius::op::scan::require_same_parquet_schema(
    std::span<sirius::op::scan::parquet_source const>(sources).first(1)));
  CHECK_NOTHROW(sirius::op::scan::require_same_parquet_schema({}));
}

TEST_CASE("require_same_parquet_schema rejects column order and extra-column drift",
          "[scan][parquet][bulk_schema]")
{
  auto const dir       = fresh_tmp_dir("bulk_schema_columns");
  auto [db_owner, con] = sirius::make_test_db_and_connection();
  auto ioctx = std::make_shared<bulk_context>(std::vector<std::unique_ptr<bulk_reactor>>{});

  SECTION("column order")
  {
    auto const path_a = write_parquet(
      con, dir, "order_a", "CREATE TABLE order_a AS SELECT 1::INTEGER AS x, 2::INTEGER AS y");
    auto const path_b = write_parquet(
      con, dir, "order_b", "CREATE TABLE order_b AS SELECT 2::INTEGER AS y, 1::INTEGER AS x");
    std::vector<sirius::op::scan::parquet_source> const sources{schema_source(path_a, ioctx),
                                                                schema_source(path_b, ioctx)};

    CHECK_THROWS_WITH(
      sirius::op::scan::require_same_parquet_schema(sources),
      Catch::Matchers::ContainsSubstring("differ at schema element 1 ('x' INT32 vs 'y' INT32)"));
  }

  SECTION("extra column")
  {
    auto const path_a =
      write_parquet(con, dir, "extra_a", "CREATE TABLE extra_a AS SELECT 1::INTEGER AS x");
    auto const path_b = write_parquet(
      con, dir, "extra_b", "CREATE TABLE extra_b AS SELECT 1::INTEGER AS x, 2::INTEGER AS z");
    std::vector<sirius::op::scan::parquet_source> const sources{schema_source(path_a, ioctx),
                                                                schema_source(path_b, ioctx)};

    CHECK_THROWS_WITH(sirius::op::scan::require_same_parquet_schema(sources),
                      Catch::Matchers::ContainsSubstring("have 2 and 3 schema elements"));
  }
}

TEST_CASE("require_same_parquet_schema rejects a difference only in writer metadata",
          "[scan][parquet][bulk_schema]")
{
  auto const dir       = fresh_tmp_dir("bulk_schema_writer");
  auto [db_owner, con] = sirius::make_test_db_and_connection();
  auto const path =
    write_parquet(con, dir, "writer", "CREATE TABLE writer AS SELECT 1::INTEGER AS x");
  auto ioctx   = std::make_shared<bulk_context>(std::vector<std::unique_ptr<bulk_reactor>>{});
  auto source  = schema_source(path, ioctx);
  auto changed = std::make_shared<cudf::io::parquet::FileMetaData>(*source.metadata);
  std::vector<sirius::op::scan::parquet_source> const sources{source,
                                                              {source.datasource, changed, {}}};
  REQUIRE(changed->schema.size() == 2);
  CHECK_NOTHROW(sirius::op::scan::require_same_parquet_schema(sources));

  SECTION("root name")
  {
    REQUIRE(changed->schema[0].name == "duckdb_schema");
    changed->schema[0].name = "schema";
    CHECK_THROWS_WITH(sirius::op::scan::require_same_parquet_schema(sources),
                      Catch::Matchers::ContainsSubstring("differ at schema element 0") &&
                        Catch::Matchers::ContainsSubstring("'schema' group"));
  }

  SECTION("converted type")
  {
    REQUIRE(changed->schema[1].converted_type == cudf::io::parquet::ConvertedType::INT_32);
    changed->schema[1].converted_type = cudf::io::parquet::ConvertedType::UINT_32;
    CHECK_THROWS_WITH(
      sirius::op::scan::require_same_parquet_schema(sources),
      Catch::Matchers::ContainsSubstring("differ at schema element 1 ('x' INT32 vs 'x' INT32") &&
        Catch::Matchers::ContainsSubstring("; they differ in annotation, width or nesting"));
  }
}

TEST_CASE("hybrid scan bulk materialize applies the reader filter like read_parquet",
          "[scan][parquet][bulk_filter]")
{
  auto const path   = parquet_fixture("lineitem.parquet");
  auto const stream = cudf::get_default_stream();
  orderkey_filter const filter;

  SECTION("filter column inside the projection")
  {
    auto const options =
      filtered_options({path.string()}, {"l_orderkey", "l_quantity"}, filter.expression);
    CHECK(cudf::io::read_parquet(options).tbl->num_rows() == 100382);
    CHECK(bulk_row_count(path, options, stream) == 100382);
  }

  SECTION("filter column outside the projection")
  {
    auto const options =
      filtered_options({path.string()}, {"l_quantity", "l_shipdate"}, filter.expression);
    CHECK(cudf::io::read_parquet(options).tbl->num_rows() == 100382);
    CHECK(bulk_row_count(path, options, stream) == 100382);
  }

  SECTION("several sources through hybrid_scan_multifile")
  {
    auto const options = filtered_options(
      {path.string(), path.string()}, {"l_orderkey", "l_quantity"}, filter.expression);

    auto source = cudf::io::datasource::create(path.string());
    auto footer = read_parquet_footer(*source);
    std::vector<cudf::host_span<std::uint8_t const>> footers(
      2, cudf::host_span<std::uint8_t const>(footer->data(), footer->size()));
    cudf::io::parquet::experimental::hybrid_scan_multifile reader{footers, options};

    auto const row_groups     = reader.all_row_groups(options);
    auto const [ranges, srcs] = reader.all_column_chunks_byte_ranges(row_groups, options);
    // Both entries are the same file, so every range reads from the one source.
    auto const staged = stage_column_chunks(*source, ranges, stream);

    CHECK(cudf::io::read_parquet(options).tbl->num_rows() == 200764);
    CHECK(reader
            .materialize_all_columns(
              row_groups, staged.spans, options, stream, cudf::get_current_device_resource_ref())
            .tbl->num_rows() == 200764);
  }
}

TEST_CASE("column_chunk_range_cache matches column_chunk_ranges",
          "[scan][parquet][column_chunk_ranges]")
{
  auto const dir       = fresh_tmp_dir("chunk_range_cache");
  auto [db_owner, con] = sirius::make_test_db_and_connection();
  // Two files with different string lengths, so their column chunk offsets differ.
  std::vector<std::shared_ptr<cudf::io::parquet::FileMetaData const>> metadata;
  for (int const scale : {1, 1000}) {
    auto const table = "ranges_" + std::to_string(scale);
    require_ok(con.Query("CREATE TABLE " + table + " AS SELECT i AS x, i * 2 AS y, (i * " +
                         std::to_string(scale) + ")::VARCHAR AS z FROM range(10240) t(i)"));
    auto const path = dir / (table + ".parquet");
    require_ok(con.Query("COPY " + table + " TO '" + path.string() +
                         "' (FORMAT PARQUET, ROW_GROUP_SIZE 2048)"));
    metadata.push_back(
      std::make_shared<cudf::io::parquet::FileMetaData const>(read_metadata(path)));
    REQUIRE(metadata.back()->row_groups.size() == 5);
  }
  auto const all_columns = std::make_shared<cudf::io::parquet_reader_options const>(
    cudf::io::parquet_reader_options::builder().build());
  auto const two_columns = std::make_shared<cudf::io::parquet_reader_options const>(
    cudf::io::parquet_reader_options::builder().column_names({"z", "x"}).build());
  std::vector<std::vector<cudf::size_type>> const subsets{{0, 1}, {2}, {3, 4}, {}, {0, 2, 4}};

  sirius::op::scan::column_chunk_range_cache cache;
  auto const check = [&](auto const& file, auto const& options, auto const& row_groups) {
    auto const expected = sirius::op::scan::column_chunk_ranges(*file, *options, row_groups);
    auto const actual   = cache.ranges(file, options, row_groups);
    REQUIRE(actual.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
      CHECK(actual[i].offset() == expected[i].offset());
      CHECK(actual[i].size() == expected[i].size());
    }
  };
  // Options outermost, so the cache also sees a change of file alone.
  for (auto const& options : {all_columns, two_columns}) {
    for (auto const& file : metadata) {
      for (auto const& row_groups : subsets) {
        check(file, options, row_groups);
      }
    }
  }
  cache.clear();
  check(metadata.front(), all_columns, subsets.front());
}
