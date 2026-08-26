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

#include "helper/type_conversions.hpp"
#include "operator_test_utils.hpp"
#include "pipeline/sirius_pipeline.hpp"

#include <rmm/cuda_stream.hpp>
#include <rmm/error.hpp>
#include <rmm/mr/per_device_resource.hpp>
#include <rmm/mr/statistics_resource_adaptor.hpp>
#include <rmm/resource_ref.hpp>

#include <cuda/memory_resource>

#include <catch.hpp>
#include <cucascade/data/data_repository.hpp>
#include <cucascade/memory/common.hpp>
#include <data/convertible_data_batch.hpp>
#include <op/aggregate/group_join_impl.hpp>
#include <op/sirius_physical_group_join.hpp>
#include <utils/group_join_test_builder.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using namespace duckdb;
using namespace sirius::op;
using namespace sirius::test::operator_utils;

namespace {

constexpr uint64_t k_default_max_bytes = 2ULL * 1024 * 1024 * 1024;
// Eight bytes admit no full slot set and force a sparse strategy commit.
constexpr uint64_t k_tiny_max_bytes = 8;

using value_row = std::pair<std::optional<int64_t>, std::optional<int64_t>>;

sirius::logical_type bigint_type() { return sirius::logical_type::make(sirius::type_id::BIGINT); }
sirius::logical_type integer_type() { return sirius::logical_type::make(sirius::type_id::INTEGER); }

groupjoin::group_join_spec make_stream_spec(groupjoin::join_form form,
                                            groupjoin::agg_op op,
                                            std::optional<std::size_t> arg_idx,
                                            uint64_t max_state_bytes,
                                            uint64_t stream_counted_row_bound = 0)
{
  auto spec                     = sirius::test::make_group_join_spec(form,
                                                 op,
                                                 /*preserved_key_idx=*/0,
                                                 /*counted_key_idx=*/0,
                                                 arg_idx,
                                                 bigint_type(),
                                                 max_state_bytes);
  spec.schedule                 = groupjoin::schedule_kind::STREAM;
  spec.stream_counted_row_bound = stream_counted_row_bound;
  return spec;
}

std::unique_ptr<sirius_physical_group_join> make_stream_op(groupjoin::group_join_spec spec)
{
  duckdb::vector<sirius::logical_type> types;
  types.push_back(integer_type());
  types.push_back(bigint_type());
  return std::make_unique<sirius_physical_group_join>(std::move(types),
                                                      /*estimated_cardinality=*/16,
                                                      std::move(spec));
}

std::shared_ptr<cucascade::data_batch> make_kv_batch(cucascade::memory::memory_space& space,
                                                     const std::vector<int32_t>& keys,
                                                     const std::vector<int64_t>& values)
{
  auto key_batch   = make_numeric_batch<int32_t>(space, keys, cudf::type_id::INT32);
  auto value_batch = make_numeric_batch<int64_t>(space, values, cudf::type_id::INT64);
  return concatenate_batches_horizontal({key_batch, value_batch}, space);
}

std::shared_ptr<cucascade::data_batch> make_kv_batch_with_nulls(
  cucascade::memory::memory_space& space,
  const std::vector<int32_t>& keys,
  const std::vector<bool>& key_valids,
  const std::vector<int64_t>& values,
  const std::vector<bool>& value_valids)
{
  auto key_batch =
    make_numeric_batch_with_nulls<int32_t>(space, keys, key_valids, cudf::type_id::INT32);
  auto value_batch =
    make_numeric_batch_with_nulls<int64_t>(space, values, value_valids, cudf::type_id::INT64);
  return concatenate_batches_horizontal({key_batch, value_batch}, space);
}

std::vector<value_row> read_output_rows(operator_data const& output)
{
  auto const& out_batches =
    dynamic_cast<const pipelineable_operator_data&>(output).get_data_batches();
  REQUIRE(out_batches.size() == 1);
  auto const view = sirius::get_cudf_table_view(*out_batches[0]);
  REQUIRE(view.num_columns() == 2);
  auto const keys           = copy_column_to_host<int32_t>(view.column(0));
  auto const key_validity   = copy_validity_to_host(view.column(0));
  auto const values         = copy_column_to_host<int64_t>(view.column(1));
  auto const value_validity = copy_validity_to_host(view.column(1));
  std::vector<value_row> rows;
  rows.reserve(keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i) {
    rows.emplace_back(key_validity[i] ? std::optional<int64_t>(keys[i]) : std::nullopt,
                      value_validity[i] ? std::optional<int64_t>(values[i]) : std::nullopt);
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

/// Drives a streamed operator through direct execute calls: one build input, one accumulate per
/// remaining counted batch, then the emit. Returns the emitted rows sorted by key.
std::vector<value_row> run_streamed(
  sirius_physical_group_join& op,
  std::vector<std::shared_ptr<cucascade::data_batch>> preserved_batches,
  std::vector<std::vector<std::shared_ptr<cucascade::data_batch>>> counted_batches,
  sirius_physical_group_join::strategy expected_strategy)
{
  auto stream          = default_stream();
  bool const is_direct = op.spec().form == groupjoin::join_form::DIRECT;

  std::size_t first_accumulate = 0;
  if (is_direct) {
    REQUIRE(!counted_batches.empty());
    group_join_input build_input(
      {}, std::move(counted_batches[0]), group_join_input::task_role::STREAM_BUILD);
    op.execute(build_input, stream);
    first_accumulate = 1;
  } else {
    group_join_input build_input(
      std::move(preserved_batches), {}, group_join_input::task_role::STREAM_BUILD);
    op.execute(build_input, stream);
  }
  REQUIRE(op.last_strategy() == expected_strategy);

  for (std::size_t i = first_accumulate; i < counted_batches.size(); ++i) {
    group_join_input accumulate_input(
      {}, std::move(counted_batches[i]), group_join_input::task_role::STREAM_ACCUMULATE);
    op.execute(accumulate_input, stream);
  }

  group_join_emit_input emit_input(/*peak_memory_estimate=*/1024 * 1024, /*device_id=*/-1);
  auto output = op.execute(emit_input, stream);
  stream.synchronize();
  return read_output_rows(*output);
}

/// Passthrough to the current device resource that, once armed, throws `rmm::out_of_memory` on
/// the k-th allocation and disarms, so the immediately retried attempt succeeds. Bound into the
/// operator through set_stream_memory_resource_for_testing to exercise the executor's
/// OOM-reschedule contract: an interrupted streamed task re-runs over the same input and state.
/// Consumers like `rmm::device_buffer` type-erase the resource by value (an owning
/// `cuda::mr::any_resource` copy), so the countdown lives in shared state: every copy draws from
/// one counter and the disarm-on-throw reaches them all.
class oom_injection_resource {
 public:
  void arm(std::size_t allocations_before_throw)
  {
    _state->armed     = true;
    _state->remaining = allocations_before_throw;
  }
  void disarm() { _state->armed = false; }

  void* allocate(cuda::stream_ref stream,
                 std::size_t bytes,
                 std::size_t alignment = alignof(std::max_align_t))
  {
    maybe_throw();
    return _upstream.allocate(stream, bytes, alignment);
  }
  void deallocate(cuda::stream_ref stream,
                  void* ptr,
                  std::size_t bytes,
                  std::size_t alignment = alignof(std::max_align_t)) noexcept
  {
    _upstream.deallocate(stream, ptr, bytes, alignment);
  }
  void* allocate_sync(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t))
  {
    return allocate(cuda::stream_ref{cudaStream_t{nullptr}}, bytes, alignment);
  }
  void deallocate_sync(void* ptr,
                       std::size_t bytes,
                       std::size_t alignment = alignof(std::max_align_t)) noexcept
  {
    deallocate(cuda::stream_ref{cudaStream_t{nullptr}}, ptr, bytes, alignment);
  }
  bool operator==(oom_injection_resource const& other) const noexcept
  {
    return _state == other._state;
  }
  friend void get_property(oom_injection_resource const&, cuda::mr::device_accessible) noexcept {}

 private:
  struct countdown {
    bool armed            = false;
    std::size_t remaining = 0;
  };

  void maybe_throw()
  {
    auto& state = *_state;
    if (!state.armed) { return; }
    if (state.remaining == 0) {
      state.armed = false;
      throw rmm::out_of_memory("group_join stream test injection");
    }
    --state.remaining;
  }

  std::shared_ptr<countdown> _state = std::make_shared<countdown>();
  rmm::device_async_resource_ref _upstream{rmm::mr::get_current_device_resource_ref()};
};

constexpr std::size_t k_oom_sweep_limit = 512;

/// Re-executes @p input (a replay-legal build input) with the injector failing at allocation
/// k = 0, 1, ... until an attempt succeeds; returns how many attempts threw, i.e. the k of the
/// successful attempt.
std::size_t execute_build_with_oom_sweep(sirius_physical_group_join& op,
                                         group_join_input& input,
                                         oom_injection_resource& inject,
                                         rmm::cuda_stream_view stream)
{
  for (std::size_t k = 0; k < k_oom_sweep_limit; ++k) {
    inject.arm(k);
    try {
      op.execute(input, stream);
      inject.disarm();
      return k;
    } catch (rmm::out_of_memory const&) {
    }
  }
  FAIL("stream build never succeeded within the OOM sweep limit");
  return k_oom_sweep_limit;
}

/// Executes the emit with the injector failing at allocation k = 0, 1, ... until an attempt
/// succeeds -- every k below the success point must throw the retryable OOM without corrupting
/// the stream state. Returns the successful emit's rows; @p failures reports the successful k.
std::vector<value_row> emit_with_oom_sweep(sirius_physical_group_join& op,
                                           oom_injection_resource& inject,
                                           rmm::cuda_stream_view stream,
                                           std::size_t& failures)
{
  for (std::size_t k = 0; k < k_oom_sweep_limit; ++k) {
    inject.arm(k);
    try {
      group_join_emit_input emit_input(/*peak_memory_estimate=*/1024 * 1024, /*device_id=*/-1);
      auto output = op.execute(emit_input, stream);
      inject.disarm();
      stream.synchronize();
      failures = k;
      return read_output_rows(*output);
    } catch (rmm::out_of_memory const&) {
    }
  }
  FAIL("stream emit never succeeded within the OOM sweep limit");
  return {};
}

/// The one-shot oracle over the same batches (multi-batch input in one task).
std::vector<value_row> run_one_shot(
  groupjoin::join_form form,
  groupjoin::agg_op op,
  std::optional<std::size_t> arg_idx,
  uint64_t max_state_bytes,
  std::vector<std::shared_ptr<cucascade::data_batch>> preserved_batches,
  std::vector<std::shared_ptr<cucascade::data_batch>> counted_batches)
{
  duckdb::vector<sirius::logical_type> types;
  types.push_back(integer_type());
  types.push_back(bigint_type());
  sirius_physical_group_join one_shot(
    std::move(types),
    /*estimated_cardinality=*/16,
    sirius::test::make_group_join_spec(form, op, 0, 0, arg_idx, bigint_type(), max_state_bytes));
  group_join_input input(std::move(preserved_batches), std::move(counted_batches));
  auto stream = default_stream();
  auto output = one_shot.execute(input, stream);
  stream.synchronize();
  return read_output_rows(*output);
}

//===----------------------------------------------------------------------===//
// Port-driven state-machine fixture (the mock-pipeline pattern of the concat tests).
//===----------------------------------------------------------------------===//

class mock_pipeline : public sirius::pipeline::sirius_pipeline {
 public:
  explicit mock_pipeline(const sirius::pipeline::pipeline_build_context& ctx) : sirius_pipeline(ctx)
  {
  }
  void set_finished(bool finished) { _finished = finished; }
  bool is_pipeline_finished() const override { return _finished; }

 private:
  bool _finished = false;
};

struct stream_port_fixture {
  duckdb::unique_ptr<sirius_physical_operator> preserved_producer;
  duckdb::unique_ptr<sirius_physical_operator> counted_producer;
  duckdb::shared_ptr<mock_pipeline> preserved_pipeline;
  duckdb::shared_ptr<mock_pipeline> counted_pipeline;
  cucascade::shared_data_repository preserved_repo;
  cucascade::shared_data_repository counted_repo;

  void attach(sirius_physical_group_join& op, bool with_preserved)
  {
    const sirius::pipeline::pipeline_build_context build_ctx{nullptr, true};
    duckdb::vector<sirius::logical_type> types;
    types.push_back(integer_type());
    sirius::pipeline::sirius_pipeline_build_state build_state;
    counted_pipeline = duckdb::make_shared_ptr<mock_pipeline>(build_ctx);
    counted_producer = duckdb::make_uniq<sirius_physical_operator>(
      SiriusPhysicalOperatorType::PROJECTION, types, 16);
    build_state.add_pipeline_operator(*counted_pipeline, *counted_producer);
    if (with_preserved) {
      preserved_pipeline = duckdb::make_shared_ptr<mock_pipeline>(build_ctx);
      preserved_producer = duckdb::make_uniq<sirius_physical_operator>(
        SiriusPhysicalOperatorType::PROJECTION, types, 16);
      build_state.add_pipeline_operator(*preserved_pipeline, *preserved_producer);
      auto preserved_port           = std::make_unique<sirius_physical_operator::port>();
      preserved_port->type          = MemoryBarrierType::FULL;
      preserved_port->repo          = &preserved_repo;
      preserved_port->src_pipeline  = preserved_pipeline;
      preserved_port->dest_pipeline = nullptr;
      op.add_port(sirius_physical_group_join::PRESERVED_PORT, std::move(preserved_port));
    }
    auto counted_port           = std::make_unique<sirius_physical_operator::port>();
    counted_port->type          = MemoryBarrierType::PIPELINE;
    counted_port->repo          = &counted_repo;
    counted_port->src_pipeline  = counted_pipeline;
    counted_port->dest_pipeline = nullptr;
    op.add_port(sirius_physical_group_join::COUNTED_PORT, std::move(counted_port));
  }
};

}  // namespace

TEST_CASE("group_join stream: ctor rejects STREAM on the COUNT pathway (R1)",
          "[group_join][stream][validation]")
{
  duckdb::vector<sirius::logical_type> types;
  types.push_back(integer_type());
  types.push_back(bigint_type());
  auto spec = sirius::test::make_count_group_join_spec(
    /*preserved_key_idx=*/0, /*counted_key_idx=*/0, std::nullopt, k_default_max_bytes);
  spec.schedule = groupjoin::schedule_kind::STREAM;
  REQUIRE_THROWS_WITH(sirius_physical_group_join(std::move(types), 1, std::move(spec)),
                      Catch::Contains("STREAM is invalid on OUTER_PRESERVING"));
}

TEST_CASE(
  "group_join stream state machine: build claim, sparse serialization, emit-once, and "
  "the emit-pending window",
  "[group_join][stream]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  auto op = make_stream_op(make_stream_spec(
    groupjoin::join_form::INNER, groupjoin::agg_op::MIN, std::size_t{1}, k_tiny_max_bytes));
  stream_port_fixture ports;
  ports.attach(*op, /*with_preserved=*/true);
  auto stream = default_stream();

  // Preserved producer unfinished: the hint waits on it.
  {
    auto hint = op->get_next_task_hint();
    REQUIRE(hint.has_value());
    CHECK(hint->hint == TaskCreationHint::WAITING_FOR_INPUT_DATA);
    CHECK(hint->producer == ports.preserved_producer.get());
  }
  ports.preserved_repo.add_data_batch(make_kv_batch(*space, {1, 2, 2, 3}, {0, 0, 0, 0}));
  {
    auto hint = op->get_next_task_hint();
    REQUIRE(hint.has_value());
    CHECK(hint->hint == TaskCreationHint::WAITING_FOR_INPUT_DATA);
  }

  // Preserved finished with data: the first hint claims the build (CAS one-shot); a second hint
  // must not claim again.
  ports.preserved_pipeline->set_finished(true);
  {
    auto hint = op->get_next_task_hint();
    REQUIRE(hint.has_value());
    CHECK(hint->hint == TaskCreationHint::READY);
    auto second = op->get_next_task_hint();
    REQUIRE(second.has_value());
    CHECK(second->hint == TaskCreationHint::WAITING_FOR_INPUT_DATA);
    CHECK(second->producer == ports.counted_producer.get());
  }

  // Build input: every preserved batch, the STREAM_BUILD role, and a budget-bounded charge.
  auto build_data = op->get_next_task_input_data();
  REQUIRE(build_data != nullptr);
  {
    auto* build_input = dynamic_cast<group_join_input*>(build_data.get());
    REQUIRE(build_input != nullptr);
    CHECK(build_input->role() == group_join_input::task_role::STREAM_BUILD);
    CHECK(build_input->get_data_batches().size() == 1);
    REQUIRE(build_input->peak_memory_estimate_override().has_value());
    CHECK(*build_input->peak_memory_estimate_override() >= k_tiny_max_bytes);
  }
  // Build in flight: no accumulate can pop, and the emit-pending override is not yet armed.
  CHECK(op->get_next_task_input_data() == nullptr);
  CHECK(op->all_ports_empty());

  op->execute(*build_data, stream);
  REQUIRE(op->last_strategy() == sirius_physical_group_join::strategy::SPARSE);

  // THE race window: ports drained, no counted data yet, emit unclaimed -- the operator must
  // report non-empty so update_pipeline_status cannot finish the pipeline and the creator loop
  // still enters for the emit.
  CHECK_FALSE(op->all_ports_empty());

  // Two counted batches: the sparse commit serializes accumulates (in-flight <= 1).
  ports.counted_repo.add_data_batch(make_kv_batch(*space, {2, 3, 9}, {7, 5, 1}));
  ports.counted_repo.add_data_batch(make_kv_batch(*space, {2, 1}, {3, 8}));
  {
    auto hint = op->get_next_task_hint();
    REQUIRE(hint.has_value());
    CHECK(hint->hint == TaskCreationHint::READY);
  }
  auto accumulate_one = op->get_next_task_input_data();
  REQUIRE(accumulate_one != nullptr);
  {
    auto* input = dynamic_cast<group_join_input*>(accumulate_one.get());
    REQUIRE(input != nullptr);
    CHECK(input->role() == group_join_input::task_role::STREAM_ACCUMULATE);
    CHECK(input->get_data_batches().size() == 1);
    CHECK(input->get_preferred_device_id().has_value());
  }
  // Sparse in-flight == 1 blocks the second pop until the first completes.
  CHECK(op->get_next_task_input_data() == nullptr);
  {
    auto hint = op->get_next_task_hint();
    REQUIRE(hint.has_value());
    CHECK(hint->hint == TaskCreationHint::WAITING_FOR_INPUT_DATA);
  }
  op->execute(*accumulate_one, stream);
  auto accumulate_two = op->get_next_task_input_data();
  REQUIRE(accumulate_two != nullptr);
  op->execute(*accumulate_two, stream);

  // Counted finished and drained: exactly one emit is claimable.
  ports.counted_pipeline->set_finished(true);
  {
    auto hint = op->get_next_task_hint();
    REQUIRE(hint.has_value());
    CHECK(hint->hint == TaskCreationHint::READY);
  }
  CHECK_FALSE(op->all_ports_empty());
  auto emit_data = op->get_next_task_input_data();
  REQUIRE(emit_data != nullptr);
  REQUIRE(dynamic_cast<group_join_emit_input*>(emit_data.get()) != nullptr);
  CHECK(emit_data->get_preferred_device_id().has_value());
  // Claimed: the override releases and no second emit exists.
  CHECK(op->all_ports_empty());
  CHECK(op->get_next_task_input_data() == nullptr);
  CHECK_FALSE(op->get_next_task_hint().has_value());

  auto output = op->execute(*emit_data, stream);
  stream.synchronize();
  auto const rows = read_output_rows(*output);
  // Every preserved key matched: MIN is duplicate-agnostic, so key 2's duplicate presence does
  // not scale it, and counted key 9 has no preserved row and is dropped.
  std::vector<value_row> const expected{{1, 8}, {2, 3}, {3, 5}};
  REQUIRE(rows == expected);
}

TEST_CASE(
  "group_join stream state machine: dense accumulates run concurrently and the empty "
  "preserved side discards the counted stream",
  "[group_join][stream]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);
  auto stream = default_stream();

  SECTION("dense commit pops a second accumulate while one is in flight")
  {
    auto op = make_stream_op(make_stream_spec(
      groupjoin::join_form::INNER, groupjoin::agg_op::SUM, std::size_t{1}, k_default_max_bytes));
    stream_port_fixture ports;
    ports.attach(*op, /*with_preserved=*/true);

    ports.preserved_repo.add_data_batch(make_kv_batch(*space, {1, 2, 3}, {0, 0, 0}));
    ports.preserved_pipeline->set_finished(true);
    REQUIRE(op->get_next_task_hint()->hint == TaskCreationHint::READY);
    auto build_data = op->get_next_task_input_data();
    REQUIRE(build_data != nullptr);
    op->execute(*build_data, stream);
    REQUIRE(op->last_strategy() == sirius_physical_group_join::strategy::DENSE);

    ports.counted_repo.add_data_batch(make_kv_batch(*space, {1, 2}, {10, 20}));
    ports.counted_repo.add_data_batch(make_kv_batch(*space, {3, 3}, {30, 5}));
    auto accumulate_one = op->get_next_task_input_data();
    auto accumulate_two = op->get_next_task_input_data();
    REQUIRE(accumulate_one != nullptr);
    REQUIRE(accumulate_two != nullptr);
    // A dense accumulate's charge is the floor: the kernel pass allocates nothing.
    CHECK(*accumulate_one->peak_memory_estimate_override() == 1024 * 1024);
    op->execute(*accumulate_two, stream);
    op->execute(*accumulate_one, stream);

    ports.counted_pipeline->set_finished(true);
    auto emit_data = op->get_next_task_input_data();
    REQUIRE(emit_data != nullptr);
    auto output = op->execute(*emit_data, stream);
    stream.synchronize();
    std::vector<value_row> const expected{{1, 10}, {2, 20}, {3, 35}};
    REQUIRE(read_output_rows(*output) == expected);
  }

  SECTION("empty preserved side enters discard mode: no tasks, no emit")
  {
    auto op = make_stream_op(make_stream_spec(
      groupjoin::join_form::INNER, groupjoin::agg_op::SUM, std::size_t{1}, k_default_max_bytes));
    stream_port_fixture ports;
    ports.attach(*op, /*with_preserved=*/true);

    ports.preserved_pipeline->set_finished(true);
    ports.counted_repo.add_data_batch(make_kv_batch(*space, {1, 2}, {10, 20}));
    // The hint claims discard mode and reports READY so the creator loop drains the repo.
    auto hint = op->get_next_task_hint();
    REQUIRE(hint.has_value());
    CHECK(hint->hint == TaskCreationHint::READY);
    // Draining drops the batches and creates no task; no emit is ever pending.
    CHECK(op->get_next_task_input_data() == nullptr);
    CHECK(op->all_ports_empty());
    ports.counted_pipeline->set_finished(true);
    CHECK_FALSE(op->get_next_task_hint().has_value());
    CHECK(op->get_next_task_input_data() == nullptr);
  }
}

TEST_CASE("group_join stream: dense INNER multi-batch parity with the one-shot result",
          "[group_join][stream]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  // Preserved domain [10, 20] with duplicates; the counted stream carries out-of-range keys
  // (bounds-checked away) and in-domain keys with no preserved row (dropped by the emit
  // predicate) -- the pre-filter/unfiltered batch classes.
  auto const preserved = [&] {
    return std::vector<std::shared_ptr<cucascade::data_batch>>{
      make_kv_batch(*space, {10, 12, 12, 15}, {0, 0, 0, 0}),
      make_kv_batch(*space, {20, 15}, {0, 0})};
  };
  auto const counted_stream = [&] {
    return std::vector<std::vector<std::shared_ptr<cucascade::data_batch>>>{
      {make_kv_batch(*space, {10, 12, 40, 0}, {4, 6, 99, 99})},
      {make_kv_batch(*space, {11, 13, 15, 12}, {1, 2, 3, 8})},
      {make_kv_batch(*space, {20, 20, 5, 12}, {7, 9, 99, 2})}};
  };
  auto const counted_flat = [&] {
    std::vector<std::shared_ptr<cucascade::data_batch>> flat;
    for (auto& group : counted_stream()) {
      for (auto& batch : group) {
        flat.push_back(batch);
      }
    }
    return flat;
  };

  for (auto const op_kind : {groupjoin::agg_op::COUNT_STAR,
                             groupjoin::agg_op::COUNT_VALID,
                             groupjoin::agg_op::SUM,
                             groupjoin::agg_op::MIN,
                             groupjoin::agg_op::MAX}) {
    auto const arg_idx = op_kind == groupjoin::agg_op::COUNT_STAR ? std::optional<std::size_t>{}
                                                                  : std::optional<std::size_t>{1};
    auto op            = make_stream_op(make_stream_spec(
      groupjoin::join_form::INNER, op_kind, arg_idx, k_default_max_bytes, /*row_bound=*/1000));
    auto const streamed =
      run_streamed(*op, preserved(), counted_stream(), sirius_physical_group_join::strategy::DENSE);
    auto const one_shot = run_one_shot(groupjoin::join_form::INNER,
                                       op_kind,
                                       arg_idx,
                                       k_default_max_bytes,
                                       preserved(),
                                       counted_flat());
    REQUIRE(streamed == one_shot);
  }
}

TEST_CASE("group_join stream: sparse INNER (adversarial extrema) and DIRECT ladder parity",
          "[group_join][stream]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);

  SECTION("INNER build commits sparse when the preserved extrema bust the budget")
  {
    auto const preserved = [&] {
      return std::vector<std::shared_ptr<cucascade::data_batch>>{
        make_kv_batch(*space, {1, 1000000}, {0, 0})};
    };
    auto const counted_stream = [&] {
      return std::vector<std::vector<std::shared_ptr<cucascade::data_batch>>>{
        {make_kv_batch(*space, {1, 5}, {3, 99})},
        {make_kv_batch(*space, {1000000, 1}, {8, 2})},
        {make_kv_batch(*space, {7, 1000000}, {99, 4})}};
    };
    auto const counted_flat = [&] {
      std::vector<std::shared_ptr<cucascade::data_batch>> flat;
      for (auto& group : counted_stream()) {
        for (auto& batch : group) {
          flat.push_back(batch);
        }
      }
      return flat;
    };
    // AVG's DOUBLE/DECIMAL output does not fit this file's [key, BIGINT] readback; its streamed
    // coverage is the q17-shaped integration matrix plus the shared SUM+divisor machinery here.
    for (auto const op_kind : {groupjoin::agg_op::SUM, groupjoin::agg_op::MIN}) {
      auto op = make_stream_op(
        make_stream_spec(groupjoin::join_form::INNER, op_kind, std::size_t{1}, k_tiny_max_bytes));
      auto const streamed = run_streamed(
        *op, preserved(), counted_stream(), sirius_physical_group_join::strategy::SPARSE);
      auto const one_shot = run_one_shot(groupjoin::join_form::INNER,
                                         op_kind,
                                         std::size_t{1},
                                         k_tiny_max_bytes,
                                         preserved(),
                                         counted_flat());
      REQUIRE(streamed == one_shot);
    }
  }

  SECTION("DIRECT stream always folds the sparse ladder, NULL keys and arguments included")
  {
    auto const counted_stream = [&] {
      return std::vector<std::vector<std::shared_ptr<cucascade::data_batch>>>{
        {make_kv_batch_with_nulls(
          *space, {1, 0, 2}, {true, false, true}, {5, 7, 0}, {true, true, false})},
        {make_kv_batch(*space, {2, 3}, {4, 6})},
        {make_kv_batch_with_nulls(*space, {0, 3}, {false, true}, {0, 1}, {false, true})}};
    };
    auto const counted_flat = [&] {
      std::vector<std::shared_ptr<cucascade::data_batch>> flat;
      for (auto& group : counted_stream()) {
        for (auto& batch : group) {
          flat.push_back(batch);
        }
      }
      return flat;
    };
    for (auto const op_kind : {groupjoin::agg_op::SUM,
                               groupjoin::agg_op::MIN,
                               groupjoin::agg_op::COUNT_STAR,
                               groupjoin::agg_op::COUNT_VALID}) {
      auto const arg_idx = op_kind == groupjoin::agg_op::COUNT_STAR ? std::optional<std::size_t>{}
                                                                    : std::optional<std::size_t>{1};
      auto op            = make_stream_op(
        make_stream_spec(groupjoin::join_form::DIRECT, op_kind, arg_idx, k_default_max_bytes));
      auto const streamed =
        run_streamed(*op, {}, counted_stream(), sirius_physical_group_join::strategy::SPARSE);
      // The one-shot oracle runs its own gates (NULL arguments and the sparse-vs-dense split);
      // only the results must match.
      duckdb::vector<sirius::logical_type> types;
      types.push_back(integer_type());
      types.push_back(bigint_type());
      sirius_physical_group_join one_shot(
        std::move(types),
        16,
        sirius::test::make_group_join_spec(groupjoin::join_form::DIRECT,
                                           op_kind,
                                           0,
                                           0,
                                           arg_idx,
                                           bigint_type(),
                                           k_default_max_bytes));
      group_join_input input({}, counted_flat());
      auto stream = default_stream();
      auto output = one_shot.execute(input, stream);
      stream.synchronize();
      REQUIRE(streamed == read_output_rows(*output));
    }
  }
}

TEST_CASE("group_join stream: belt-checks and the dense replay guard throw",
          "[group_join][stream][validation]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);
  auto stream = default_stream();

  auto build_dense = [&](sirius_physical_group_join& op) {
    group_join_input build_input(
      {make_kv_batch(*space, {1, 2, 3}, {0, 0, 0})}, {}, group_join_input::task_role::STREAM_BUILD);
    op.execute(build_input, stream);
    REQUIRE(op.last_strategy() == sirius_physical_group_join::strategy::DENSE);
  };

  SECTION("NOT-NULL proof violation")
  {
    auto op = make_stream_op(make_stream_spec(
      groupjoin::join_form::INNER, groupjoin::agg_op::SUM, std::size_t{1}, k_default_max_bytes));
    build_dense(*op);
    group_join_input bad(
      {},
      {make_kv_batch_with_nulls(*space, {1, 2}, {true, true}, {3, 0}, {true, false})},
      group_join_input::task_role::STREAM_ACCUMULATE);
    REQUIRE_THROWS_WITH(op->execute(bad, stream), Catch::Contains("NOT-NULL proof violated"));
  }

  SECTION("row-bound proof violation")
  {
    auto op = make_stream_op(make_stream_spec(groupjoin::join_form::INNER,
                                              groupjoin::agg_op::SUM,
                                              std::size_t{1},
                                              k_default_max_bytes,
                                              /*stream_counted_row_bound=*/3));
    build_dense(*op);
    group_join_input over({},
                          {make_kv_batch(*space, {1, 2, 3, 1, 2}, {1, 2, 3, 4, 5})},
                          group_join_input::task_role::STREAM_ACCUMULATE);
    REQUIRE_THROWS_WITH(op->execute(over, stream), Catch::Contains("row-bound proof violated"));
  }

  SECTION("dense accumulate replay is refused instead of double-applied")
  {
    auto op = make_stream_op(make_stream_spec(
      groupjoin::join_form::INNER, groupjoin::agg_op::SUM, std::size_t{1}, k_default_max_bytes));
    build_dense(*op);
    group_join_input batch(
      {}, {make_kv_batch(*space, {1, 2}, {3, 4})}, group_join_input::task_role::STREAM_ACCUMULATE);
    op->execute(batch, stream);
    REQUIRE_THROWS_WITH(op->execute(batch, stream), Catch::Contains("cannot replay"));
  }
}

TEST_CASE("group_join stream: role charges cover the observed dense state and emit allocations",
          "[group_join][stream][no_history_peak_memory_estimate]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);
  auto stream = default_stream();

  // The streamed dense state is the build's dominant allocation and the emit's CUB proxy: measure
  // both against the charges the operator stamps (budget for the build; state-bytes-again plus
  // outputs for the emit).
  constexpr int64_t range = 4096;
  rmm::mr::statistics_resource_adaptor stats_mr{rmm::mr::get_current_device_resource_ref()};
  auto state = make_group_join_stream_dense_state(groupjoin::dense_value_op::SUM,
                                                  cudf::data_type{cudf::type_id::INT32},
                                                  /*presence_wide=*/false,
                                                  /*min_key=*/0,
                                                  range,
                                                  stream,
                                                  stats_mr);
  stream.synchronize();
  auto const build_observed = static_cast<std::size_t>(stats_mr.get_bytes_counter().peak);
  CHECK(state->state_bytes() >= build_observed);

  auto keys = make_kv_batch(*space, {1, 2, 3, 2}, {5, 6, 7, 8});
  {
    auto const view = sirius::get_cudf_table_view(*keys);
    state->accumulate_preserved(view.column(0), stream);
    auto const rep = view.column(1);
    state->accumulate_counted(view.column(0), &rep, stream);
  }

  rmm::mr::statistics_resource_adaptor emit_mr{rmm::mr::get_current_device_resource_ref()};
  auto emitted = state->emit(/*check_count_product_overflow=*/false, stream, emit_mr);
  stream.synchronize();
  auto const emit_observed = static_cast<std::size_t>(emit_mr.get_bytes_counter().peak);
  // The emit charge's dominant terms: state-bytes-again plus group-bounded outputs.
  CHECK(state->state_bytes() + 1024 * 1024 >= emit_observed);
  REQUIRE(emitted->num_rows() == 3);
}

TEST_CASE("group_join stream: OOM-rescheduled emit replays against unreleased state",
          "[group_join][stream][oom_retry]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);
  auto stream = default_stream();

  SECTION("DIRECT: the ladder survives a failed collapse")
  {
    auto const counted = [&] {
      return std::vector<std::shared_ptr<cucascade::data_batch>>{
        make_kv_batch(*space, {1, 2, 3}, {5, 7, 9}),
        make_kv_batch(*space, {2, 4}, {3, 8}),
        make_kv_batch(*space, {1, 4, 5}, {6, 2, 4})};
    };
    oom_injection_resource inject;
    auto op = make_stream_op(make_stream_spec(
      groupjoin::join_form::DIRECT, groupjoin::agg_op::MIN, std::size_t{1}, k_default_max_bytes));
    op->set_stream_memory_resource_for_testing(rmm::device_async_resource_ref{inject});

    auto batches = counted();
    group_join_input build_input({}, {batches[0]}, group_join_input::task_role::STREAM_BUILD);
    op->execute(build_input, stream);
    for (std::size_t i = 1; i < batches.size(); ++i) {
      group_join_input accumulate({}, {batches[i]}, group_join_input::task_role::STREAM_ACCUMULATE);
      op->execute(accumulate, stream);
    }

    std::size_t failures = 0;
    auto const rows      = emit_with_oom_sweep(*op, inject, stream, failures);
    CHECK(failures > 0);  // at least one attempt failed mid-collapse and was replayed
    auto const oracle = run_one_shot(groupjoin::join_form::DIRECT,
                                     groupjoin::agg_op::MIN,
                                     std::size_t{1},
                                     k_default_max_bytes,
                                     {},
                                     counted());
    REQUIRE(rows == oracle);
  }

  SECTION("INNER: the preserved partial survives a failed combine")
  {
    auto const preserved = [&] {
      return std::vector<std::shared_ptr<cucascade::data_batch>>{
        make_kv_batch(*space, {1, 1000000, 5}, {0, 0, 0})};
    };
    auto const counted = [&] {
      return std::vector<std::shared_ptr<cucascade::data_batch>>{
        make_kv_batch(*space, {1, 5}, {3, 9}),
        make_kv_batch(*space, {1000000, 1}, {8, 2}),
        make_kv_batch(*space, {7, 1000000}, {99, 4})};
    };
    oom_injection_resource inject;
    // The tiny budget forces the sparse commit (ladder plus preserved partial).
    auto op = make_stream_op(make_stream_spec(
      groupjoin::join_form::INNER, groupjoin::agg_op::SUM, std::size_t{1}, k_tiny_max_bytes));
    op->set_stream_memory_resource_for_testing(rmm::device_async_resource_ref{inject});

    group_join_input build_input(preserved(), {}, group_join_input::task_role::STREAM_BUILD);
    op->execute(build_input, stream);
    REQUIRE(op->last_strategy() == sirius_physical_group_join::strategy::SPARSE);
    for (auto& batch : counted()) {
      group_join_input accumulate({}, {batch}, group_join_input::task_role::STREAM_ACCUMULATE);
      op->execute(accumulate, stream);
    }

    std::size_t failures = 0;
    auto const rows      = emit_with_oom_sweep(*op, inject, stream, failures);
    CHECK(failures > 0);
    auto const oracle = run_one_shot(groupjoin::join_form::INNER,
                                     groupjoin::agg_op::SUM,
                                     std::size_t{1},
                                     k_tiny_max_bytes,
                                     preserved(),
                                     counted());
    REQUIRE(rows == oracle);
  }
}

TEST_CASE("group_join stream: OOM-rescheduled build reconstructs from scratch",
          "[group_join][stream][oom_retry]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);
  auto stream = default_stream();

  auto const preserved = [&] {
    return std::vector<std::shared_ptr<cucascade::data_batch>>{
      make_kv_batch(*space, {1, 2, 2, 3}, {0, 0, 0, 0})};
  };
  auto const counted = [&] {
    return std::vector<std::shared_ptr<cucascade::data_batch>>{
      make_kv_batch(*space, {1, 3, 9}, {4, 6, 99}), make_kv_batch(*space, {2, 3}, {1, 2})};
  };

  auto const run_after_swept_build = [&](uint64_t max_state_bytes,
                                         sirius_physical_group_join::strategy expected) {
    oom_injection_resource inject;
    auto op = make_stream_op(make_stream_spec(
      groupjoin::join_form::INNER, groupjoin::agg_op::SUM, std::size_t{1}, max_state_bytes));
    op->set_stream_memory_resource_for_testing(rmm::device_async_resource_ref{inject});

    group_join_input build_input(preserved(), {}, group_join_input::task_role::STREAM_BUILD);
    auto const failures = execute_build_with_oom_sweep(*op, build_input, inject, stream);
    CHECK(failures > 0);  // at least one attempt left partial state behind and was replayed
    REQUIRE(op->last_strategy() == expected);

    for (auto& batch : counted()) {
      group_join_input accumulate({}, {batch}, group_join_input::task_role::STREAM_ACCUMULATE);
      op->execute(accumulate, stream);
    }
    group_join_emit_input emit_input(/*peak_memory_estimate=*/1024 * 1024, /*device_id=*/-1);
    auto output = op->execute(emit_input, stream);
    stream.synchronize();
    auto const oracle = run_one_shot(groupjoin::join_form::INNER,
                                     groupjoin::agg_op::SUM,
                                     std::size_t{1},
                                     max_state_bytes,
                                     preserved(),
                                     counted());
    REQUIRE(read_output_rows(*output) == oracle);
  };

  SECTION("dense commit")
  {
    run_after_swept_build(k_default_max_bytes, sirius_physical_group_join::strategy::DENSE);
  }
  SECTION("sparse commit")
  {
    run_after_swept_build(k_tiny_max_bytes, sirius_physical_group_join::strategy::SPARSE);
  }
}

TEST_CASE(
  "group_join stream: OOM-rescheduled sparse accumulate replays once with its rows recorded",
  "[group_join][stream][oom_retry]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);
  auto stream = default_stream();

  // Distinct batch sizes so a lost row accounting is visible in the total the emit-time COUNT
  // product validation would consume.
  auto const counted = [&] {
    return std::vector<std::shared_ptr<cucascade::data_batch>>{
      make_kv_batch(*space, {1, 2, 3}, {5, 7, 9}),
      make_kv_batch(*space, {2, 4}, {3, 8}),
      make_kv_batch(*space, {1, 4, 5, 6}, {6, 2, 4, 1}),
      make_kv_batch(*space, {6}, {12}),
      make_kv_batch(*space, {3, 7}, {2, 2})};
  };

  oom_injection_resource inject;
  auto op = make_stream_op(make_stream_spec(
    groupjoin::join_form::DIRECT, groupjoin::agg_op::SUM, std::size_t{1}, k_default_max_bytes));
  op->set_stream_memory_resource_for_testing(rmm::device_async_resource_ref{inject});

  auto batches        = counted();
  int64_t total_rows  = 0;
  std::size_t replays = 0;
  for (std::size_t i = 0; i < batches.size(); ++i) {
    total_rows += sirius::get_cudf_table_view(*batches[i]).num_rows();
    auto const role = i == 0 ? group_join_input::task_role::STREAM_BUILD
                             : group_join_input::task_role::STREAM_ACCUMULATE;
    group_join_input input({}, {batches[i]}, role);
    input.set_preferred_device_id(0);
    inject.arm(i);  // escalate the failure point across the stream
    try {
      op->execute(input, stream);
      inject.disarm();
    } catch (rmm::out_of_memory const&) {
      ++replays;
      // The pin rides on the input object, which the reschedule path re-runs unchanged.
      CHECK(input.get_preferred_device_id() == std::optional<int>{0});
      op->execute(input, stream);
    }
    // The one-shot claim records the rows before the fold, so an interrupted-and-replayed fold
    // pairs with already-recorded rows: never a double count, never an undercount.
    CHECK(op->stream_counted_rows_for_testing() == total_rows);
  }
  CHECK(replays > 0);

  group_join_emit_input emit_input(/*peak_memory_estimate=*/1024 * 1024, /*device_id=*/-1);
  auto output = op->execute(emit_input, stream);
  stream.synchronize();
  auto const oracle = run_one_shot(groupjoin::join_form::DIRECT,
                                   groupjoin::agg_op::SUM,
                                   std::size_t{1},
                                   k_default_max_bytes,
                                   {},
                                   counted());
  REQUIRE(read_output_rows(*output) == oracle);
}

TEST_CASE("group_join stream: counted batches spilled to HOST re-materialize per task",
          "[group_join][stream][spill]")
{
  auto* gpu_space  = get_default_gpu_space();
  auto* host_space = get_default_host_space();
  REQUIRE(gpu_space);
  REQUIRE(host_space);
  // The tier converters batch their copies through cudaMemcpyBatchAsync, which requires a real
  // (non-default) CUDA stream -- the same class of stream every pipeline task runs on.
  rmm::cuda_stream task_stream;
  auto const stream = task_stream.view();
  auto& manager     = get_default_memory_manager();

  auto const counted = [&] {
    return std::vector<std::shared_ptr<cucascade::data_batch>>{
      make_kv_batch(*gpu_space, {1, 2, 3}, {5, 7, 9}),
      make_kv_batch(*gpu_space, {2, 4}, {3, 8}),
      make_kv_batch(*gpu_space, {1, 4, 5}, {6, 2, 4})};
  };

  auto op = make_stream_op(make_stream_spec(
    groupjoin::join_form::DIRECT, groupjoin::agg_op::MIN, std::size_t{1}, k_default_max_bytes));
  stream_port_fixture ports;
  ports.attach(*op, /*with_preserved=*/false);

  auto batches = counted();
  for (auto const& batch : batches) {
    ports.counted_repo.add_data_batch(batch);
  }
  // Force every counted batch to HOST between arrival and accumulate: idle repository batches
  // are first-class downgrade candidates under memory pressure.
  for (auto const& batch : batches) {
    sirius::convertible_data_batch wrapper(batch);
    auto const moved = wrapper.convert({host_space}, stream, manager, /*blocking=*/true);
    REQUIRE(moved.has_value());
    auto const ro = batch->to_read_only();
    REQUIRE(ro.get_memory_space()->get_tier() == cucascade::memory::Tier::HOST);
  }

  // Drive the schedule through the ports: every popped task re-materializes its batch through
  // prepare_for_processing (the bytes_to_materialize seam of the pipeline task).
  for (std::size_t i = 0; i < batches.size(); ++i) {
    auto const hint = op->get_next_task_hint();
    REQUIRE(hint.has_value());
    REQUIRE(hint->hint == TaskCreationHint::READY);
    auto input = op->get_next_task_input_data();
    REQUIRE(input != nullptr);
    CHECK(input->get_origin_tiers() == "HOST");
    input->prepare_for_processing(gpu_space, stream);
    op->execute(*input, stream);
  }

  ports.counted_pipeline->set_finished(true);
  REQUIRE(op->get_next_task_hint().has_value());
  auto emit_input = op->get_next_task_input_data();
  REQUIRE(emit_input != nullptr);
  auto output = op->execute(*emit_input, stream);
  stream.synchronize();
  auto const oracle = run_one_shot(groupjoin::join_form::DIRECT,
                                   groupjoin::agg_op::MIN,
                                   std::size_t{1},
                                   k_default_max_bytes,
                                   {},
                                   counted());
  REQUIRE(read_output_rows(*output) == oracle);
}

TEST_CASE("group_join stream: sparse role charges cover the observed allocations",
          "[group_join][stream][no_history_peak_memory_estimate]")
{
  auto* space = get_default_gpu_space();
  REQUIRE(space);
  auto stream = default_stream();

  // The operator allocates through this adaptor; push/pop counters isolate each task's
  // incremental peak -- exactly what its reservation charge must dominate.
  rmm::mr::statistics_resource_adaptor stats{rmm::mr::get_current_device_resource_ref()};

  constexpr cudf::size_type k_rows = 200000;
  auto const make_disjoint_batch   = [&](int32_t base) {
    std::vector<int32_t> keys(k_rows);
    std::vector<int64_t> values(k_rows);
    for (cudf::size_type r = 0; r < k_rows; ++r) {
      keys[static_cast<std::size_t>(r)]   = base + r;
      values[static_cast<std::size_t>(r)] = r;
    }
    return make_kv_batch(*space, keys, values);
  };

  auto const check_task = [&](sirius_physical_group_join& op, operator_data& input) {
    REQUIRE(input.peak_memory_estimate_override().has_value());
    auto const charge = *input.peak_memory_estimate_override();
    stats.push_counters();
    op.execute(input, stream);
    stream.synchronize();
    auto const counters = stats.pop_counters();
    auto const observed = static_cast<std::size_t>(counters.first.peak);
    CAPTURE(charge, observed);
    CHECK(charge >= observed);
  };

  SECTION("DIRECT ladder: build, every accumulate (full carry collapse included), emit")
  {
    auto op = make_stream_op(make_stream_spec(
      groupjoin::join_form::DIRECT, groupjoin::agg_op::SUM, std::size_t{1}, k_default_max_bytes));
    op->set_stream_memory_resource_for_testing(rmm::device_async_resource_ref{stats});
    stream_port_fixture ports;
    ports.attach(*op, /*with_preserved=*/false);

    // Disjoint keys keep every merge growing; batch 8 carries through three resident slots.
    constexpr int k_batches = 8;
    for (int i = 0; i < k_batches; ++i) {
      ports.counted_repo.add_data_batch(make_disjoint_batch(i * k_rows));
    }
    for (int i = 0; i < k_batches; ++i) {
      REQUIRE(op->get_next_task_hint().has_value());
      auto input = op->get_next_task_input_data();
      REQUIRE(input != nullptr);
      check_task(*op, *input);
    }
    ports.counted_pipeline->set_finished(true);
    REQUIRE(op->get_next_task_hint().has_value());
    auto emit_input = op->get_next_task_input_data();
    REQUIRE(emit_input != nullptr);
    check_task(*op, *emit_input);
  }

  SECTION("INNER sparse: build (preserved partial), accumulates, emit with the combine")
  {
    auto op = make_stream_op(make_stream_spec(
      groupjoin::join_form::INNER, groupjoin::agg_op::SUM, std::size_t{1}, k_tiny_max_bytes));
    op->set_stream_memory_resource_for_testing(rmm::device_async_resource_ref{stats});
    stream_port_fixture ports;
    ports.attach(*op, /*with_preserved=*/true);

    ports.preserved_repo.add_data_batch(make_disjoint_batch(0));
    ports.preserved_pipeline->set_finished(true);
    REQUIRE(op->get_next_task_hint().has_value());
    auto build_input = op->get_next_task_input_data();
    REQUIRE(build_input != nullptr);
    check_task(*op, *build_input);
    REQUIRE(op->last_strategy() == sirius_physical_group_join::strategy::SPARSE);

    constexpr int k_batches = 4;
    for (int i = 0; i < k_batches; ++i) {
      ports.counted_repo.add_data_batch(make_disjoint_batch(i * k_rows));
    }
    for (int i = 0; i < k_batches; ++i) {
      REQUIRE(op->get_next_task_hint().has_value());
      auto input = op->get_next_task_input_data();
      REQUIRE(input != nullptr);
      check_task(*op, *input);
    }
    ports.counted_pipeline->set_finished(true);
    REQUIRE(op->get_next_task_hint().has_value());
    auto emit_input = op->get_next_task_input_data();
    REQUIRE(emit_input != nullptr);
    check_task(*op, *emit_input);
  }
}
