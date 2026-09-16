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

#include <cudf/ast/expressions.hpp>
#include <cudf/table/table.hpp>

#include <rmm/cuda_stream_view.hpp>
#include <rmm/resource_ref.hpp>

#include <op/dynamic_filter/sirius_dynamic_filter.hpp>
#include <op/scan/dynamic_filter_gate.hpp>
#include <op/scan/scan_plan.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace sirius::op::scan {

/// Post-decode mode: membership-only after scan-time AST filtering, or AST plus membership
/// otherwise.
enum class dynamic_filter_apply_mode { membership_masks_only, include_ast_row_masks };

/**
 * @brief ANDs compatible filters into @p tree
 *
 * Column references follow @p plan; hive partitions are skipped. The existing root is returned
 * when no filter applies. Returned expressions and filter-owned scalars must outlive installed
 * AST. A negative device ID selects the current device.
 */
[[nodiscard]] cudf::ast::expression const* merge_dynamic_filters_into_ast(
  cudf::ast::tree& tree,
  cudf::ast::expression const* existing_root,
  sirius::op::sirius_dynamic_filter_set const& filters,
  scan_plan const& plan,
  int device_id = -1);

/**
 * @brief Membership filters a scan already folded into its survivor gather
 *
 * Travels with the scan's output so the downstream DYNAMIC_FILTER operator applies only filters
 * published after the scan's snapshot. Identities stay valid for the query: the channel co-owns
 * every published filter and never removes one.
 */
struct scan_dynamic_filter_result {
  std::vector<sirius::op::sirius_dynamic_filter const*> applied;
  /// Channel size the scan observed when it took its snapshot.
  std::size_t observed_filter_count = 0;

  [[nodiscard]] bool empty() const noexcept { return applied.empty(); }
};

/**
 * @brief Gathers rows that pass visible filters, or returns null when no mask applies
 *
 * Input uses scan output layout. A gate may suppress low-value masks; a negative device ID selects
 * the current device. Filters listed in @p already_applied were folded into the input upstream and
 * are not applied again (nor re-measured).
 */
[[nodiscard]] std::unique_ptr<cudf::table> apply_dynamic_filters_to_view(
  cudf::table_view const& input,
  sirius::op::sirius_dynamic_filter_set const& filters,
  rmm::cuda_stream_view stream,
  dynamic_filter_apply_mode mode = dynamic_filter_apply_mode::include_ast_row_masks,
  dynamic_filter_gate* gate      = nullptr,
  int device_id                  = -1,
  std::span<sirius::op::sirius_dynamic_filter const* const> already_applied = {});

/**
 * @brief Applies filters through the scan-level gate
 *
 * A maskless attempt does not train the gate, preserving useful replicas on other GPUs.
 */
[[nodiscard]] std::unique_ptr<cudf::table> apply_dynamic_filters_gated_view(
  cudf::table_view const& input,
  sirius::op::sirius_dynamic_filter_set const& filters,
  dynamic_filter_gate& gate,
  rmm::cuda_stream_view stream,
  dynamic_filter_apply_mode mode,
  int device_id                                                             = -1,
  std::span<sirius::op::sirius_dynamic_filter const* const> already_applied = {});

/// Resolves a scan output ordinal to its column position in a not-yet-projected split view;
/// null when the ordinal has no data column there (hive partition, out of range).
using probe_position_fn = std::function<std::optional<cudf::size_type>(std::size_t output_col)>;

/**
 * @brief Filters an unmaterialized scan split with one survivor gather
 *
 * Forms the conjunction of @p residual_mask (the scan's own row filter, may be null) and the
 * mask of every applicable membership filter, then gathers @p output_positions of @p input once
 * (all columns when empty): that gather is the split's only materialization. Filter masks are
 * computed on the view's storage carriers (narrow carriers are widened per value) in the gate's
 * cascade order, each using the running conjunction as a stencil so rows already dropped are not
 * probed. Per-filter marginal keep ratios and the scan-level ratio are recorded on @p gate from
 * device-side survivor counts; the applied identities are reported through @p applied.
 *
 * Returns null when nothing applies (no residual and no usable filter); the caller then
 * materializes as it otherwise would. Reads of @p input are only enqueued on @p stream.
 *
 * Blocking: the single `cudf::apply_boolean_mask` keeps its own host synchronization; the
 * survivor counts ride on it (copied to pinned memory before the gather, event-guarded) and add
 * no wait in practice.
 */
[[nodiscard]] std::unique_ptr<cudf::table> gather_view_survivors(
  cudf::table_view const& input,
  std::span<cudf::size_type const> output_positions,
  probe_position_fn const& probe_position,
  std::unique_ptr<cudf::column> residual_mask,
  sirius::op::sirius_dynamic_filter_set const* filters,
  dynamic_filter_gate* gate,
  int device_id,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr,
  scan_dynamic_filter_result* applied);

}  // namespace sirius::op::scan
