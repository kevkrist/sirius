// SPDX-License-Identifier: Apache-2.0
//
// Transactional owning compression: a stage keeps the original intact while building a
// self-contained candidate. Root projections may remove intermediate copies, but no candidate
// leaf aliases the original table.
#include "api/compressed_table_io.hpp"
#include "api/simpatico_codegen.hpp"
#include "codegen/plan/representation.hpp"
#include "compress_internals.hpp"
#include "test_utils.hpp"

#include <cudf/copying.hpp>
#include <cudf/null_mask.hpp>
#include <cudf/strings/strings_column_view.hpp>
#include <cudf/table/table.hpp>
#include <cudf/utilities/default_stream.hpp>
#include <cudf/utilities/error.hpp>

#include <rmm/error.hpp>
#include <rmm/mr/cuda_async_memory_resource.hpp>
#include <rmm/mr/limiting_resource_adaptor.hpp>
#include <rmm/mr/per_device_resource.hpp>

#include <cuda_runtime.h>

#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using simpatico::staged_compression;
using simpatico::try_compress_with_plan;

constexpr char const* kBareIdentity = "input -> identity\n";
constexpr char const* kBareSplit    = "input -> str_split\n";
constexpr char const* kSplitDirect  = "input -> str_split -> offsets, chars, null_mask\n";
constexpr char const* kSplitIdentity =
  "input -> str_split -> offsets, chars, null_mask\n"
  "str_split.offsets -> identity\n"
  "str_split.chars -> identity\n"
  "str_split.null_mask -> identity\n";
constexpr char const* kSplitCodec =
  "input -> str_split -> offsets, chars, null_mask\n"
  "str_split.chars -> lz4\n";
constexpr char const* kSplitCodecNoMask =
  "input -> str_split -> offsets, chars\n"
  "str_split.chars -> lz4\n";

std::vector<std::string> const kValues{"alpha", "", "charlie", "delta", "echo"};
std::vector<bool> const kNulls{true, false, true, true, false};

template <typename T>
concept exposes_columns = requires(T const& value) { value.columns; };

static_assert(!exposes_columns<simpatico::compressed_table_inspection>);
static_assert(!std::is_copy_constructible_v<staged_compression>);
static_assert(!std::is_copy_assignable_v<staged_compression>);
static_assert(!std::is_move_assignable_v<staged_compression>);
static_assert(std::is_nothrow_move_constructible_v<staged_compression>);
static_assert(std::is_nothrow_destructible_v<staged_compression>);

struct pointers {
  void const* data    = nullptr;
  void const* mask    = nullptr;
  void const* offsets = nullptr;
  void const* chars   = nullptr;
};

pointers pointers_of(cudf::column_view column, rmm::cuda_stream_view stream)
{
  pointers result;
  result.data = column.head<void>();
  result.mask = column.null_mask();
  if (column.type().id() == cudf::type_id::STRING && column.num_children() > 0) {
    cudf::strings_column_view strings(column);
    result.offsets = strings.offsets().head<void>();
    result.chars   = strings.chars_begin(stream);
  }
  return result;
}

bool same(pointers const& lhs, pointers const& rhs)
{
  return lhs.data == rhs.data && lhs.mask == rhs.mask && lhs.offsets == rhs.offsets &&
         lhs.chars == rhs.chars;
}

std::set<void const*> published_pointers(simpatico::PlanTree const& tree,
                                         rmm::cuda_stream_view stream)
{
  std::set<void const*> result;
  for (auto const& node : tree.nodes) {
    auto add = [&](simpatico::compressed_representation const* rep) {
      if (rep == nullptr) return;
      for (auto const& channel : rep->named_channels(stream)) {
        if (auto const* ptr = channel.view.head<void>(); ptr != nullptr) result.insert(ptr);
      }
    };
    add(node.rep.get());
    for (auto const& [path, rep] : node.channels) {
      (void)path;
      add(rep.get());
    }
  }
  return result;
}

void expect_no_alias(simpatico::compressed_table const& candidate,
                     pointers const& source,
                     rmm::cuda_stream_view stream,
                     char const* message)
{
  auto const found = published_pointers(*candidate.columns[0].compound, stream);
  for (auto const* ptr : {source.data, source.mask, source.offsets, source.chars}) {
    if (ptr != nullptr) expect(found.count(ptr) == 0, message);
  }
}

struct serialized {
  std::vector<std::uint8_t> header;
  std::vector<std::uint8_t> payload;
};

template <typename TableLike>
serialized serialize(TableLike const& table, rmm::cuda_stream_view requested_stream)
{
  serialized result;
  std::vector<simpatico::payload_buffer_ref> buffers;
  std::uint64_t payload_bytes = 0;
  std::string error;
  rmm::cuda_stream_view stream = requested_stream;
  if constexpr (std::same_as<std::remove_cvref_t<TableLike>,
                             simpatico::compressed_table_inspection>) {
    stream = table.stream();
    expect(stream.value() == requested_stream.value(), "inspection used a different stream");
    error = simpatico::build_compressed_table_header(table, result.header, buffers, payload_bytes);
  } else {
    error = simpatico::build_compressed_table_header(
      table, result.header, buffers, payload_bytes, stream);
  }
  expect(error.empty(), ("header build failed: " + error).c_str());

  result.payload.resize(static_cast<std::size_t>(payload_bytes));
  stream.synchronize();
  for (auto const& buffer : buffers) {
    if (buffer.size_bytes == 0) continue;
    expect(cudaMemcpy(result.payload.data() + buffer.offset,
                      buffer.device_ptr,
                      static_cast<std::size_t>(buffer.size_bytes),
                      cudaMemcpyDeviceToHost) == cudaSuccess,
           "payload copy failed");
  }
  return result;
}

rmm::mr::limiting_resource_adaptor starving_resource(std::size_t budget)
{
  return rmm::mr::limiting_resource_adaptor{
    cuda::mr::any_resource<cuda::mr::device_accessible>{rmm::mr::cuda_async_memory_resource{}},
    budget};
}

cudaError_t always_fails(rmm::cuda_stream_view) noexcept { return cudaErrorUnknown; }

int fail_once_calls = 0;
cudaError_t fails_once(rmm::cuda_stream_view) noexcept
{
  return fail_once_calls++ == 0 ? cudaErrorUnknown : cudaSuccess;
}

struct completion_override {
  explicit completion_override(simpatico::detail::completion_probe probe)
  {
    simpatico::detail::stage_completion_probe = probe;
  }
  ~completion_override() { simpatico::detail::stage_completion_probe = nullptr; }

  completion_override(completion_override const&)            = delete;
  completion_override& operator=(completion_override const&) = delete;
};

void test_reject_returns_exact_original(rmm::cuda_stream_view stream,
                                        rmm::device_async_resource_ref mr)
{
  auto check = [&](char const* label, std::unique_ptr<cudf::table> input, char const* plan) {
    auto expected     = std::make_unique<cudf::table>(input->view(), stream, mr);
    auto const before = pointers_of(input->view().column(0), stream);

    auto stage = try_compress_with_plan(std::move(input), plan, stream, mr);
    expect(stage.ok(), (std::string(label) + ": " + stage.error()).c_str());
    expect(stage.table().num_columns() == 1, "inspection lost a column");

    auto returned = std::move(stage).reject();
    expect(returned != nullptr, "reject returned null");
    expect(same(before, pointers_of(returned->view().column(0), stream)),
           "reject reconstructed instead of returning the original allocations");
    expect(columns_equal_any(expected->view().column(0), returned->view().column(0), stream),
           "reject changed the original value");
  };

  check("identity", make_int32_table(1, 4096, 7), kBareIdentity);
  check("bare split", make_strings_table(kValues, kNulls, stream), kBareSplit);
  check("direct split", make_strings_table(kValues, kNulls, stream), kSplitDirect);
  check("identity-routed split", make_strings_table(kValues, kNulls, stream), kSplitIdentity);
  check("codec-routed split", make_strings_table(kValues, kNulls, stream), kSplitCodec);
}

void test_accept_is_self_contained(rmm::cuda_stream_view stream, rmm::device_async_resource_ref mr)
{
  {
    auto input          = make_int32_table(1, 4096, 13);
    auto expected       = std::make_unique<cudf::table>(input->view(), stream, mr);
    auto const original = pointers_of(input->view().column(0), stream);

    auto stage = try_compress_with_plan(std::move(input), kBareIdentity, stream, mr);
    expect(stage.ok(), stage.error().c_str());
    auto accepted = std::move(stage).accept();

    expect_no_alias(accepted, original, stream, "accepted identity aliases the original");
    auto decoded = std::move(accepted).decompress(stream, mr);
    expect(columns_equal(expected->view().column(0), decoded->view().column(0)),
           "identity accept changed the value");
  }

  auto check_strings = [&](char const* label, std::vector<bool> const& validity, char const* plan) {
    auto input          = make_strings_table(kValues, validity, stream);
    auto expected       = std::make_unique<cudf::table>(input->view(), stream, mr);
    auto const original = pointers_of(input->view().column(0), stream);

    auto stage = try_compress_with_plan(std::move(input), plan, stream, mr);
    expect(stage.ok(), (std::string(label) + ": " + stage.error()).c_str());
    auto accepted = std::move(stage).accept();

    expect_no_alias(accepted, original, stream, "accepted STRING candidate aliases the original");
    auto decoded = simpatico::decompress(accepted, stream, mr);
    expect(strings_equal(expected->view().column(0), decoded->view().column(0), stream),
           (std::string(label) + ": accept changed the strings").c_str());
  };

  check_strings("bare split", kNulls, kBareSplit);
  check_strings("direct split", kNulls, kSplitDirect);
  check_strings("identity-routed split", kNulls, kSplitIdentity);
  check_strings("nullable codec", kNulls, kSplitCodec);
  check_strings("projected codec", {}, kSplitCodecNoMask);
}

void poison_mask_padding(cudf::column_view column, rmm::cuda_stream_view stream)
{
  expect(column.null_mask() != nullptr, "poison fixture has no mask");
  auto const allocation = cudf::bitmask_allocation_size_bytes(column.size());
  auto const logical    = static_cast<std::size_t>((column.size() + 7) / 8);
  if (allocation <= logical) return;
  auto* bytes =
    reinterpret_cast<std::uint8_t*>(const_cast<cudf::bitmask_type*>(column.null_mask()));
  expect(
    cudaMemsetAsync(bytes + logical, 0xA5, allocation - logical, stream.value()) == cudaSuccess,
    "failed to poison mask padding");
  stream.synchronize();
}

void test_inspection_and_wire_identity(rmm::cuda_stream_view stream,
                                       rmm::device_async_resource_ref mr)
{
  for (char const* plan : {kSplitDirect, kSplitIdentity, kSplitCodec}) {
    auto input = make_strings_table(kValues, kNulls, stream);
    auto expected =
      serialize(simpatico::compress_with_plan(input->view(), plan, stream, mr), stream);

    auto stage = try_compress_with_plan(std::move(input), plan, stream, mr);
    expect(stage.ok(), stage.error().c_str());
    auto inspection = stage.table();
    expect(inspection.num_columns() == 1, "inspection column count is wrong");
    expect(inspection.num_rows() == static_cast<std::int64_t>(kValues.size()),
           "inspection row count is wrong");
    expect(!inspection.describe().front().empty(), "inspection describes no leaves");

    auto decoded = inspection.decompress(mr);
    expect(strings_equal(make_strings_table(kValues, kNulls, stream)->view().column(0),
                         decoded->view().column(0),
                         stream),
           "live inspection did not decode");

    auto actual = serialize(inspection, stream);
    expect(expected.header == actual.header, "staged compression changed the header");
    expect(expected.payload == actual.payload, "staged compression changed the payload");
    (void)std::move(stage).reject();
  }

  // cuDF defines logical mask bits, not padded bytes. Poison the source padding and verify the
  // staged path declines projection and takes the same owning copy path as the view API.
  auto input = make_strings_table(kValues, kNulls, stream);
  poison_mask_padding(input->view().column(0), stream);
  auto expected =
    serialize(simpatico::compress_with_plan(input->view(), kSplitDirect, stream, mr), stream);
  auto stage = try_compress_with_plan(std::move(input), kSplitDirect, stream, mr);
  expect(stage.ok(), stage.error().c_str());
  auto actual = serialize(stage.table(), stream);
  expect(expected.header == actual.header, "poisoned mask changed the staged header");
  expect(expected.payload == actual.payload, "staged fallback diverged on poisoned mask padding");
  (void)std::move(stage).reject();
}

void enqueue_payload(simpatico::compressed_table_inspection inspection,
                     std::vector<std::uint8_t>& header,
                     void*& pinned,
                     std::size_t& payload_size)
{
  std::vector<simpatico::payload_buffer_ref> buffers;
  std::uint64_t bytes = 0;
  auto const error = simpatico::build_compressed_table_header(inspection, header, buffers, bytes);
  expect(error.empty(), error.c_str());
  payload_size = static_cast<std::size_t>(bytes);
  pinned       = nullptr;
  if (payload_size == 0) return;
  expect(cudaHostAlloc(&pinned, payload_size, cudaHostAllocDefault) == cudaSuccess,
         "cudaHostAlloc failed");
  for (auto const& buffer : buffers) {
    if (buffer.size_bytes == 0) continue;
    expect(cudaMemcpyAsync(static_cast<std::uint8_t*>(pinned) + buffer.offset,
                           buffer.device_ptr,
                           static_cast<std::size_t>(buffer.size_bytes),
                           cudaMemcpyDeviceToHost,
                           inspection.stream().value()) == cudaSuccess,
           "asynchronous payload copy failed");
  }
}

void expect_pending_payload(serialized const& expected,
                            std::vector<std::uint8_t> const& header,
                            void* pinned,
                            std::size_t payload_size)
{
  expect(expected.header == header, "async inspection changed the header");
  expect(expected.payload.size() == payload_size, "async inspection payload size changed");
  if (payload_size != 0) {
    expect(std::memcmp(expected.payload.data(), pinned, payload_size) == 0,
           "decision did not wait for the inspection payload copy");
    expect(cudaFreeHost(pinned) == cudaSuccess, "cudaFreeHost failed");
  }
}

void test_current_tail_is_covered(rmm::cuda_stream_view stream, rmm::device_async_resource_ref mr)
{
  auto run = [&](bool accept) {
    auto input = make_strings_table(kValues, kNulls, stream);
    auto expected =
      serialize(simpatico::compress_with_plan(input->view(), kSplitDirect, stream, mr), stream);
    auto stage = try_compress_with_plan(std::move(input), kSplitDirect, stream, mr);
    expect(stage.ok(), stage.error().c_str());

    std::vector<std::uint8_t> header;
    void* pinned             = nullptr;
    std::size_t payload_size = 0;
    enqueue_payload(stage.table(), header, pinned, payload_size);

    if (accept) {
      auto accepted = std::move(stage).accept();
      expect(accepted.num_columns() == 1, "accept lost the candidate");
    } else {
      auto returned = std::move(stage).reject();
      expect(returned != nullptr, "reject lost the original");
    }
    expect_pending_payload(expected, header, pinned, payload_size);
  };

  run(false);
  run(true);

  // Destruction also waits at the current tail before abandoning both outcomes.
  auto input = make_strings_table(kValues, kNulls, stream);
  auto expected =
    serialize(simpatico::compress_with_plan(input->view(), kSplitDirect, stream, mr), stream);
  std::vector<std::uint8_t> header;
  void* pinned             = nullptr;
  std::size_t payload_size = 0;
  {
    auto stage = try_compress_with_plan(std::move(input), kSplitDirect, stream, mr);
    expect(stage.ok(), stage.error().c_str());
    enqueue_payload(stage.table(), header, pinned, payload_size);
  }
  expect_pending_payload(expected, header, pinned, payload_size);
}

void test_recoverable_failures(rmm::cuda_stream_view stream, rmm::device_async_resource_ref mr)
{
  {
    auto input        = make_strings_table(kValues, kNulls, stream);
    auto const before = pointers_of(input->view().column(0), stream);
    auto stage = try_compress_with_plan(std::move(input), "input -> not_an_operator\n", stream, mr);
    expect(!stage.ok() && !stage.error().empty(), "unknown operator was not a failed stage");
    auto returned = std::move(stage).reject();
    expect(returned != nullptr && same(before, pointers_of(returned->view().column(0), stream)),
           "failed-stage reject lost the original");
  }

  // The first column succeeds before the second operator throws: rollback must still return both
  // exact roots, not merely the column that failed.
  {
    auto input         = make_int32_table(2, 2048, 19);
    auto const before0 = pointers_of(input->view().column(0), stream);
    auto const before1 = pointers_of(input->view().column(1), stream);
    auto stage         = try_compress_with_plan(
      std::move(input), std::string{kBareIdentity} + "---\n" + kBareSplit, stream, mr);
    expect(!stage.ok(), "str_split accepted an INT32 column");
    auto returned = std::move(stage).reject();
    expect(returned != nullptr, "partial failure lost the table");
    expect(same(before0, pointers_of(returned->view().column(0), stream)) &&
             same(before1, pointers_of(returned->view().column(1), stream)),
           "partial failure changed an original allocation");
  }

  bool saw_injected_failure = false;
  for (std::size_t budget : {std::size_t{0}, std::size_t{64}, std::size_t{256}}) {
    auto input        = make_int32_table(1, 16384, 23);
    auto const before = pointers_of(input->view().column(0), stream);
    auto starving     = starving_resource(budget);
    auto stage        = try_compress_with_plan(std::move(input), kBareIdentity, stream, starving);
    if (!stage.ok()) {
      saw_injected_failure = true;
      auto returned        = std::move(stage).reject();
      expect(returned != nullptr && same(before, pointers_of(returned->view().column(0), stream)),
             "device allocation failure lost the original");
    } else {
      (void)std::move(stage).reject();
    }
  }
  expect(saw_injected_failure, "limiting resource never injected a failure");
}

void test_completion_failures(rmm::cuda_stream_view stream, rmm::device_async_resource_ref mr)
{
  auto expect_throws = [](auto&& action, char const* message) {
    bool threw = false;
    try {
      action();
    } catch (std::exception const&) {
      threw = true;
    }
    expect(threw, message);
  };

  {
    auto stage = try_compress_with_plan(make_int32_table(1, 1024, 29), kBareIdentity, stream, mr);
    expect(stage.ok(), stage.error().c_str());
    {
      completion_override broken{&always_fails};
      expect_throws([&] { (void)std::move(stage).accept(); },
                    "accept succeeded without quiescence");
    }
    expect(stage.ok(), "failed accept consumed the stage");
    (void)std::move(stage).reject();
  }

  {
    auto stage = try_compress_with_plan(make_int32_table(1, 1024, 31), kBareIdentity, stream, mr);
    expect(stage.ok(), stage.error().c_str());
    {
      completion_override broken{&always_fails};
      expect_throws([&] { (void)std::move(stage).reject(); },
                    "reject succeeded without quiescence");
    }
    expect(stage.ok(), "failed reject consumed the stage");
    (void)std::move(stage).accept();
  }

  {
    auto stage =
      try_compress_with_plan(make_int32_table(1, 32, 37), "input -> not_an_operator\n", stream, mr);
    expect(!stage.ok(), "fixture did not produce a failed stage");
    {
      completion_override broken{&always_fails};
      expect_throws([&] { (void)std::move(stage).reject(); },
                    "failed-stage reject ignored quiescence");
    }
    expect(!stage.ok(), "failed reject consumed a failed stage");
    (void)std::move(stage).reject();
  }

  fail_once_calls    = 0;
  bool fatal_escaped = false;
  try {
    completion_override broken{&fails_once};
    (void)try_compress_with_plan(
      make_int32_table(1, 32, 41), "input -> not_an_operator\n", stream, mr);
  } catch (cudf::fatal_cuda_error const&) {
    fatal_escaped = true;
  }
  expect(fatal_escaped, "rollback-quiescence failure became an ordinary failed stage");
}

void test_projection_declines_and_empty_fallback(rmm::cuda_stream_view stream,
                                                 rmm::device_async_resource_ref mr)
{
  simpatico::str_split_compressor compressor;

  auto nullable = make_strings_table(kValues, kNulls, stream);
  expect(!compressor.project_staged_root(nullable->view().column(0), stream, mr).has_value(),
         "nullable STRING unexpectedly used an all-borrowed projection");

  auto nonnull = make_strings_table(kValues, {}, stream);
  auto sliced  = cudf::slice(nonnull->view(), {1, 4})[0];
  expect(!compressor.project_staged_root(sliced.column(0), stream, mr).has_value(),
         "sliced STRING projected unre-based offsets");

  auto empty = make_strings_table({}, {}, stream);
  expect(!compressor.project_staged_root(empty->view().column(0), stream, mr).has_value(),
         "empty STRING projected without storage");

  auto stage = try_compress_with_plan(std::move(empty), kBareSplit, stream, mr);
  expect(stage.ok(), stage.error().c_str());
  auto accepted = std::move(stage).accept();
  auto decoded  = simpatico::decompress(accepted, stream, mr);
  expect(decoded->num_rows() == 0, "empty fallback changed the row count");

  // The >2 GiB decline uses the same fallback but requires a >2 GiB device allocation; it remains
  // a resource-gated test rather than pretending a small fixture reaches that branch.
}

void test_move_and_state_errors(rmm::cuda_stream_view stream, rmm::device_async_resource_ref mr)
{
  auto stage = try_compress_with_plan(make_int32_table(1, 256, 43), kBareIdentity, stream, mr);
  expect(stage.ok(), stage.error().c_str());

  staged_compression moved{std::move(stage)};
  expect(moved.ok(), "move constructor lost the transaction");
  expect(!stage.ok() && stage.error().empty(), "moved-from stage is not empty");

  bool empty_reject_threw = false;
  try {
    (void)std::move(stage).reject();
  } catch (std::logic_error const&) {
    empty_reject_threw = true;
  }
  expect(empty_reject_threw, "empty-stage reject was silent");

  auto accepted = std::move(moved).accept();
  expect(accepted.num_columns() == 1, "accept lost the candidate");

  bool decided_table_threw = false;
  try {
    (void)moved.table();
  } catch (std::logic_error const&) {
    decided_table_threw = true;
  }
  expect(decided_table_threw, "decided-stage inspection was silent");

  auto failed =
    try_compress_with_plan(make_int32_table(1, 16, 47), "input -> not_an_operator\n", stream, mr);
  bool failed_accept_threw = false;
  try {
    (void)std::move(failed).accept();
  } catch (std::logic_error const&) {
    failed_accept_threw = true;
  }
  expect(failed_accept_threw && !failed.ok(), "failed-stage accept consumed the stage");
  (void)std::move(failed).reject();
}

void test_multi_column(rmm::cuda_stream_view stream, rmm::device_async_resource_ref mr)
{
  auto numbers_only = make_int32_table(1, static_cast<int>(kValues.size()), 53)->release();
  std::vector<std::unique_ptr<cudf::column>> columns;
  columns.push_back(make_strings_column(kValues, kNulls, stream));
  columns.push_back(std::move(numbers_only.front()));
  auto input    = std::make_unique<cudf::table>(std::move(columns));
  auto expected = std::make_unique<cudf::table>(input->view(), stream, mr);

  auto stage = try_compress_with_plan(
    std::move(input), std::string{kSplitDirect} + "---\n" + kBareIdentity, stream, mr);
  expect(stage.ok(), stage.error().c_str());
  expect(stage.table().num_columns() == 2, "multi-column inspection lost a column");

  auto accepted = std::move(stage).accept();
  auto decoded  = simpatico::decompress(accepted, stream, mr);
  expect(strings_equal(expected->view().column(0), decoded->view().column(0), stream),
         "multi-column STRING changed");
  expect(columns_equal(expected->view().column(1), decoded->view().column(1)),
         "multi-column INT32 changed");
}

void test_non_default_stream(rmm::device_async_resource_ref mr)
{
  cudaStream_t raw = nullptr;
  expect(cudaStreamCreate(&raw) == cudaSuccess, "stream creation failed");
  {
    rmm::cuda_stream_view stream{raw};
    auto input    = make_strings_table(kValues, kNulls, stream);
    auto expected = std::make_unique<cudf::table>(input->view(), stream, mr);
    auto stage    = try_compress_with_plan(std::move(input), kSplitCodec, stream, mr);
    expect(stage.ok(), stage.error().c_str());
    expect(stage.table().stream().value() == raw, "inspection forgot the stage stream");
    (void)serialize(stage.table(), stream);

    auto accepted = std::move(stage).accept();
    auto decoded  = simpatico::decompress(accepted, stream, mr);
    expect(strings_equal(expected->view().column(0), decoded->view().column(0), stream),
           "non-default stream changed the result");
  }
  expect(cudaStreamSynchronize(raw) == cudaSuccess, "stream synchronization failed");
  expect(cudaStreamDestroy(raw) == cudaSuccess, "stream destruction failed");
}

}  // namespace

int main()
{
  if (cudaSetDevice(0) != cudaSuccess) {
    std::fprintf(stderr, "test_staged_compression: cudaSetDevice failed\n");
    return 1;
  }

  try {
    auto stream = cudf::get_default_stream();
    auto mr     = rmm::mr::get_current_device_resource_ref();

    test_reject_returns_exact_original(stream, mr);
    test_accept_is_self_contained(stream, mr);
    test_inspection_and_wire_identity(stream, mr);
    test_current_tail_is_covered(stream, mr);
    test_recoverable_failures(stream, mr);
    test_completion_failures(stream, mr);
    test_projection_declines_and_empty_fallback(stream, mr);
    test_move_and_state_errors(stream, mr);
    test_multi_column(stream, mr);
    test_non_default_stream(mr);

    std::printf("test_staged_compression: PASS\n");
    return 0;
  } catch (std::exception const& error) {
    std::fprintf(stderr, "test_staged_compression: FAIL: %s\n", error.what());
    return 1;
  }
}
