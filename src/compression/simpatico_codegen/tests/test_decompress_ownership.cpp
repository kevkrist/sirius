// SPDX-License-Identifier: Apache-2.0
#include "api/compressed_table_io.hpp"
#include "api/simpatico_codegen.hpp"
#include "codegen/plan/representation.hpp"
#include "test_utils.hpp"

#include <cudf/column/column_factories.hpp>
#include <cudf/strings/strings_column_view.hpp>
#include <cudf/table/table.hpp>
#include <cudf/utilities/default_stream.hpp>

#include <cuda_runtime.h>

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

template <typename Table>
concept rvalue_thread_decompressible =
  requires(Table&& table) { simpatico::decompress(std::move(table), 2); };

template <typename Table>
concept rvalue_pool_decompressible = requires(Table&& table, simpatico::stream_pool& pool) {
  simpatico::decompress(std::move(table), pool);
};

static_assert(!rvalue_thread_decompressible<simpatico::compressed_table>);
static_assert(!rvalue_pool_decompressible<simpatico::compressed_table>);

void expect_same_table(cudf::table_view expected,
                       cudf::table const& actual,
                       rmm::cuda_stream_view stream,
                       std::string const& label)
{
  expect(actual.num_columns() == expected.num_columns(),
         (label + ": decompressed column count").c_str());
  for (cudf::size_type i = 0; i < expected.num_columns(); ++i) {
    expect(columns_equal_any(expected.column(i), actual.view().column(i), stream),
           (label + ": column " + std::to_string(i) + " differs").c_str());
  }
}

void exercise_const_entry_points(simpatico::compressed_table const& compressed,
                                 cudf::table_view expected,
                                 rmm::cuda_stream_view stream,
                                 rmm::device_async_resource_ref mr,
                                 std::string const& label)
{
  auto first  = simpatico::decompress(compressed, stream, mr);
  auto second = simpatico::decompress(compressed, stream, mr);
  expect_same_table(expected, *first, stream, label + " single #1");
  expect_same_table(expected, *second, stream, label + " single #2");

  simpatico::stream_pool pool;
  expect(pool.init(2), (label + ": stream pool init failed").c_str());
  {
    auto parallel_first  = simpatico::decompress(compressed, pool, mr);
    auto parallel_second = simpatico::decompress(compressed, pool, mr);
    expect_same_table(expected, *parallel_first, stream, label + " parallel #1");
    expect_same_table(expected, *parallel_second, stream, label + " parallel #2");
  }
}

simpatico::compressed_table pin_roundtrip(std::string const& label,
                                          cudf::table_view input,
                                          std::string const& dsl,
                                          rmm::cuda_stream_view stream,
                                          rmm::device_async_resource_ref mr)
{
  auto compressed = simpatico::compress_with_plan(input, dsl, stream, mr);

  std::vector<std::uint8_t> header;
  std::vector<simpatico::payload_buffer_ref> buffers;
  std::uint64_t payload_bytes = 0;
  auto error =
    simpatico::build_compressed_table_header(compressed, header, buffers, payload_bytes, stream);
  expect(error.empty(), (label + " header: " + error).c_str());

  std::vector<std::uint8_t> payload(static_cast<std::size_t>(payload_bytes));
  stream.synchronize();
  for (auto const& buffer : buffers) {
    if (buffer.size_bytes == 0) continue;
    expect(buffer.device_ptr != nullptr, "pin roundtrip payload has a null source");
    expect(cudaMemcpy(payload.data() + buffer.offset,
                      buffer.device_ptr,
                      static_cast<std::size_t>(buffer.size_bytes),
                      cudaMemcpyDeviceToHost) == cudaSuccess,
           "pin roundtrip device-to-host copy failed");
  }

  simpatico::payload_fetch_fn fetch = [&payload](std::uint64_t offset,
                                                 std::size_t size,
                                                 void* destination,
                                                 rmm::cuda_stream_view fetch_stream) {
    if (size == 0) return;
    if (cudaMemcpyAsync(destination,
                        payload.data() + offset,
                        size,
                        cudaMemcpyHostToDevice,
                        fetch_stream.value()) != cudaSuccess) {
      throw std::runtime_error("pin roundtrip host-to-device copy failed");
    }
  };

  std::string read_error;
  auto restored =
    simpatico::read_compressed_table_from_memory(header, fetch, stream, mr, &read_error);
  expect(read_error.empty(), (label + " read: " + read_error).c_str());
  stream.synchronize();
  return restored;
}

cudf::column_view channel(simpatico::str_split_compressed_representation const& rep,
                          std::string const& name,
                          rmm::cuda_stream_view stream)
{
  for (auto const& output : rep.named_channels(stream)) {
    if (output.name == name) return output.view;
  }
  throw std::runtime_error("missing str_split channel '" + name + "'");
}

cudf::column_view with_unknown_null_count(cudf::column_view source)
{
  std::vector<cudf::column_view> children;
  children.reserve(source.num_children());
  for (cudf::size_type i = 0; i < source.num_children(); ++i) {
    children.push_back(source.child(i));
  }
  return cudf::column_view{source.type(),
                           source.size(),
                           source.head<void>(),
                           source.null_mask(),
                           static_cast<cudf::size_type>(-1),
                           source.offset(),
                           children};
}

std::unique_ptr<cudf::column> make_offsets(std::int32_t last,
                                           rmm::cuda_stream_view stream,
                                           rmm::device_async_resource_ref mr)
{
  auto offsets = cudf::make_fixed_width_column(
    cudf::data_type{cudf::type_id::INT32}, 2, cudf::mask_state::UNALLOCATED, stream, mr);
  std::int32_t host[]{0, last};
  expect(cudaMemcpyAsync(offsets->mutable_view().head<void>(),
                         host,
                         sizeof(host),
                         cudaMemcpyHostToDevice,
                         stream.value()) == cudaSuccess,
         "offset upload failed");
  stream.synchronize();
  return offsets;
}

std::unique_ptr<cudf::column> make_channel(cudf::data_type type,
                                           cudf::size_type size,
                                           rmm::cuda_stream_view stream,
                                           rmm::device_async_resource_ref mr)
{
  auto result =
    cudf::make_fixed_width_column(type, size, cudf::mask_state::UNALLOCATED, stream, mr);
  auto const bytes = static_cast<std::size_t>(size) * static_cast<std::size_t>(cudf::size_of(type));
  if (bytes != 0) {
    expect(
      cudaMemsetAsync(result->mutable_view().head<void>(), 0, bytes, stream.value()) == cudaSuccess,
      "channel initialization failed");
  }
  stream.synchronize();
  return result;
}

void test_const_repeatability(rmm::cuda_stream_view stream, rmm::device_async_resource_ref mr)
{
  {
    auto input = make_int32_table(1, 4096, 17);
    auto compressed =
      simpatico::compress_with_plan(input->view(), "input -> identity\n", stream, mr);
    exercise_const_entry_points(compressed, input->view(), stream, mr, "identity");
  }

  std::vector<std::string> values{"alpha", "", "charlie", "delta", "echo"};
  std::vector<bool> valid{true, false, true, true, false};
  {
    auto input = make_strings_table(values, valid, stream);
    auto compressed =
      simpatico::compress_with_plan(input->view(), "input -> str_split\n", stream, mr);
    exercise_const_entry_points(compressed, input->view(), stream, mr, "bare str_split");
  }

  {
    auto input = make_strings_table(values, valid, stream);
    std::string const dsl =
      "input -> str_split -> offsets, chars, null_mask\n"
      "str_split.offsets -> identity\n"
      "str_split.chars -> identity\n"
      "str_split.null_mask -> identity\n";
    auto compressed = simpatico::compress_with_plan(input->view(), dsl, stream, mr);
    exercise_const_entry_points(compressed, input->view(), stream, mr, "routed str_split");
  }

  {
    auto input = make_strings_table(
      {"red", "blue", "red", "green", "blue"}, {true, true, false, true, true}, stream);
    auto compressed =
      simpatico::compress_with_plan(input->view(), "input -> dictionary\n", stream, mr);
    exercise_const_entry_points(compressed, input->view(), stream, mr, "dictionary");
  }
}

void test_pinned_const_codegen_and_bitjoin(rmm::cuda_stream_view stream,
                                           rmm::device_async_resource_ref mr)
{
  {
    auto input = make_int32_table(1, 8192, 31);
    std::string const dsl =
      "input -> delta -> differences\n"
      "delta.differences -> rle -> values, runs\n"
      "delta.differences.values -> bitpack\n"
      "delta.differences.runs -> bitpack\n";
    auto compressed = pin_roundtrip("pinned codegen", input->view(), dsl, stream, mr);
    exercise_const_entry_points(compressed, input->view(), stream, mr, "pinned codegen");
  }

  {
    auto input = make_f32_table(1, 4096, 37);
    std::string const dsl =
      "input -> bitextract_f32 -> sign, exponent, mantissa\n"
      "bitextract_f32.sign, bitextract_f32.exponent, bitextract_f32.mantissa "
      "-> bitjoin_f32 -> rejoined\n";
    auto compressed = pin_roundtrip("pinned bitjoin", input->view(), dsl, stream, mr);
    exercise_const_entry_points(compressed, input->view(), stream, mr, "pinned bitjoin");
  }
}

void test_identity_take(rmm::cuda_stream_view stream, rmm::device_async_resource_ref mr)
{
  auto input      = make_int32_table(1, 4096, 41);
  auto compressed = simpatico::compress_with_plan(input->view(), "input -> identity\n", stream, mr);
  auto* rep =
    find_rep<simpatico::identity_compressed_representation>(*compressed.columns[0].compound);
  expect(rep != nullptr, "identity representation not found");

  auto channels = rep->named_channels(stream);
  expect(channels.size() == 1, "identity representation has unexpected channels");
  void const* stored_data = channels[0].view.head<void>();

  auto decoded = std::move(compressed).decompress(stream, mr);
  expect(decoded->view().column(0).head<void>() == stored_data,
         "rvalue identity decode copied its data buffer");
  expect_same_table(input->view(), *decoded, stream, "identity take");

  expect_consumed_error([&] { (void)compressed.describe(stream); },
                        "identity describe after take was not loud");
  expect_consumed_error([&] { (void)rep->decompress(stream, mr); },
                        "identity const decode after take was not loud");
  expect_consumed_error([&] { (void)rep->named_channels(stream); },
                        "identity channels after take were not loud");
  expect_consumed_error([&] { (void)rep->required_channels(); },
                        "identity requirements after take were not loud");
  expect_consumed_error([&] { (void)rep->describe_meta(); },
                        "identity metadata after take was not loud");

  expect_consumed_error(
    [&] {
      (void)simpatico::decompress(
        static_cast<simpatico::compressed_table const&>(compressed), 2, mr);
    },
    "parallel decode did not surface a consumed representation exception");
}

void test_str_split_take(rmm::cuda_stream_view stream, rmm::device_async_resource_ref mr)
{
  auto input = make_strings_table(
    {"zero", "one", "", "three", "four"}, {true, false, true, true, false}, stream);
  auto compressed =
    simpatico::compress_with_plan(input->view(), "input -> str_split\n", stream, mr);
  auto* rep =
    find_rep<simpatico::str_split_compressed_representation>(*compressed.columns[0].compound);
  expect(rep != nullptr, "str_split representation not found");

  void const* stored_offsets = channel(*rep, "offsets", stream).head<void>();
  void const* stored_chars   = channel(*rep, "chars", stream).head<void>();
  void const* stored_mask    = channel(*rep, "null_mask", stream).head<void>();

  auto decoded        = simpatico::decompress(std::move(compressed), stream, mr);
  auto decoded_column = decoded->view().column(0);
  cudf::strings_column_view strings(decoded_column);
  expect(strings.offsets().head<void>() == stored_offsets,
         "rvalue str_split decode copied offsets");
  expect(strings.chars_begin(stream) == stored_chars, "rvalue str_split decode copied chars");
  expect(decoded_column.null_mask() == stored_mask, "rvalue str_split decode copied its mask");
  expect_same_table(input->view(), *decoded, stream, "str_split take");

  expect_consumed_error([&] { (void)compressed.describe(stream); },
                        "str_split describe after take was not loud");
  expect_consumed_error([&] { (void)rep->decompress(stream, mr); },
                        "str_split const decode after take was not loud");
  expect_consumed_error([&] { (void)rep->named_channels(stream); },
                        "str_split channels after take were not loud");
  expect_consumed_error([&] { (void)rep->required_channels(); },
                        "str_split requirements after take were not loud");
  expect_consumed_error([&] { (void)rep->describe_meta(); },
                        "str_split metadata after take was not loud");
}

void test_copying_rep_survives_rvalue(rmm::cuda_stream_view stream,
                                      rmm::device_async_resource_ref mr)
{
  auto input = make_strings_table(
    {"red", "blue", "red", "green", "blue"}, {true, true, false, true, true}, stream);
  auto compressed =
    simpatico::compress_with_plan(input->view(), "input -> dictionary\n", stream, mr);
  auto* rep =
    find_rep<simpatico::dictionary_compressed_representation>(*compressed.columns[0].compound);
  expect(rep != nullptr, "dictionary representation not found");

  auto decoded = simpatico::decompress(std::move(compressed), stream, mr);
  expect_same_table(input->view(), *decoded, stream, "dictionary rvalue");

  expect(!rep->named_channels(stream).empty(), "dictionary was poisoned by rvalue decode");
  auto direct = rep->decompress(stream, mr);
  expect(columns_equal_any(input->view().column(0), direct->view(), stream),
         "dictionary direct reuse differs");
  expect(!compressed.describe(stream)[0].empty(), "dictionary describe after rvalue is empty");
  auto repeated =
    simpatico::decompress(static_cast<simpatico::compressed_table const&>(compressed), stream, mr);
  expect_same_table(input->view(), *repeated, stream, "dictionary reuse");
}

void test_validation_precedes_consumption(rmm::cuda_stream_view stream,
                                          rmm::device_async_resource_ref mr)
{
  auto offsets = make_offsets(1, stream, mr);
  auto chars   = make_channel(cudf::data_type{cudf::type_id::UINT8}, 1, stream, mr);
  simpatico::str_split_compressed_representation rep(
    1, std::move(offsets), std::move(chars), nullptr, 0);

  auto const correct_size = rep.chars_size_;
  rep.chars_size_ += 1;
  bool validation_failed = false;
  try {
    (void)rep.take_decompressed(stream, mr);
  } catch (std::logic_error const& error) {
    validation_failed =
      std::string(error.what()).find("invalid chars storage") != std::string::npos;
  }
  expect(validation_failed, "malformed str_split storage was not rejected before take");

  rep.chars_size_ = correct_size;
  expect(rep.named_channels(stream).size() == 2,
         "pre-move validation failure consumed the str_split rep");
  auto decoded = rep.take_decompressed(stream, mr);
  expect(decoded != nullptr, "validated str_split rep could not be consumed afterward");
}

void verify_widened_chars_descriptor(cudf::type_id chars_id,
                                     rmm::cuda_stream_view stream,
                                     rmm::device_async_resource_ref mr)
{
  constexpr cudf::size_type chars_elements = 3;
  auto offsets =
    make_offsets(chars_elements * cudf::size_of(cudf::data_type{chars_id}), stream, mr);
  auto chars = make_channel(cudf::data_type{chars_id}, chars_elements, stream, mr);
  auto rep   = std::make_unique<simpatico::str_split_compressed_representation>(
    1, std::move(offsets), std::move(chars), nullptr, 0);

  auto named = rep->named_channels(stream);
  expect(named.size() == 2, "synthetic widened str_split has unexpected channel count");
  expect(named[1].view.type().id() == chars_id, "widened chars view lost its dtype");
  expect(named[1].view.size() == chars_elements, "widened chars view lost its element count");

  auto tree = std::make_unique<simpatico::PlanTree>();
  tree->nodes.resize(1);
  tree->nodes[0].op  = "input";
  tree->nodes[0].rep = std::move(rep);

  simpatico::compressed_column column;
  column.dtype    = cudf::data_type{cudf::type_id::STRING};
  column.num_rows = 1;
  column.compound = std::move(tree);
  simpatico::compressed_table table;
  table.columns.push_back(std::move(column));

  auto descriptors = table.describe(stream);
  expect(descriptors.size() == 1 && descriptors[0].size() == 1,
         "widened str_split descriptor shape differs");
  auto const& buffers = descriptors[0][0].buffers;
  auto chars_desc     = std::find_if(
    buffers.begin(), buffers.end(), [](auto const& buffer) { return buffer.name == "chars"; });
  expect(chars_desc != buffers.end(), "widened chars descriptor missing");
  expect(chars_desc->type_tag == simpatico::dtype_to_tag(cudf::data_type{chars_id}),
         "widened chars descriptor lost its type tag");
  expect(chars_desc->num_rows == chars_elements, "widened chars descriptor lost its element count");
  expect(chars_desc->size_bytes ==
           static_cast<std::uint64_t>(chars_elements) *
             static_cast<std::uint64_t>(cudf::size_of(cudf::data_type{chars_id})),
         "widened chars descriptor has the wrong padded byte count");
}

void test_widened_descriptors(rmm::cuda_stream_view stream, rmm::device_async_resource_ref mr)
{
  verify_widened_chars_descriptor(cudf::type_id::UINT32, stream, mr);
  verify_widened_chars_descriptor(cudf::type_id::UINT64, stream, mr);
}

void test_unknown_source_null_counts(rmm::cuda_stream_view stream,
                                     rmm::device_async_resource_ref mr)
{
  auto check = [&](std::vector<bool> const& validity, bool expect_mask) {
    auto input   = make_strings_table({"alpha", "", "charlie", "delta"}, validity, stream);
    auto unknown = with_unknown_null_count(input->view().column(0));

    simpatico::str_split_compressor compressor;
    auto representation = compressor.compress(unknown, stream, mr);
    auto* rep = dynamic_cast<simpatico::str_split_compressed_representation*>(representation.get());
    expect(rep != nullptr, "unknown-count str_split returned the wrong representation");
    expect(rep->has_null_mask_ == expect_mask, "unknown-count mask presence differs");
    expect(rep->null_count_ == (expect_mask ? -1 : 0), "unknown-count cache differs");

    auto decoded = rep->decompress(stream, mr);
    expect(columns_equal_any(input->view().column(0), decoded->view(), stream),
           "unknown-count str_split decode differs");
  };

  check({true, false, true, false}, true);
  check({}, false);
}

void test_unknown_null_count_io(rmm::cuda_stream_view stream, rmm::device_async_resource_ref mr)
{
  auto input =
    make_strings_table({"alpha", "", "charlie", "delta"}, {true, false, true, false}, stream);
  std::string const dsl =
    "input -> str_split -> offsets, chars, null_mask\n"
    "str_split.offsets -> identity\n"
    "str_split.chars -> identity\n"
    "str_split.null_mask -> identity\n";
  auto restored = pin_roundtrip("v10 nullable str_split", input->view(), dsl, stream, mr);

  // v10 persists only the routed leaves, not a str_split null-count field. Each decode rebuilds
  // a transient str_split rep with null_count_ == -1 and must resolve it from the stored mask.
  exercise_const_entry_points(restored, input->view(), stream, mr, "v10 nullable str_split");
}

}  // namespace

int main()
{
  if (cudaSetDevice(0) != cudaSuccess) {
    std::fprintf(stderr, "test_decompress_ownership: cudaSetDevice failed\n");
    return 1;
  }
  try {
    auto stream = cudf::get_default_stream();
    auto mr     = rmm::mr::get_current_device_resource_ref();

    test_const_repeatability(stream, mr);
    test_pinned_const_codegen_and_bitjoin(stream, mr);
    test_identity_take(stream, mr);
    test_str_split_take(stream, mr);
    test_copying_rep_survives_rvalue(stream, mr);
    test_validation_precedes_consumption(stream, mr);
    test_widened_descriptors(stream, mr);
    test_unknown_source_null_counts(stream, mr);
    test_unknown_null_count_io(stream, mr);

    std::printf("test_decompress_ownership: PASS\n");
    return 0;
  } catch (std::exception const& error) {
    std::fprintf(stderr, "test_decompress_ownership: FAIL: %s\n", error.what());
    return 1;
  }
}
