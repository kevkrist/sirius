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

#include "op/sirius_physical_operator.hpp"

#include <cudf/table/table.hpp>

#include <rmm/resource_ref.hpp>

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
 * aggregate-output null masks the dense emit does not have). The planner ladder still emits only
 * count specs -- the INNER/DIRECT rungs arrive with later planner work, so those forms are
 * currently reachable only by constructing the operator directly (as the Catch2 suites do).
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
  std::size_t counted_key_idx;      ///< Join key column on the counted child.
  duckdb::vector<slot_spec> slots;  ///< Detection emits exactly one; the mechanism takes N.
  uint64_t max_state_bytes;         ///< Engine-owned dense-state budget for this form.
};

}  // namespace groupjoin

class group_join_input : public pipelineable_operator_data {
 public:
  enum class input_side : uint8_t { PRESERVED, COUNTED };

  group_join_input(std::vector<std::shared_ptr<::cucascade::data_batch>> preserved_batches,
                   std::vector<std::shared_ptr<::cucascade::data_batch>> counted_batches);

  [[nodiscard]] std::vector<input_side> const& input_sides() const noexcept { return _input_sides; }

 private:
  struct tagged_batches {
    std::vector<std::shared_ptr<::cucascade::data_batch>> batches;
    std::vector<input_side> sides;
  };

  explicit group_join_input(tagged_batches input);
  [[nodiscard]] static tagged_batches tag_batches(
    std::vector<std::shared_ptr<::cucascade::data_batch>> preserved_batches,
    std::vector<std::shared_ptr<::cucascade::data_batch>> counted_batches);

  std::vector<input_side> _input_sides;
};

/**
 * @brief Fused join+group-by (GROUPJOIN) operator.
 *
 * Planner-wired pathway today: an eligible preserved-side outer equi-join with a grouped COUNT.
 * Children are [preserved, counted] and output is `[key, aggregate]`. Runtime selects
 * direct-address or exact sparse aggregation while preserving SQL NULL semantics; value bundles
 * (SUM/MIN/MAX/AVG) additionally require NULL-free argument columns for the dense strategy and
 * fall back to the mask-preserving sparse strategy otherwise. The DIRECT form consumes a single
 * "counted" input at the execute level; its pipeline construction is later planner work, so
 * `build_pipelines` still requires two children.
 */
class sirius_physical_group_join : public sirius_physical_operator {
 public:
  static constexpr SiriusPhysicalOperatorType TYPE = SiriusPhysicalOperatorType::GROUP_JOIN;

  static constexpr std::string_view PRESERVED_PORT = "preserved";
  static constexpr std::string_view COUNTED_PORT   = "counted";

  /// Throws on any @p spec outside the whitelisted (form, bundle) combinations.
  sirius_physical_group_join(duckdb::vector<sirius::logical_type> types,
                             std::size_t estimated_cardinality,
                             groupjoin::group_join_spec spec);

  std::string params_to_string() const override;
  [[nodiscard]] std::string_view input_port_for(
    sirius_physical_operator const& producer) const override;
  [[nodiscard]] MemoryBarrierType input_barrier_for(
    sirius_physical_operator const& producer) const override;

  std::unique_ptr<operator_data> execute(const operator_data& input_data,
                                         rmm::cuda_stream_view stream) override;

  bool is_source() const override { return true; }
  bool is_sink() const override { return true; }

  void build_pipelines(pipeline::sirius_pipeline& current,
                       pipeline::sirius_meta_pipeline& meta_pipeline) override;

  std::optional<task_creation_hint> get_next_task_hint() override;

  std::unique_ptr<operator_data> get_next_task_input_data() override;

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

 private:
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

  groupjoin::group_join_spec _spec;
  strategy _last_strategy = strategy::NOT_RUN;
};

}  // namespace sirius::op
