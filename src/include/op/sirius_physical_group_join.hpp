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

#pragma once

#include "op/dynamic_filter/dynamic_filter_publish_plan.hpp"
#include "op/dynamic_filter/dynamic_filter_stats.hpp"
#include "op/sirius_physical_operator.hpp"

#include <cudf/table/table.hpp>

#include <rmm/resource_ref.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace sirius::op {

/**
 * @brief Planner-to-execution contract for the GROUPJOIN framework.
 *
 * The detection ladder in `sirius_plan_aggregate.cpp` (entry point `try_plan_group_join`) emits a
 * `group_join_spec` describing which fused join+group-by shape it recognized;
 * `sirius_physical_group_join` stores the spec and maps it onto the monomorphized kernels in
 * `src/cuda/group_join_impl.cu`. The executor accepts the COUNT pathway (`OUTER_PRESERVING` with
 * one COUNT_STAR/COUNT_VALID slot) plus the INNER and DIRECT forms with any one
 * COUNT/SUM/MIN/MAX/AVG slot; value bundles over `OUTER_PRESERVING` fail closed (they would need
 * aggregate-output null masks the dense emit does not have). The planner ladder emits count specs
 * (rung P0), two-child INNER specs (rung P1), and single-child DIRECT specs (rung P2); rungs P1
 * and P2 sit behind `operator_params.enable_group_join`.
 */
namespace groupjoin {

/// Join semantics of the fused operator.
enum class join_form : uint8_t {
  OUTER_PRESERVING,  ///< LEFT/RIGHT outer join: emit every present group with SQL
                     ///< LOJ-then-GROUP-BY empty-group semantics.
  INNER,             ///< Inner join: emit only groups with presence > 0 and matched > 0.
  DIRECT             ///< Single input, plain GROUP BY semantics (NULL key is a group).
};

/// Aggregate operation computed by one slot of the fused state.
enum class agg_op : uint8_t { COUNT_STAR, COUNT_VALID, SUM, MIN, MAX, AVG };

/// Accumulation schedule of the fused operator, decided at plan time (the counted port's barrier
/// is fixed at pipeline conversion, so there is no runtime schedule switch). ONE_SHOT drains both
/// FULL-barrier ports into a single task; STREAM builds state from the completed preserved side,
/// accumulates the counted side one PIPELINE-delivered batch per task, and emits once the counted
/// producer finishes. The COUNT pathway (rung P0) is always ONE_SHOT.
enum class schedule_kind : uint8_t { ONE_SHOT, STREAM };

/// One aggregate output of the fusion.
struct slot_spec {
  agg_op op;                           ///< Aggregate operation.
  std::optional<std::size_t> arg_idx;  ///< Counted-side argument column; nullopt for COUNT(*).
  sirius::logical_type output_type;    ///< Declared result type (drives finalize and widening).
};

/// Everything the planner decides about one fusion; the operator adds no policy of its own.
struct group_join_spec {
  join_form form;                   ///< Join semantics; DIRECT ignores the preserved side.
  std::size_t preserved_key_idx;    ///< Group/join key column on the preserved child.
  std::size_t counted_key_idx;      ///< Join key column on the counted child; DIRECT's group key.
  duckdb::vector<slot_spec> slots;  ///< Detection emits exactly one; the mechanism takes N.
  uint64_t max_state_bytes;         ///< Engine-owned dense-state budget for this form.
  /// Accumulation schedule; the counted-byte plan gate selects STREAM for over-gate shapes whose
  /// streamed-admission proofs pass. STREAM is invalid on OUTER_PRESERVING (the ctor throws).
  schedule_kind schedule = schedule_kind::ONE_SHOT;
  /// Plan-proven upper bound on the counted-side row count backing a streamed SUM/AVG int64
  /// accumulation proof; 0 when no such proof applies. A streamed dense accumulate belt-checks
  /// its running row total against this bound and throws on a violation, so lying source
  /// metadata can fail the query but never corrupt the accumulation.
  uint64_t stream_counted_row_bound = 0;
};

}  // namespace groupjoin

class group_join_input : public pipelineable_operator_data {
 public:
  enum class input_side : uint8_t { PRESERVED, COUNTED };

  /// Which task of the operator's schedule this input feeds. ONE_SHOT is the both-sides single
  /// task; STREAM_BUILD carries the whole preserved side (INNER) or the stream's first counted
  /// batch (DIRECT); STREAM_ACCUMULATE carries exactly one counted batch.
  enum class task_role : uint8_t { ONE_SHOT, STREAM_BUILD, STREAM_ACCUMULATE };

  group_join_input(std::vector<std::shared_ptr<::cucascade::data_batch>> preserved_batches,
                   std::vector<std::shared_ptr<::cucascade::data_batch>> counted_batches,
                   task_role role = task_role::ONE_SHOT);

  [[nodiscard]] std::vector<input_side> const& input_sides() const noexcept { return _input_sides; }
  [[nodiscard]] task_role role() const noexcept { return _role; }

  /// Operator-authoritative reservation charge for streamed tasks; unset for ONE_SHOT inputs.
  void set_peak_memory_estimate(std::size_t bytes) noexcept { _peak_memory_estimate = bytes; }
  [[nodiscard]] std::optional<std::size_t> peak_memory_estimate_override() const noexcept override
  {
    return _peak_memory_estimate;
  }

  /// One-shot latch over this input's streamed side effects (row accounting; a dense
  /// accumulate's state mutation). Returns true exactly once, so an OOM-rescheduled task
  /// re-running over the same input can detect the replay instead of double-applying.
  [[nodiscard]] bool claim_stream_application() const noexcept
  {
    return !_stream_application_claimed.exchange(true, std::memory_order_acq_rel);
  }

 private:
  struct tagged_batches {
    std::vector<std::shared_ptr<::cucascade::data_batch>> batches;
    std::vector<input_side> sides;
  };
  /// Keeps the delegating constructor out of overload resolution against the public one.
  struct tagged_ctor {};

  group_join_input(tagged_ctor, tagged_batches input, task_role role);
  [[nodiscard]] static tagged_batches tag_batches(
    std::vector<std::shared_ptr<::cucascade::data_batch>> preserved_batches,
    std::vector<std::shared_ptr<::cucascade::data_batch>> counted_batches);

  std::vector<input_side> _input_sides;
  task_role _role = task_role::ONE_SHOT;
  std::optional<std::size_t> _peak_memory_estimate;
  mutable std::atomic<bool> _stream_application_claimed{false};
};

/**
 * @brief Synthetic input of a STREAM schedule's emit task.
 *
 * The emit consumes only the operator-owned stream state, so this input carries no batches. It is
 * deliberately NOT a pipelineable_operator_data: the task-creator loop drops an empty
 * pipelineable input without creating a task, while a non-pipelineable input passes through (the
 * scan_operator_input precedent). The base no-op prepare_for_processing applies; the output batch
 * is built on the memory space the stream state recorded at build time.
 */
class group_join_emit_input : public operator_data {
 public:
  group_join_emit_input(std::size_t peak_memory_estimate, int device_id)
    : _peak_memory_estimate(peak_memory_estimate)
  {
    set_preferred_device_id(device_id);
  }

  [[nodiscard]] std::optional<std::size_t> peak_memory_estimate_override() const noexcept override
  {
    return _peak_memory_estimate;
  }

 private:
  std::size_t _peak_memory_estimate;
};

/**
 * @brief Fused join+group-by (GROUPJOIN) operator.
 *
 * Planner-wired pathways today: an eligible preserved-side outer equi-join with a grouped COUNT
 * (rung P0), an eligible INNER equi-join under a single value or count aggregate whose group key
 * is the preserved side's join key (rung P1, the q17 shape), and a single integer-keyed aggregate
 * whose child is an opaque comparison-join subtree (rung P2, the q2 DIRECT shape). Two-child
 * forms take children [preserved, counted]; the DIRECT form takes the single opaque child on the
 * "counted" port. Output is `[key, aggregate]`. Runtime selects direct-address or exact sparse
 * aggregation while preserving SQL NULL semantics; value bundles (SUM/MIN/MAX/AVG) additionally
 * require NULL-free argument columns for the dense strategy and fall back to the mask-preserving
 * sparse strategy otherwise -- which is also what makes DIRECT's opaque child sound: padding
 * NULLs from an outer join inside the child route to the sparse path, which emits the correct
 * NULL aggregates.
 *
 * The preserved child may be a routing-only `DELIM_SCAN` (rung P1's delim provenance): that child
 * produces no data itself -- its `build_pipelines` registers a scheduling dependency on the
 * owning delim join's distinct chain, and the preserved batches arrive from the distinct chain's
 * root pipeline, which `input_port_for` maps onto the "preserved" port.
 *
 * When the detection rung replaced a hash join that would have published dynamic filters, the
 * fused operator carries the equivalent `dynamic_filter_publish_plan`: once the preserved
 * producer pipeline finishes (observed from `get_next_task_hint`, the first poll after that
 * pipeline completes), a single whole-preserved GPU-resident delivery publishes the preserved
 * keys as membership filters to the counted-side scan, with the same best-effort timing and
 * skip semantics as `sirius_physical_hash_join`'s build-side publication.
 *
 * The DIRECT form carries exactly one child and never a publication plan (there is no preserved
 * side); `build_pipelines` and `input_port_for` fail closed on any child count that does not
 * match the spec's form.
 *
 * A spec with `schedule == STREAM` (never emitted for the COUNT pathway) keeps the same ports and
 * pipeline construction but takes a PIPELINE barrier on the counted port and splits the single
 * task into a schedule driven by a single-slot build-state machine (the
 * `per_partition_build_state` pattern of `sirius_physical_hash_join`): one build task claimed by
 * a hint-side compare-exchange once the preserved producer finishes (INNER; a DIRECT stream's
 * first counted batch doubles as its build), then one accumulate task per counted batch pinned to
 * the build's device, then one emit task whose synthetic `group_join_emit_input` fires after the
 * counted producer finishes and every accumulate completed. `all_ports_empty` reports non-empty
 * while a built state's emit is pending so neither the task-creator loop nor
 * `update_pipeline_status` can retire the pipeline in the window between the last accumulate and
 * the emit. INNER commits its strategy once at build time from the preserved-key extrema (the
 * preserved domain is the whole group domain, so out-of-range counted keys are bounds-checked
 * away and pre-filter rows are discarded by the emit predicate); a DIRECT stream always runs the
 * sparse merge ladder, because a dense DIRECT domain cannot be known before the stream ends.
 * Streamed reservation sizing is per role through `peak_memory_estimate_override` on the task
 * inputs rather than the pipeline memory history.
 */
class sirius_physical_group_join : public sirius_physical_operator {
 public:
  static constexpr SiriusPhysicalOperatorType TYPE = SiriusPhysicalOperatorType::GROUP_JOIN;

  static constexpr std::string_view PRESERVED_PORT = "preserved";
  static constexpr std::string_view COUNTED_PORT   = "counted";

  /// Throws on any @p spec outside the whitelisted (form, bundle) combinations, on a DIRECT spec
  /// carrying a publication plan, and on a STREAM schedule over OUTER_PRESERVING (the COUNT
  /// pathway never streams).
  sirius_physical_group_join(duckdb::vector<sirius::logical_type> types,
                             std::size_t estimated_cardinality,
                             groupjoin::group_join_spec spec,
                             dynamic_filter_publish_plan dynamic_filter_plan = {},
                             dynamic_filter_stats* dynamic_filter_stats_sink = nullptr);
  ~sirius_physical_group_join() override;

  std::string params_to_string() const override;
  [[nodiscard]] std::string_view input_port_for(
    sirius_physical_operator const& producer) const override;
  [[nodiscard]] MemoryBarrierType input_barrier_for(
    sirius_physical_operator const& producer) const override;

  void restrict_dynamic_filter_replicas(std::vector<int> const& admitted_gpu_ids)
  {
    _dynamic_filter_plan.restrict_replicas_to(admitted_gpu_ids);
  }

  [[nodiscard]] dynamic_filter_publish_plan const& dynamic_filter_plan() const noexcept
  {
    return _dynamic_filter_plan;
  }

  std::unique_ptr<operator_data> execute(const operator_data& input_data,
                                         rmm::cuda_stream_view stream) override;

  bool is_source() const override { return true; }
  bool is_sink() const override { return true; }

  void build_pipelines(pipeline::sirius_pipeline& current,
                       pipeline::sirius_meta_pipeline& meta_pipeline) override;

  std::optional<task_creation_hint> get_next_task_hint() override;

  std::unique_ptr<operator_data> get_next_task_input_data() override;

  /// Reports non-empty while a streamed schedule's built state has an unclaimed emit pending, so
  /// the pipeline cannot finish (and the task-creator loop still enters) in the window between
  /// the last accumulate's completion and the emit task's creation.
  [[nodiscard]] bool all_ports_empty() override;

  [[nodiscard]] std::size_t no_history_peak_memory_estimate(
    const input_stats& stats) const override;

  [[nodiscard]] groupjoin::group_join_spec const& spec() const noexcept { return _spec; }
  [[nodiscard]] std::size_t preserved_key_idx() const noexcept { return _spec.preserved_key_idx; }
  [[nodiscard]] std::size_t counted_key_idx() const noexcept { return _spec.counted_key_idx; }
  /// Aggregate argument column on the counted child; std::nullopt means COUNT(*).
  [[nodiscard]] std::optional<std::size_t> counted_value_idx() const noexcept
  {
    return _spec.slots[0].arg_idx;
  }
  [[nodiscard]] uint64_t max_state_bytes() const noexcept { return _spec.max_state_bytes; }

  enum class strategy : uint8_t { NOT_RUN, DENSE, SPARSE };
  [[nodiscard]] strategy last_strategy() const noexcept { return _last_strategy; }

  /// Test seam: overrides the memory resource the streamed build/accumulate/emit roles allocate
  /// through (default: the stream state's memory-space allocator), so unit tests can inject
  /// failure and statistics adaptors to exercise OOM-retry and the per-role reservation
  /// property. Throws on a non-STREAM spec.
  void set_stream_memory_resource_for_testing(std::optional<rmm::device_async_resource_ref> mr);

  /// Accumulated streamed counted-row total (test observability for the OOM-retry row
  /// accounting that drives emit-time COUNT product validation). Throws on a non-STREAM spec.
  [[nodiscard]] int64_t stream_counted_rows_for_testing() const;

 protected:
  void on_finalize_operator() override;

 private:
  /// Single-slot streamed-schedule state; allocated in the ctor for STREAM specs only. Defined in
  /// the source file.
  struct stream_state;

  /// STREAM analogues of the base hint/input methods, driven by the stream_state stage machine.
  [[nodiscard]] std::optional<task_creation_hint> stream_task_hint();
  [[nodiscard]] std::unique_ptr<operator_data> stream_task_input_data();

  /// Build task: preserved-key extrema, one-time strategy commit, and state construction (INNER);
  /// ladder initialization folding the first counted batch (DIRECT).
  std::unique_ptr<operator_data> execute_stream_build(group_join_input const& input,
                                                      rmm::cuda_stream_view stream);
  /// Accumulate task: fold exactly one counted batch into the built state.
  std::unique_ptr<operator_data> execute_stream_accumulate(group_join_input const& input,
                                                           rmm::cuda_stream_view stream);
  /// Emit task: selection/finalize over the built state, then state release.
  std::unique_ptr<operator_data> execute_stream_emit(rmm::cuda_stream_view stream);

  /// Folds one counted batch into the sparse merge ladder (callers serialize; in-flight <= 1).
  void stream_sparse_fold(cudf::column_view const& keys,
                          cudf::column_view const* rep_args,
                          rmm::cuda_stream_view stream,
                          rmm::device_async_resource_ref mr);

  /// Releases the stream state's device residents under a device guard; idempotent.
  void release_stream_state();

  /// COUNT-over-outer-join execution (the OUTER_PRESERVING form).
  std::unique_ptr<cudf::table> execute_count_outer(
    std::vector<::cucascade::read_only_data_batch> const& ro_batches,
    std::vector<group_join_input::input_side> const& input_sides,
    rmm::cuda_stream_view stream,
    rmm::device_async_resource_ref mr);

  /// INNER/DIRECT execution over the count and value bundles.
  std::unique_ptr<cudf::table> execute_inner_direct(
    std::vector<::cucascade::read_only_data_batch> const& ro_batches,
    std::vector<group_join_input::input_side> const& input_sides,
    rmm::cuda_stream_view stream,
    rmm::device_async_resource_ref mr);

  /// Claims and runs preserved-key membership publication once the preserved producer pipeline
  /// has finished. Called from get_next_task_hint; a plan-less operator returns immediately.
  void maybe_publish_preserved_membership();

  /// Requires PUBLISHING and leaves FINISHED or FAILED. Device OOM is contained; other failures
  /// propagate.
  void publish_preserved_membership(cudf::table_view const& preserved_view,
                                    rmm::cuda_stream_view stream);

  enum class dynamic_filter_publication_state : uint8_t {
    OPEN,
    PUBLISHING,
    FINISHED,
    FAILED,  ///< Terminal: a failed publication is never retried.
    CLOSED
  };

  groupjoin::group_join_spec _spec;
  strategy _last_strategy = strategy::NOT_RUN;
  /// Non-null exactly for STREAM specs.
  std::unique_ptr<stream_state> _stream;

  // Narrowed before execution; immutable during execution.
  dynamic_filter_publish_plan _dynamic_filter_plan;
  // Non-owning; SiriusContext outlives the plan.
  dynamic_filter_stats* _dynamic_filter_stats = nullptr;
  std::atomic<dynamic_filter_publication_state> _dynamic_filter_publication_state{
    dynamic_filter_publication_state::OPEN};
};

}  // namespace sirius::op
