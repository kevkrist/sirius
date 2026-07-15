// SPDX-License-Identifier: Apache-2.0
//
// Tests for write_compressed_table / read_compressed_table (.hpln v10).
//
// Each test_* function throws std::runtime_error on failure; main() catches and
// reports.  Tests are intentionally independent so a failure in one does not
// mask others.

#include "api/compressed_table_io.hpp"
#include "api/simpatico_codegen.hpp"
#include "codegen/jit/nvrtc_compiler.hpp"
#include "codegen/plan/leaf_desc.hpp"
#include "test_utils.hpp"

#include <cudf/table/table.hpp>
#include <cudf/types.hpp>
#include <cudf/utilities/default_stream.hpp>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// RAII temp file: created on construction, deleted on destruction.
struct TmpFile {
  std::string path;
  TmpFile()
  {
    char buf[] = "/tmp/simpatico_io_test_XXXXXX";
    int fd     = mkstemp(buf);
    if (fd != -1) ::close(fd);
    path = std::string(buf) + ".hpln";
  }
  ~TmpFile() { std::remove(path.c_str()); }
};
template <typename T>
std::size_t append_le(std::vector<std::uint8_t>& bytes, T value)
{
  auto const offset  = bytes.size();
  auto const encoded = std::bit_cast<std::array<std::uint8_t, sizeof(T)>>(value);
  bytes.insert(bytes.end(), encoded.begin(), encoded.end());
  return offset;
}

void append_str16(std::vector<std::uint8_t>& bytes, std::string_view value)
{
  append_le(bytes, static_cast<std::uint16_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
}

template <typename T>
void overwrite_le(std::vector<std::uint8_t>& bytes, std::size_t offset, T value)
{
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
    throw std::logic_error("malformed-header fixture overwrite is out of range");
  }
  auto const encoded = std::bit_cast<std::array<std::uint8_t, sizeof(T)>>(value);
  std::copy(encoded.begin(), encoded.end(), bytes.begin() + offset);
}

struct IdentityHeaderFixture {
  std::vector<std::uint8_t> bytes;
  std::size_t column_type{};
  std::size_t column_rows{};
  std::size_t root_child{};
  std::size_t num_leaves{};
  std::size_t leaf_start{};
  std::size_t leaf_kind{};
  std::size_t leaf_type{};
  std::size_t leaf_rows{};
  std::size_t buffer_type{};
  std::size_t buffer_size{};
  std::size_t buffer_offset{};
};

IdentityHeaderFixture make_identity_header_fixture()
{
  IdentityHeaderFixture fixture;
  auto& bytes = fixture.bytes;
  bytes.insert(bytes.end(), {'H', 'P', 'L', 'N'});
  append_le(bytes, std::uint8_t{10});
  append_le(bytes, std::uint16_t{1});

  append_str16(bytes, "");
  fixture.column_type = append_le(bytes, std::uint8_t{2});
  append_le(bytes, std::int32_t{0});
  fixture.column_rows = append_le(bytes, std::int64_t{4});

  append_le(bytes, std::uint16_t{2});
  append_str16(bytes, "input");
  append_le(bytes, std::uint8_t{0});
  append_le(bytes, std::uint16_t{1});
  append_str16(bytes, "identity");
  fixture.root_child = append_le(bytes, std::uint32_t{1});
  append_le(bytes, std::uint16_t{0});

  append_str16(bytes, "identity");
  append_le(bytes, std::uint8_t{0});
  append_le(bytes, std::uint16_t{0});
  append_le(bytes, std::uint16_t{0});

  fixture.num_leaves = append_le(bytes, std::uint16_t{1});
  fixture.leaf_start = bytes.size();
  append_le(bytes, std::uint32_t{1});
  append_le(bytes, std::int32_t{-1});
  fixture.leaf_kind = append_le(bytes, std::uint8_t{5});
  fixture.leaf_type = append_le(bytes, std::uint8_t{2});
  fixture.leaf_rows = append_le(bytes, std::uint64_t{4});
  append_le(bytes, std::uint8_t{0});
  append_le(bytes, std::uint8_t{1});

  append_str16(bytes, "data");
  fixture.buffer_type   = append_le(bytes, std::uint8_t{2});
  fixture.buffer_size   = append_le(bytes, std::uint64_t{16});
  fixture.buffer_offset = append_le(bytes, std::uint64_t{0});
  return fixture;
}

struct BitjoinHeaderFixture {
  std::vector<std::uint8_t> bytes;
  std::size_t output_type{};
  std::size_t input_node{};
};

BitjoinHeaderFixture make_bitjoin_header_fixture()
{
  BitjoinHeaderFixture fixture;
  auto& bytes = fixture.bytes;
  bytes.insert(bytes.end(), {'H', 'P', 'L', 'N'});
  append_le(bytes, std::uint8_t{10});
  append_le(bytes, std::uint16_t{1});

  append_str16(bytes, "");
  append_le(bytes, std::uint8_t{2});
  append_le(bytes, std::int32_t{0});
  append_le(bytes, std::int64_t{0});

  append_le(bytes, std::uint16_t{3});
  append_str16(bytes, "input");
  append_le(bytes, std::uint8_t{0});
  append_le(bytes, std::uint16_t{1});
  append_str16(bytes, "identity");
  append_le(bytes, std::uint32_t{1});
  append_le(bytes, std::uint16_t{0});

  append_str16(bytes, "identity");
  append_le(bytes, std::uint8_t{0});
  append_le(bytes, std::uint16_t{1});
  append_str16(bytes, "value");
  append_le(bytes, std::uint32_t{2});
  append_le(bytes, std::uint16_t{1});
  append_str16(bytes, "value");

  append_str16(bytes, "bitjoin_i32");
  append_le(bytes, std::uint8_t{1});
  fixture.output_type = append_le(bytes, std::uint8_t{2});
  append_le(bytes, std::uint16_t{1});
  fixture.input_node = append_le(bytes, std::uint32_t{1});
  append_str16(bytes, "value");
  append_le(bytes, std::uint8_t{0});
  append_le(bytes, std::uint16_t{0});
  append_le(bytes, std::uint16_t{0});

  append_le(bytes, std::uint16_t{0});
  return fixture;
}

void expect_memory_header_rejected(std::vector<std::uint8_t> const& header,
                                   std::string_view expected_error,
                                   std::string_view label)
{
  std::size_t fetch_calls{0};
  simpatico::payload_fetch_fn fetch =
    [&fetch_calls](std::uint64_t, std::size_t, void*, rmm::cuda_stream_view) { ++fetch_calls; };

  std::string error;
  auto restored = simpatico::read_compressed_table_from_memory(
    header, fetch, cudf::get_default_stream(), rmm::mr::get_current_device_resource_ref(), &error);
  expect(restored.columns.empty(), (std::string(label) + ": expected an empty table").c_str());
  expect(fetch_calls == 0,
         (std::string(label) + ": payload was fetched before header rejection").c_str());
  expect(error.find(expected_error) != std::string::npos,
         (std::string(label) + ": unexpected error: " + error).c_str());
}

// Flatten leaf kinds from describe() into a simple vector for comparison.
std::vector<simpatico::OpId> leaf_kinds(simpatico::compressed_table const& ct)
{
  std::vector<simpatico::OpId> out;
  for (auto const& descs : ct.describe())
    for (auto const& ld : descs)
      out.push_back(ld.kind);
  return out;
}

// Flatten buffer names from describe() into a simple vector for comparison.
std::vector<std::string> buf_names(simpatico::compressed_table const& ct)
{
  std::vector<std::string> out;
  for (auto const& descs : ct.describe())
    for (auto const& ld : descs)
      for (auto const& bd : ld.buffers)
        out.push_back(bd.name);
  return out;
}

// ---------------------------------------------------------------------------
// Core roundtrip helper
// ---------------------------------------------------------------------------

void io_roundtrip(char const* label,
                  cudf::table_view input,
                  std::string const& dsl,
                  std::vector<std::string> column_names = {})
{
  auto stream = cudf::get_default_stream();

  // Compress.
  simpatico::compressed_table ct1 = simpatico::compress_with_plan(
    input, dsl, stream, rmm::mr::get_current_device_resource_ref(), column_names);
  expect(ct1.columns.size() == static_cast<std::size_t>(input.num_columns()),
         (std::string(label) + ": compress column count").c_str());

  // Write.
  TmpFile tmp;
  std::string werr = simpatico::write_compressed_table(ct1, tmp.path);
  expect(werr.empty(), (std::string(label) + ": write error: " + werr).c_str());

  // Read.
  std::string rerr;
  simpatico::compressed_table ct2 = simpatico::read_compressed_table(
    tmp.path, stream, rmm::mr::get_current_device_resource_ref(), &rerr);
  expect(rerr.empty(), (std::string(label) + ": read error: " + rerr).c_str());
  expect(ct2.columns.size() == ct1.columns.size(),
         (std::string(label) + ": read column count").c_str());

  // Metadata survives.
  for (std::size_t i = 0; i < ct1.columns.size(); ++i) {
    auto const& c1 = ct1.columns[i];
    auto const& c2 = ct2.columns[i];
    expect(c1.dtype == c2.dtype, (std::string(label) + ": dtype col " + std::to_string(i)).c_str());
    expect(c1.num_rows == c2.num_rows,
           (std::string(label) + ": num_rows col " + std::to_string(i)).c_str());
    expect(c1.name == c2.name, (std::string(label) + ": name col " + std::to_string(i)).c_str());
    // The plan is persisted structurally (node array), not as DSL text, so the
    // compound's presence round-trips even though plan_dsl is not restored.
    expect((c1.compound != nullptr) == (c2.compound != nullptr),
           (std::string(label) + ": compound presence col " + std::to_string(i)).c_str());
  }

  // Leaf structure survives (same kinds and buffer names).
  expect(leaf_kinds(ct1) == leaf_kinds(ct2),
         (std::string(label) + ": leaf kinds mismatch").c_str());
  expect(buf_names(ct1) == buf_names(ct2),
         (std::string(label) + ": buffer names mismatch").c_str());

  // Decompress and compare pixel-exact to original.
  auto out = simpatico::decompress(ct2, stream, rmm::mr::get_current_device_resource_ref());
  expect(out != nullptr, (std::string(label) + ": decompress returned null").c_str());
  expect(out->num_columns() == input.num_columns(),
         (std::string(label) + ": decompressed column count").c_str());
  for (int i = 0; i < input.num_columns(); ++i)
    expect(columns_equal_any(input.column(i), out->view().column(i), stream),
           (std::string(label) + ": data mismatch col " + std::to_string(i)).c_str());
}

// In-memory (pinned-blob) roundtrip via the production pin-path entry points:
// build_compressed_table_header enumerates payload buffers, we assemble the
// payload host-side, then read_compressed_table_from_memory reconstructs
// through the same fetch seam pin_table uses.
void memory_roundtrip(char const* label, cudf::table_view input, std::string const& dsl)
{
  auto stream = cudf::get_default_stream();
  auto mr     = rmm::mr::get_current_device_resource_ref();

  simpatico::compressed_table ct = simpatico::compress_with_plan(input, dsl, stream, mr);

  std::vector<std::uint8_t> header;
  std::vector<simpatico::payload_buffer_ref> buffers;
  std::uint64_t payload_bytes = 0;
  auto const herr =
    simpatico::build_compressed_table_header(ct, header, buffers, payload_bytes, stream);
  expect(herr.empty(), (std::string(label) + ": header error: " + herr).c_str());

  std::vector<std::uint8_t> payload(payload_bytes);
  stream.synchronize();
  for (auto const& b : buffers) {
    if (b.size_bytes == 0 || b.device_ptr == nullptr) continue;
    expect(cudaMemcpy(payload.data() + b.offset,
                      b.device_ptr,
                      static_cast<std::size_t>(b.size_bytes),
                      cudaMemcpyDeviceToHost) == cudaSuccess,
           (std::string(label) + ": payload staging copy failed").c_str());
  }

  simpatico::payload_fetch_fn fetch =
    [&payload](std::uint64_t off, std::size_t sz, void* dst, rmm::cuda_stream_view s) {
      if (cudaMemcpyAsync(dst, payload.data() + off, sz, cudaMemcpyHostToDevice, s.value()) !=
          cudaSuccess)
        throw std::runtime_error("memory_roundtrip: fetch copy failed");
    };

  std::string rerr;
  simpatico::compressed_table ct2 =
    simpatico::read_compressed_table_from_memory(header, fetch, stream, mr, &rerr);
  expect(rerr.empty(), (std::string(label) + ": read error: " + rerr).c_str());

  auto out = simpatico::decompress(ct2, stream, mr);
  expect(out != nullptr, (std::string(label) + ": decompress returned null").c_str());
  expect(out->num_columns() == input.num_columns(),
         (std::string(label) + ": decompressed column count").c_str());
  for (int i = 0; i < input.num_columns(); ++i)
    expect(columns_equal_any(input.column(i), out->view().column(i), stream),
           (std::string(label) + ": data mismatch col " + std::to_string(i)).c_str());
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// 1. Fused delta→rle→bitpack plan: exercises codegen_fused_representation
//    serialisation and the multi-buffer fused detect path in rep_from_leaf_desc.
void test_fused_delta_rle_bitpack()
{
  auto t = make_int32_table(1, 4096, 7);
  io_roundtrip("fused_delta_rle_bitpack",
               t->view(),
               "input -> delta -> differences\n"
               "delta.differences -> rle -> values, runs\n"
               "delta.differences.values -> bitpack\n"
               "delta.differences.runs -> bitpack\n");
}

// 1b. Regression: tail-routed nested RLE on a bitpack metadata channel, at a
//     scale where the metadata channel is far shorter than the column.
//
//     `bitpack` is a fused-tree LEAF (build_fused_tree), so `bitpack.chunk_count
//     -> rle` is decoded as a SEPARATE codegen subtree whose true length is the
//     parent's per-chunk count (~ceil(col_rows/kChunkSize)), i.e. ~1000x below
//     the column row count. The decode grid is ceil(rep->num_rows / kChunkSize),
//     so the subtree's rep MUST carry its own length. Before the fix,
//     reconstruction gave every fused rep the *column* row count, so this
//     subtree launched ceil(col_rows/1024) blocks against per-chunk metadata
//     built for only ceil(chunk_count_len/1024) chunks — a device out-of-bounds
//     read (context-fatal on real data / hardware). ~300k rows yields ~293
//     parent chunks, so the buggy grid overran a 2-entry rle_runs_offsets by
//     ~290 blocks. The tiny hand-written plans elsewhere never dispatch a nested
//     metadata subtree at this scale, which is why this went uncaught.
void test_nested_metadata_rle_scale()
{
  auto t = make_int32_table(1, 300000, 5);
  io_roundtrip("nested_metadata_rle_scale",
               t->view(),
               "input -> bitpack -> chunk_min, chunk_count, chunk_bits, packed\n"
               "bitpack.chunk_min -> identity\n"
               "bitpack.chunk_bits -> identity\n"
               "bitpack.packed -> identity\n"
               "bitpack.chunk_count -> rle -> runs, values\n"
               "bitpack.chunk_count.runs -> rle -> runs, values\n"
               "bitpack.chunk_count.values -> rle -> runs, values\n");
}

// 2. FOR + fused bitpack on deltas: JIT fused region covers both ops.
void test_for_bitpack()
{
  auto t = make_int32_table(1, 4096, 11);
  io_roundtrip("for_bitpack",
               t->view(),
               "input -> for -> deltas, references\n"
               "for.deltas -> bitpack\n");
}

// 3. FOR only: deltas stored as Raw fixed-stride leaf (no downstream bitpack).
void test_for_only()
{
  auto t = make_int32_table(1, 4096, 13);
  io_roundtrip("for_only", t->view(), "input -> for -> deltas, references\n");
}

// 3b. ZigZag leaf: exercises the codegen_fused_representation("zigzag") channel
//     write/read (OpId::Zigzag -> make_fused_rep), terminal and with an
//     entropy tail on the stored channel.
void test_zigzag()
{
  auto t = make_int32_table(1, 4096, 113);
  io_roundtrip("zigzag_terminal",
               t->view(),
               "input -> delta -> differences\n"
               "delta.differences -> zigzag -> zigzag\n");
  io_roundtrip("zigzag_ans",
               t->view(),
               "input -> delta -> differences\n"
               "delta.differences -> zigzag -> zigzag\n"
               "delta.differences.zigzag -> ans\n");
}

// 4. ALP (floating-point): exercises alp_compressed_representation with its
//    four output channels and alp_rd leaf_meta on a FLOAT32 column.
void test_alp_f32()
{
  auto t = make_f32_table(1, 4096, 17);
  io_roundtrip("alp_f32", t->view(), "input -> alp\n");
}

// Bitextract: multi-output op whose planes are terminal channel leaves,
// exercising per-node output_names + terminal-slot attachment on read.
void test_bitextract_f32()
{
  auto t = make_f32_table(1, 4096, 29);
  io_roundtrip(
    "bitextract_f32", t->view(), "input -> bitextract_f32 -> sign, exponent, mantissa\n");
}

// Bitjoin DAG: the reconvergent node carries structured attrs (input node refs
// + channels) that must survive the structural node serialization.
void test_bitjoin_f32()
{
  auto t = make_f32_table(1, 4096, 31);
  io_roundtrip("bitjoin_f32",
               t->view(),
               "input -> bitextract_f32 -> sign, exponent, mantissa\n"
               "bitextract_f32.sign, bitextract_f32.exponent, bitextract_f32.mantissa "
               "-> bitjoin_f32 -> rejoined\n");
}

// ALP-RD (f64): six output channels + right_bw carried in a channel.
void test_alp_rd_f64()
{
  auto t = make_f64_table(1, 4096, 37);
  io_roundtrip("alp_rd_f64", t->view(), "input -> alp_rd\n");
}

// Dictionary (STRING): variable channel set on a STRING column, exercising the
// STRING column dtype tag and keys_offsets/keys_chars/indices channels.
void test_dictionary()
{
  auto t = make_string_table(4096, cudf::get_default_stream());
  io_roundtrip("dictionary", t->view(), "input -> dictionary\n");
}

// 5. Multi-column: three columns with three different plans in one file.
//    Verifies the column loop in write/read and that per-column plans don't bleed.
void test_multi_column()
{
  auto t = make_int32_table(3, 2048, 21);
  io_roundtrip("multi_column",
               t->view(),
               "input -> delta -> differences\n"
               "delta.differences -> rle -> values, runs\n"
               "delta.differences.values -> bitpack\n"
               "delta.differences.runs -> bitpack\n"
               "---\n"
               "input -> for -> deltas, references\n"
               "for.deltas -> bitpack\n"
               "---\n"
               "input -> for -> deltas, references\n");
}

// 6. Column names survive write→read.
void test_column_names_survive()
{
  auto t = make_int32_table(2, 1024, 3);
  io_roundtrip("column_names_survive",
               t->view(),
               "input -> for -> deltas, references\n"
               "---\n"
               "input -> for -> deltas, references\n",
               {"price", "volume"});
}

// 7. Zero-row table: edge case that should produce an empty but valid file
//    that round-trips without error.
void test_zero_rows()
{
  auto t = make_int32_table(1, 0, 0);
  io_roundtrip("zero_rows",
               t->view(),
               "input -> delta -> differences\n"
               "delta.differences -> rle -> values, runs\n"
               "delta.differences.values -> bitpack\n"
               "delta.differences.runs -> bitpack\n");
}

// 8. Error: file does not exist.
void test_error_not_found()
{
  std::string err;
  auto ct = simpatico::read_compressed_table("/nonexistent/path/that/does/not/exist.hpln",
                                             cudf::get_default_stream(),
                                             rmm::mr::get_current_device_resource_ref(),
                                             &err);
  expect(!err.empty(), "error_not_found: expected non-empty error");
  expect(ct.columns.empty(), "error_not_found: expected empty result");
}

// Error: a non-HPLN file (garbage) is rejected rather than crashing.
void test_error_garbage()
{
  TmpFile tmp;
  {
    std::ofstream f(tmp.path, std::ios::binary);
    f.write("garbage data", 12);
  }
  std::string err;
  auto ct = simpatico::read_compressed_table(
    tmp.path, cudf::get_default_stream(), rmm::mr::get_current_device_resource_ref(), &err);
  expect(!err.empty(), "error_garbage: expected non-empty error");
  expect(ct.columns.empty(), "error_garbage: expected empty result");
}

// Error: wrong magic bytes.
void test_error_bad_magic()
{
  TmpFile tmp;
  {
    std::ofstream f(tmp.path, std::ios::binary);
    char bad[] = {'X', 'X', 'X', 'X', 8, 0, 0};
    f.write(bad, sizeof(bad));
  }
  std::string err;
  auto ct = simpatico::read_compressed_table(
    tmp.path, cudf::get_default_stream(), rmm::mr::get_current_device_resource_ref(), &err);
  expect(!err.empty(), "error_bad_magic: expected non-empty error");
}

// Error: correct magic but unsupported version number.
void test_error_bad_version()
{
  TmpFile tmp;
  {
    std::ofstream f(tmp.path, std::ios::binary);
    char data[] = {'H', 'P', 'L', 'N', 99, 0, 0};  // version 99
    f.write(data, sizeof(data));
  }
  std::string err;
  auto ct = simpatico::read_compressed_table(
    tmp.path, cudf::get_default_stream(), rmm::mr::get_current_device_resource_ref(), &err);
  expect(!err.empty(), "error_bad_version: expected non-empty error");
}

void test_malformed_headers_fail_before_payload_fetch()
{
  auto const identity = make_identity_header_fixture();
  auto reject_identity_mutation =
    [&](std::size_t offset, auto value, std::string_view expected_error, std::string_view label) {
      auto header = identity.bytes;
      overwrite_le(header, offset, value);
      expect_memory_header_rejected(header, expected_error, label);
    };

  reject_identity_mutation(
    identity.column_type, std::uint8_t{254}, "invalid column type tag", "invalid column type");
  reject_identity_mutation(identity.column_rows,
                           std::numeric_limits<std::int64_t>::max(),
                           "row count is outside",
                           "oversized column row count");
  reject_identity_mutation(
    identity.root_child, std::uint32_t{99}, "invalid child index", "invalid plan edge");
  reject_identity_mutation(identity.leaf_kind,
                           static_cast<std::uint8_t>(simpatico::OpId::StrSplit),
                           "invalid serialized leaf kind",
                           "structural leaf kind");
  reject_identity_mutation(
    identity.leaf_type, std::uint8_t{254}, "invalid leaf type tag", "invalid leaf type");

  auto const too_many_rows =
    static_cast<std::uint64_t>(std::numeric_limits<cudf::size_type>::max()) + 1;
  reject_identity_mutation(
    identity.leaf_rows, too_many_rows, "row count is outside", "oversized leaf row count");
  reject_identity_mutation(
    identity.leaf_rows, std::uint64_t{3}, "root leaf row count", "root row disagreement");
  reject_identity_mutation(
    identity.buffer_type, std::uint8_t{254}, "invalid type tag", "invalid buffer type");
  reject_identity_mutation(
    identity.buffer_type, std::uint8_t{10}, "not a fixed-width payload type", "string buffer type");
  reject_identity_mutation(identity.buffer_size,
                           std::uint64_t{15},
                           "not divisible by the element size",
                           "non-divisible buffer size");
  reject_identity_mutation(identity.buffer_size,
                           std::uint64_t{12},
                           "identity buffer row count",
                           "identity buffer row disagreement");

  auto oversized_buffer = identity.bytes;
  overwrite_le(oversized_buffer, identity.buffer_type, std::uint8_t{0});
  overwrite_le(oversized_buffer, identity.buffer_size, too_many_rows);
  expect_memory_header_rejected(
    oversized_buffer, "derived row count is outside", "oversized derived buffer row count");

  auto duplicate_destination = identity.bytes;
  std::vector<std::uint8_t> const duplicate_leaf(identity.bytes.begin() + identity.leaf_start,
                                                 identity.bytes.end());
  overwrite_le(duplicate_destination, identity.num_leaves, std::uint16_t{2});
  duplicate_destination.insert(
    duplicate_destination.end(), duplicate_leaf.begin(), duplicate_leaf.end());
  expect_memory_header_rejected(
    duplicate_destination, "duplicate leaf destination", "duplicate leaf destination");
  auto const bitjoin        = make_bitjoin_header_fixture();
  auto invalid_bitjoin_type = bitjoin.bytes;
  overwrite_le(invalid_bitjoin_type, bitjoin.output_type, std::uint8_t{254});
  expect_memory_header_rejected(invalid_bitjoin_type, "plan node", "invalid bitjoin output type");

  auto invalid_bitjoin_input = bitjoin.bytes;
  overwrite_le(invalid_bitjoin_input, bitjoin.input_node, std::uint32_t{99});
  expect_memory_header_rejected(
    invalid_bitjoin_input, "invalid input node index", "invalid bitjoin input node");

  auto overflowing_file_range = identity.bytes;
  overwrite_le(overflowing_file_range,
               identity.buffer_offset,
               std::numeric_limits<std::uint64_t>::max() - std::uint64_t{15});
  TmpFile tmp;
  {
    std::ofstream file(tmp.path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<char const*>(overflowing_file_range.data()),
               static_cast<std::streamsize>(overflowing_file_range.size()));
    expect(static_cast<bool>(file), "overflowing file-range fixture write failed");
  }

  std::string error;
  auto restored = simpatico::read_compressed_table(
    tmp.path, cudf::get_default_stream(), rmm::mr::get_current_device_resource_ref(), &error);
  expect(restored.columns.empty(), "overflowing file payload range returned a table");
  expect(!error.empty(), "overflowing file payload range was not rejected");
}

void test_v10_wire_widths_rejected()
{
  auto const wire_limit = static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max());
  auto reject = [&](simpatico::compressed_table const& source, std::string_view expected_error) {
    std::vector<std::uint8_t> header{1, 2, 3};
    std::vector<simpatico::payload_buffer_ref> buffers{{7, nullptr, 9}};
    std::uint64_t payload_bytes = 11;
    auto const error            = simpatico::build_compressed_table_header(
      source, header, buffers, payload_bytes, cudf::get_default_stream());
    expect(error.find(expected_error) != std::string::npos,
           ("v10 width limit: unexpected error: " + error).c_str());
    expect(header.empty() && buffers.empty() && payload_bytes == 0,
           "v10 width limit: outputs were not cleared on rejection");
  };

  simpatico::compressed_table too_many_columns;
  too_many_columns.columns.resize(wire_limit + 1);
  reject(too_many_columns, "column count");

  simpatico::compressed_table long_column_name;
  long_column_name.columns.resize(1);
  long_column_name.columns.front().name = std::string(wire_limit + 1, 'x');
  reject(long_column_name, "name length");
}

// Error: identity on a STRING column has no single contiguous payload buffer;
// build_compressed_table_header must reject it loudly and clear its outputs.
void test_identity_string_header_rejected()
{
  auto stream = cudf::get_default_stream();
  auto input  = make_string_table(128, stream);
  auto source = simpatico::compress_with_plan(
    input->view(), "input -> identity\n", stream, rmm::mr::get_current_device_resource_ref());

  std::vector<std::uint8_t> header{1, 2, 3};
  std::vector<simpatico::payload_buffer_ref> buffers{{7, nullptr, 9}};
  std::uint64_t payload_bytes = 11;
  auto const error =
    simpatico::build_compressed_table_header(source, header, buffers, payload_bytes, stream);
  expect(!error.empty(), "identity_string_header: expected loud rejection");
  expect(header.empty() && buffers.empty() && payload_bytes == 0,
         "identity_string_header: outputs were not cleared on rejection");
}

// The two production STRING plan shapes from plans/tpch_sf1000 (customer/
// supplier): variable-length "address" (offsets delta -> ans) and constant-
// length "phone" (offsets delta -> rle with terminal runs/values). Exercised
// through BOTH the file path and the in-memory pin path — the shapes the
// rerouted sf1000 plans serialize in production.
void test_str_split_plan_shapes_roundtrip()
{
  auto stream = cudf::get_default_stream();

  std::vector<std::string> addresses;
  std::vector<std::string> phones;
  addresses.reserve(512);
  phones.reserve(512);
  for (int i = 0; i < 512; ++i) {
    addresses.push_back("No. " + std::to_string((i * 37) % 990) + " Elm Street, Apt " +
                        std::to_string(i % 97));
    char buf[16];
    std::snprintf(buf,
                  sizeof(buf),
                  "%02d-%03d-%03d-%03d",
                  i % 100,
                  (i * 7) % 1000,
                  (i * 13) % 1000,
                  (i * 31) % 1000);
    phones.emplace_back(buf);  // constant length 14 — one RLE run of offset deltas
  }
  auto addr_tbl  = make_strings_table(addresses, {}, stream);
  auto phone_tbl = make_strings_table(phones, {}, stream);

  std::string const address_dsl =
    "input -> str_split -> offsets, chars\n"
    "str_split.offsets -> delta -> differences\n"
    "str_split.chars -> deflate\n"
    "str_split.offsets.differences -> ans\n";
  std::string const phone_dsl =
    "input -> str_split -> offsets, chars\n"
    "str_split.offsets -> delta -> differences\n"
    "str_split.offsets.differences -> rle -> runs, values\n"
    "str_split.chars -> bitpack -> chunk_min, chunk_count, chunk_bits, packed\n"
    "str_split.chars.packed -> deflate\n"
    "str_split.chars.chunk_count -> rle -> runs, values\n"
    "str_split.chars.chunk_bits -> bitcomp\n"
    "str_split.chars.chunk_min -> rle -> runs, values\n";

  io_roundtrip("str_split_address_shape", addr_tbl->view(), address_dsl);
  io_roundtrip("str_split_phone_shape", phone_tbl->view(), phone_dsl);
  memory_roundtrip("str_split_address_shape_mem", addr_tbl->view(), address_dsl);
  memory_roundtrip("str_split_phone_shape_mem", phone_tbl->view(), phone_dsl);
}

void test_memory_reader_selection_fetches_only_requested_columns()
{
  auto stream     = cudf::get_default_stream();
  auto mr         = rmm::mr::get_current_device_resource_ref();
  auto input      = make_int32_table(3, 512, 41);
  auto compressed = simpatico::compress_with_plan(input->view(),
                                                  "input -> identity\n---\n"
                                                  "input -> identity\n---\n"
                                                  "input -> identity\n",
                                                  stream,
                                                  mr,
                                                  {"a", "b", "c"});

  std::vector<std::uint8_t> header;
  std::vector<simpatico::payload_buffer_ref> buffers;
  std::uint64_t payload_bytes{0};
  auto const header_error =
    simpatico::build_compressed_table_header(compressed, header, buffers, payload_bytes, stream);
  expect(header_error.empty(), ("selection header: " + header_error).c_str());
  expect(buffers.size() == 3, "selection fixture must have one buffer per column");

  std::vector<std::uint8_t> payload(static_cast<std::size_t>(payload_bytes));
  stream.synchronize();
  for (auto const& buffer : buffers) {
    expect(cudaMemcpy(payload.data() + buffer.offset,
                      buffer.device_ptr,
                      static_cast<std::size_t>(buffer.size_bytes),
                      cudaMemcpyDeviceToHost) == cudaSuccess,
           "selection payload staging failed");
  }

  std::vector<std::uint64_t> fetched_offsets;
  simpatico::payload_fetch_fn fetch = [&payload, &fetched_offsets](
                                        std::uint64_t offset,
                                        std::size_t size,
                                        void* destination,
                                        rmm::cuda_stream_view fetch_stream) {
    fetched_offsets.push_back(offset);
    if (cudaMemcpyAsync(destination,
                        payload.data() + offset,
                        size,
                        cudaMemcpyHostToDevice,
                        fetch_stream.value()) != cudaSuccess) {
      throw std::runtime_error("selection fetch failed");
    }
  };

  std::vector<std::size_t> selected{2, 0};
  std::string error;
  auto restored = simpatico::read_compressed_table_from_memory(
    header,
    fetch,
    stream,
    mr,
    &error,
    std::optional<std::span<const std::size_t>>{std::span<const std::size_t>{selected}});
  expect(error.empty(), ("selection read: " + error).c_str());
  expect(restored.num_columns() == 2, "selection returned the wrong column count");
  expect(restored.columns[0].name == std::optional<std::string>{"c"} &&
           restored.columns[1].name == std::optional<std::string>{"a"},
         "selection did not preserve requested order");
  expect(fetched_offsets == std::vector<std::uint64_t>{buffers[2].offset, buffers[0].offset},
         "selection fetched an unrequested column or changed fetch order");

  auto decoded = simpatico::decompress(std::move(restored), stream, mr);
  expect(columns_equal_any(input->view().column(2), decoded->view().column(0), stream),
         "selection data mismatch for source column 2");
  expect(columns_equal_any(input->view().column(0), decoded->view().column(1), stream),
         "selection data mismatch for source column 0");

  fetched_offsets.clear();
  std::vector<std::size_t> duplicate{1, 1};
  error.clear();
  auto invalid = simpatico::read_compressed_table_from_memory(
    header,
    fetch,
    stream,
    mr,
    &error,
    std::optional<std::span<const std::size_t>>{std::span<const std::size_t>{duplicate}});
  expect(!error.empty() && invalid.num_columns() == 0, "duplicate selection was not rejected");
  expect(fetched_offsets.empty(), "invalid selection fetched payload bytes before failing");

  std::vector<std::size_t> none;
  error.clear();
  auto empty = simpatico::read_compressed_table_from_memory(
    header,
    fetch,
    stream,
    mr,
    &error,
    std::optional<std::span<const std::size_t>>{std::span<const std::size_t>{none}});
  expect(error.empty() && empty.num_columns() == 0,
         "engaged empty selection did not reconstruct an empty table");
  expect(fetched_offsets.empty(), "empty selection fetched payload bytes");
}

void test_memory_reader_selection_fetches_all_buffers_for_selected_column()
{
  auto stream = cudf::get_default_stream();
  auto mr     = rmm::mr::get_current_device_resource_ref();

  std::vector<std::string> values{"alpha", "", "charlie", "delta", "echo", "foxtrot"};
  std::vector<bool> validity{true, false, true, true, false, true};
  auto strings         = make_strings_table(values, validity, stream);
  auto integers        = make_int32_table(1, static_cast<int>(values.size()), 53);
  auto columns         = strings->release();
  auto integer_columns = integers->release();
  columns.push_back(std::move(integer_columns.front()));
  auto input = std::make_unique<cudf::table>(std::move(columns));

  auto compressed =
    simpatico::compress_with_plan(input->view(),
                                  "input -> str_split -> offsets, chars, null_mask\n"
                                  "str_split.offsets -> delta -> differences\n"
                                  "str_split.offsets.differences -> bitpack\n"
                                  "str_split.chars -> lz4\n"
                                  "str_split.null_mask -> identity\n"
                                  "---\n"
                                  "input -> identity\n",
                                  stream,
                                  mr,
                                  {"text", "number"});

  std::vector<std::uint8_t> header;
  std::vector<simpatico::payload_buffer_ref> buffers;
  std::uint64_t payload_bytes{0};
  auto const header_error =
    simpatico::build_compressed_table_header(compressed, header, buffers, payload_bytes, stream);
  expect(header_error.empty(), ("multi-buffer selection header: " + header_error).c_str());

  auto const description = compressed.describe();
  expect(description.size() == 2, "multi-buffer selection fixture must have two columns");
  auto count_buffers = [](auto const& leaves) {
    std::size_t count{0};
    for (auto const& leaf : leaves)
      count += leaf.buffers.size();
    return count;
  };
  auto const string_buffer_count  = count_buffers(description[0]);
  auto const integer_buffer_count = count_buffers(description[1]);
  expect(string_buffer_count > 1, "string selection fixture must have multiple payload buffers");
  expect(integer_buffer_count == 1, "integer selection fixture must have one payload buffer");
  expect(string_buffer_count + integer_buffer_count == buffers.size(),
         "described buffer count does not match serialized payload");

  std::vector<std::uint8_t> payload(static_cast<std::size_t>(payload_bytes));
  stream.synchronize();
  for (auto const& buffer : buffers) {
    if (buffer.size_bytes == 0) continue;
    expect(cudaMemcpy(payload.data() + buffer.offset,
                      buffer.device_ptr,
                      static_cast<std::size_t>(buffer.size_bytes),
                      cudaMemcpyDeviceToHost) == cudaSuccess,
           "multi-buffer selection payload staging failed");
  }

  std::vector<std::uint64_t> fetched_offsets;
  simpatico::payload_fetch_fn fetch = [&payload, &fetched_offsets](
                                        std::uint64_t offset,
                                        std::size_t size,
                                        void* destination,
                                        rmm::cuda_stream_view fetch_stream) {
    if (offset > payload.size() || size > payload.size() - static_cast<std::size_t>(offset))
      throw std::out_of_range("multi-buffer selection fetch range");
    fetched_offsets.push_back(offset);
    if (cudaMemcpyAsync(destination,
                        payload.data() + offset,
                        size,
                        cudaMemcpyHostToDevice,
                        fetch_stream.value()) != cudaSuccess)
      throw std::runtime_error("multi-buffer selection fetch failed");
  };

  auto read_selection = [&](std::size_t index) {
    std::vector<std::size_t> selected{index};
    std::string error;
    auto restored = simpatico::read_compressed_table_from_memory(
      header,
      fetch,
      stream,
      mr,
      &error,
      std::optional<std::span<const std::size_t>>{std::span<const std::size_t>{selected}});
    expect(error.empty(), ("multi-buffer selection read: " + error).c_str());
    expect(restored.num_columns() == 1, "multi-buffer selection returned wrong column count");
    return restored;
  };

  auto restored_string = read_selection(0);
  std::vector<std::uint64_t> expected_string_offsets;
  expected_string_offsets.reserve(string_buffer_count);
  for (std::size_t i = 0; i < string_buffer_count; ++i)
    expected_string_offsets.push_back(buffers[i].offset);
  expect(fetched_offsets == expected_string_offsets,
         "string selection did not fetch exactly all buffers for that column");
  auto decoded_string = simpatico::decompress(std::move(restored_string), stream, mr);
  expect(columns_equal_any(input->view().column(0), decoded_string->view().column(0), stream),
         "multi-buffer string selection data mismatch");

  fetched_offsets.clear();
  auto restored_integer = read_selection(1);
  expect(fetched_offsets == std::vector<std::uint64_t>{buffers[string_buffer_count].offset},
         "integer selection fetched buffers from the string column");
  auto decoded_integer = simpatico::decompress(std::move(restored_integer), stream, mr);
  expect(columns_equal_any(input->view().column(1), decoded_integer->view().column(0), stream),
         "multi-buffer integer selection data mismatch");
}
}  // namespace

int main()
{
  if (cudaSetDevice(0) != cudaSuccess) {
    std::fprintf(stderr, "test_compressed_table_io: cudaSetDevice failed\n");
    return 1;
  }

  struct Case {
    char const* name;
    void (*fn)();
  };
  Case cases[] = {
    {"fused_delta_rle_bitpack", test_fused_delta_rle_bitpack},
    {"nested_metadata_rle_scale", test_nested_metadata_rle_scale},
    {"for_bitpack", test_for_bitpack},
    {"for_only", test_for_only},
    {"zigzag", test_zigzag},
    {"alp_f32", test_alp_f32},
    {"bitextract_f32", test_bitextract_f32},
    {"bitjoin_f32", test_bitjoin_f32},
    {"alp_rd_f64", test_alp_rd_f64},
    {"dictionary", test_dictionary},
    {"multi_column", test_multi_column},
    {"memory_reader_selection", test_memory_reader_selection_fetches_only_requested_columns},
    {"memory_reader_multi_buffer_selection",
     test_memory_reader_selection_fetches_all_buffers_for_selected_column},
    {"column_names_survive", test_column_names_survive},
    {"zero_rows", test_zero_rows},
    {"error_not_found", test_error_not_found},
    {"error_garbage", test_error_garbage},
    {"error_bad_magic", test_error_bad_magic},
    {"error_bad_version", test_error_bad_version},
    {"malformed_headers", test_malformed_headers_fail_before_payload_fetch},
    {"v10_wire_widths_rejected", test_v10_wire_widths_rejected},
    {"identity_string_header_rejected", test_identity_string_header_rejected},
    {"str_split_plan_shapes", test_str_split_plan_shapes_roundtrip},
  };

  int failures = 0;
  for (auto const& c : cases) {
    try {
      c.fn();
      std::printf("  PASS  %s\n", c.name);
    } catch (std::exception const& e) {
      std::fprintf(stderr, "  FAIL  %s: %s\n", c.name, e.what());
      ++failures;
    }
  }

  if (failures == 0) {
    std::printf("test_compressed_table_io: PASS (%zu cases)\n", sizeof(cases) / sizeof(cases[0]));
    return 0;
  }
  std::fprintf(stderr,
               "test_compressed_table_io: FAIL (%d/%zu cases failed)\n",
               failures,
               sizeof(cases) / sizeof(cases[0]));
  return 1;
}
