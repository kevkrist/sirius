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

#include <cudf/binaryop.hpp>
#include <cudf/column/column_factories.hpp>
#include <cudf/stream_compaction.hpp>
#include <cudf/transform.hpp>
#include <cudf/utilities/memory_resource.hpp>
#include <cudf/utilities/pinned_memory.hpp>

#include <rmm/device_buffer.hpp>
#include <rmm/device_uvector.hpp>

#include <cuda/stream_ref>
#include <cuda_runtime_api.h>
#include <nvtx3/nvtx3.hpp>

#include <cucascade/cuda/event.hpp>
#include <cucascade/error.hpp>
#include <log/logging.hpp>
#include <op/dynamic_filter/dynamic_filter_device.hpp>
#include <op/dynamic_filter/dynamic_filter_mask_kernel.hpp>
#include <op/dynamic_filter/dynamic_filter_mask_ops.hpp>
#include <op/dynamic_filter/dynamic_filter_membership_probe.hpp>
#include <op/scan/dynamic_filter_merge.hpp>

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace sirius::op::scan {

cudf::ast::expression const* merge_dynamic_filters_into_ast(
  cudf::ast::tree& tree,
  cudf::ast::expression const* existing_root,
  sirius::op::sirius_dynamic_filter_set const& filters,
  scan_plan const& plan,
  int device_id)
{
  device_id        = sirius::op::detail::resolve_dynamic_filter_device_id(device_id);
  auto const* root = existing_root;
  for (auto const col_idx : filters.filtered_columns()) {
    if (col_idx >= plan.output_layout.size()) { continue; }
    auto const& entry = plan.output_layout[col_idx];
    if (entry.source != scan_plan::output_entry::DATA) { continue; }  // hive — skip
    auto const& parquet_col_name = plan.data_columns[entry.idx].name;

    for (auto const& f : filters.filters_for_column(col_idx)) {
      if (!f->is_available_on_device(device_id)) { continue; }
      auto const* lowerable = dynamic_cast<sirius::op::sirius_ast_lowerable const*>(f.get());
      if (!lowerable) { continue; }
      auto const& col_ref  = tree.emplace<cudf::ast::column_name_reference>(parquet_col_name);
      auto const& fragment = lowerable->to_ast(tree, col_ref, device_id);
      root                 = root ? &tree.emplace<cudf::ast::operation>(
                      cudf::ast::ast_operator::LOGICAL_AND, *root, fragment)
                                  : &fragment;
    }
  }
  return root;
}

namespace {

/// One membership filter selected for a pass, with the gate's prior measurement when it has one.
struct membership_entry {
  std::size_t col_idx;
  sirius::op::sirius_mask_applicable const* filter;
  sirius::op::sirius_dynamic_filter const* identity;
  std::optional<double> recorded;
};

/**
 * Snapshots the channel's membership filters usable on @p device_id whose consumer column
 * satisfies @p column_usable, drops filters the gate already proved useless and filters in
 * @p already_applied, and orders the rest most-selective-first (unknown last) so each mask sees
 * the fewest surviving rows.
 */
template <class ColumnUsable>
std::vector<membership_entry> collect_membership_entries(
  sirius::op::sirius_dynamic_filter_set const& filters,
  dynamic_filter_gate const* gate,
  int device_id,
  std::size_t observed_filter_count,
  ColumnUsable const& column_usable,
  std::span<sirius::op::sirius_dynamic_filter const* const> already_applied)
{
  // Closed explicitly after the sort: the range covers the channel snapshot, gate lookups and
  // ordering, not the mask cascade that follows.
  nvtx3::scoped_range nvtx_snapshot_range{"dynfilter::apply::snapshot"};
  std::vector<membership_entry> entries;
  for (auto const col_idx : filters.filtered_columns()) {
    if (!column_usable(col_idx)) { continue; }
    for (auto const& f : filters.filters_for_column(col_idx)) {
      if (!f->is_available_on_device(device_id)) { continue; }
      if (std::find(already_applied.begin(), already_applied.end(), f.get()) !=
          already_applied.end()) {
        continue;
      }
      auto const* applicable = dynamic_cast<sirius::op::sirius_mask_applicable const*>(f.get());
      if (!applicable) { continue; }
      auto recorded = gate ? gate->filter_keep_ratio(f.get(), observed_filter_count) : std::nullopt;
      if (recorded && dynamic_filter_gate::filter_skippable(*recorded)) { continue; }
      entries.push_back({col_idx, applicable, f.get(), recorded});
    }
  }
  std::stable_sort(entries.begin(), entries.end(), [](auto const& a, auto const& b) {
    return a.recorded.value_or(1.0) < b.recorded.value_or(1.0);
  });
  return entries;
}

/// Pinned host landing zone for the per-step survivor counts. Stream-ordered slab from cuDF's
/// pinned resource (cucascade's small-pinned pool once Sirius installs it): acquiring and
/// releasing enqueue at most an event wait/record on @p stream, never a host sync.
class pinned_counts {
 public:
  pinned_counts(std::size_t n, rmm::cuda_stream_view stream)
    : _mr(cudf::get_pinned_memory_resource()),
      _stream(stream),
      _bytes(n * sizeof(cudf::size_type)),
      _data(static_cast<cudf::size_type*>(
        _mr.allocate(cuda::stream_ref{_stream.value()}, _bytes, alignof(cudf::size_type))))
  {
  }
  ~pinned_counts() noexcept
  {
    _mr.deallocate(cuda::stream_ref{_stream.value()}, _data, _bytes, alignof(cudf::size_type));
  }
  pinned_counts(pinned_counts const&)            = delete;
  pinned_counts& operator=(pinned_counts const&) = delete;

  [[nodiscard]] cudf::size_type* data() noexcept { return _data; }
  [[nodiscard]] std::size_t bytes() const noexcept { return _bytes; }
  [[nodiscard]] cudf::size_type operator[](std::size_t i) const noexcept { return _data[i]; }

 private:
  rmm::host_device_async_resource_ref _mr;
  rmm::cuda_stream_view _stream;
  std::size_t _bytes;
  cudf::size_type* _data;
};

/// Device-side survivor counts landed in pinned memory ahead of the gather so the gather's own
/// host sync covers the copy; wait() only guards against a gather that no longer syncs.
class counts_readback {
 public:
  counts_readback(rmm::device_uvector<cudf::size_type> const& counts, rmm::cuda_stream_view stream)
    : _host(counts.size(), stream)
  {
    CUCASCADE_CUDA_TRY(cudaMemcpyAsync(
      _host.data(), counts.data(), _host.bytes(), cudaMemcpyDeviceToHost, stream.value()));
    _ready.record(stream);
  }

  void wait() { _ready.synchronize(); }
  [[nodiscard]] cudf::size_type operator[](std::size_t i) const noexcept { return _host[i]; }

 private:
  pinned_counts _host;
  cucascade::cuda::cuda_event _ready;
};

/// Gathers @p output_positions of @p input (all columns when empty) where @p mask is non-null
/// and true — the split's only materialization.
std::unique_ptr<cudf::table> gather_with_mask(cudf::table_view const& input,
                                              std::span<cudf::size_type const> output_positions,
                                              cudf::column_view const& mask,
                                              rmm::cuda_stream_view stream,
                                              rmm::device_async_resource_ref mr)
{
  nvtx3::scoped_range nvtx_gather_range{"dynfilter::scan::gather"};
  auto const to_gather = output_positions.empty()
                           ? input
                           : input.select(output_positions.begin(), output_positions.end());
  return cudf::apply_boolean_mask(to_gather, mask, stream, mr);
}

/// What one mask pass over a split produced, for the gate bookkeeping both kernels share.
struct survivor_pass {
  std::unique_ptr<cudf::table> survivors;  // null when nothing applied
  /// Rows entering the first membership mask (after the residual).
  cudf::size_type rows_into_masks = 0;
  /// Per entry: rows surviving its mask; unset for entries whose mask was not applied.
  std::vector<std::optional<cudf::size_type>> rows_after;
};

/// The split and its admitted membership entries, as both kernels see them.
struct survivor_pass_inputs {
  cudf::table_view const& input;
  std::span<cudf::size_type const> output_positions;
  probe_position_fn const& probe_position;
  std::vector<membership_entry> const& entries;
  dynamic_filter_gate* gate;
  std::size_t observed_filter_count;
  int device_id;
  rmm::cuda_stream_view stream;
  rmm::device_async_resource_ref mr;

  [[nodiscard]] cudf::column_view probe_column(membership_entry const& e) const
  {
    return input.column(*probe_position(e.col_idx));  // present by construction of the entry list
  }

  /// Same verdict the cascade records for a probe the filter cannot serve: nothing kept out, so
  /// the filter is not worth attempting again on this scan.
  void record_unservable(membership_entry const& e) const
  {
    if (gate && !e.recorded) {
      gate->record_filter_keep_ratio(e.identity, 1.0, observed_filter_count);
    }
  }
};

/**
 * `dynamic_filter_mask_kernel::cascade`: one stencilled probe kernel per filter, and one
 * streaming AND + count pass after the residual and after every mask. @p residual_mask is
 * consumed.
 */
survivor_pass gather_cascade(survivor_pass_inputs const& in,
                             std::unique_ptr<cudf::column>& residual_mask)
{
  auto const& entries = in.entries;
  survivor_pass pass;
  pass.rows_after.assign(entries.size(), std::nullopt);

  // counts[0] = rows entering the first membership mask (after the residual); counts[i + 1] =
  // rows surviving entry i. Zeroed on the stream, read back after the gather's own sync.
  rmm::device_uvector<cudf::size_type> counts(entries.size() + 1, in.stream, in.mr);
  CUCASCADE_CUDA_TRY(
    cudaMemsetAsync(counts.data(), 0, counts.size() * sizeof(cudf::size_type), in.stream.value()));

  // The running conjunction. Null-free once folded, so it can serve as a cuco stencil and as the
  // gather's boolean mask directly.
  std::unique_ptr<cudf::column> conjunction;
  auto const fold = [&](std::unique_ptr<cudf::column> mask, cudf::size_type* survivor_count) {
    if (!conjunction) {
      conjunction = std::move(mask);
      sirius::op::and_mask_count(*conjunction, std::nullopt, survivor_count, in.stream);
      if (conjunction->nullable()) { conjunction->set_null_mask(rmm::device_buffer{}, 0); }
    } else {
      sirius::op::and_mask_count(*conjunction, mask->view(), survivor_count, in.stream);
    }
  };

  bool const has_residual = residual_mask != nullptr;
  if (has_residual) { fold(std::move(residual_mask), counts.data()); }

  std::vector<bool> mask_applied(entries.size(), false);
  for (std::size_t i = 0; i < entries.size(); ++i) {
    auto const& e             = entries[i];
    bool const* const stencil = conjunction ? conjunction->view().data<bool>() : nullptr;
    auto mask =
      e.filter->compute_mask_if(in.probe_column(e), stencil, in.device_id, in.stream, in.mr);
    if (!mask) {
      in.record_unservable(e);
      continue;
    }
    fold(std::move(mask), counts.data() + i + 1);
    mask_applied[i] = true;
  }
  if (!conjunction) { return pass; }

  counts_readback host_counts(counts, in.stream);
  pass.survivors =
    gather_with_mask(in.input, in.output_positions, conjunction->view(), in.stream, in.mr);
  host_counts.wait();
  pass.rows_into_masks = has_residual ? host_counts[0] : in.input.num_rows();
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (mask_applied[i]) { pass.rows_after[i] = host_counts[i + 1]; }
  }
  return pass;
}

/**
 * `dynamic_filter_mask_kernel::fused`: the residual and every servable membership probe in one
 * pass per split (rounds of k_fused_membership_max_steps), with the two shapes where a pass
 * would only add work short-cut. Returns null — before consuming @p residual_mask or touching the
 * gate — when a filter kind has no fused form, so the caller can run the cascade instead.
 */
std::optional<survivor_pass> gather_fused(survivor_pass_inputs const& in,
                                          std::unique_ptr<cudf::column>& residual_mask)
{
  auto const& entries = in.entries;
  auto const num_rows = in.input.num_rows();

  std::vector<sirius::op::membership_probe> steps(entries.size());
  std::vector<sirius::op::device_probe_status> status(entries.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    status[i] =
      entries[i].filter->device_probe(in.probe_column(entries[i]), in.device_id, steps[i]);
    if (status[i] == sirius::op::device_probe_status::unsupported) { return std::nullopt; }
  }

  survivor_pass pass;
  pass.rows_after.assign(entries.size(), std::nullopt);
  std::vector<std::size_t> ready;  // entry indices, in cascade order
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (status[i] == sirius::op::device_probe_status::ready) {
      ready.push_back(i);
    } else {
      in.record_unservable(entries[i]);
    }
  }

  bool const has_residual = residual_mask != nullptr;
  if (ready.empty()) {
    if (!has_residual) { return pass; }
    // Residual alone: apply_boolean_mask drops its null rows, so the gather is the whole select.
    pass.survivors =
      gather_with_mask(in.input, in.output_positions, residual_mask->view(), in.stream, in.mr);
    pass.rows_into_masks = pass.survivors->num_rows();
    return pass;
  }
  if (!has_residual && ready.size() == 1) {
    // One membership filter, no residual: the filter's plain probe kernel already yields the
    // final mask and the gather's row count is its survivor count — no fold, no count pass, no
    // readback (a fused pass would only add the count reduction to the same probe).
    auto const& e = entries[ready.front()];
    auto mask     = e.filter->compute_mask_if(
      in.probe_column(e), /*stencil=*/nullptr, in.device_id, in.stream, in.mr);
    if (!mask) {
      in.record_unservable(e);
      return pass;
    }
    pass.survivors =
      gather_with_mask(in.input, in.output_positions, mask->view(), in.stream, in.mr);
    pass.rows_into_masks           = num_rows;
    pass.rows_after[ready.front()] = pass.survivors->num_rows();
    return pass;
  }

  // The conjunction is written in place over the residual, or into a fresh mask column without
  // one. counts[0] = rows passing the residual; counts[s + 1] = rows surviving ready step s.
  std::unique_ptr<cudf::column> conjunction =
    has_residual ? std::move(residual_mask)
                 : cudf::make_numeric_column(cudf::data_type{cudf::type_id::BOOL8},
                                             num_rows,
                                             cudf::mask_state::UNALLOCATED,
                                             in.stream,
                                             in.mr);
  rmm::device_uvector<cudf::size_type> counts(ready.size() + 1, in.stream, in.mr);
  CUCASCADE_CUDA_TRY(
    cudaMemsetAsync(counts.data(), 0, counts.size() * sizeof(cudf::size_type), in.stream.value()));
  {
    nvtx3::scoped_range nvtx_fused_range{"dynfilter::apply::fused_mask"};
    auto const residual_view = conjunction->view();
    bool* const out          = conjunction->mutable_view().data<bool>();
    std::vector<sirius::op::membership_probe> round;
    round.reserve(sirius::op::k_fused_membership_max_steps);
    for (std::size_t first = 0; first < ready.size();
         first += sirius::op::k_fused_membership_max_steps) {
      auto const last = std::min(ready.size(), first + sirius::op::k_fused_membership_max_steps);
      round.clear();
      for (auto s = first; s < last; ++s) {
        round.push_back(steps[ready[s]]);
      }
      // A later round folds into the (already null-free) conjunction the previous one wrote.
      bool const first_round = first == 0;
      sirius::op::fused_membership_mask(
        first_round ? (has_residual ? residual_view.data<bool>() : nullptr) : out,
        first_round && has_residual && residual_view.nullable() ? residual_view.null_mask()
                                                                : nullptr,
        first_round ? residual_view.offset() : 0,
        round,
        out,
        num_rows,
        first_round && has_residual ? counts.data() : nullptr,
        counts.data() + 1 + static_cast<std::ptrdiff_t>(first),
        in.stream);
    }
  }
  // The pass folded the residual's validity into the values.
  if (conjunction->nullable()) { conjunction->set_null_mask(rmm::device_buffer{}, 0); }

  counts_readback host_counts(counts, in.stream);
  pass.survivors =
    gather_with_mask(in.input, in.output_positions, conjunction->view(), in.stream, in.mr);
  host_counts.wait();
  pass.rows_into_masks = has_residual ? host_counts[0] : num_rows;
  for (std::size_t s = 0; s < ready.size(); ++s) {
    pass.rows_after[ready[s]] = host_counts[s + 1];
  }
  return pass;
}

}  // namespace

std::unique_ptr<cudf::table> apply_dynamic_filters_to_view(
  cudf::table_view const& input,
  sirius::op::sirius_dynamic_filter_set const& filters,
  rmm::cuda_stream_view stream,
  dynamic_filter_apply_mode mode,
  dynamic_filter_gate* gate,
  int device_id,
  std::span<sirius::op::sirius_dynamic_filter const* const> already_applied)
{
  nvtx3::scoped_range nvtx_range{"dynfilter::apply_output"};
  if (input.num_rows() == 0 || input.num_columns() == 0) { return nullptr; }

  device_id           = sirius::op::detail::resolve_dynamic_filter_device_id(device_id);
  auto const num_cols = static_cast<std::size_t>(input.num_columns());
  auto const mr       = cudf::get_current_device_resource_ref();

  std::unique_ptr<cudf::table> owned;  // most recent step's product backing `current`
  cudf::table_view current = input;
  auto const cascade_step  = [&](std::unique_ptr<cudf::column> mask) -> double {
    nvtx3::scoped_range nvtx_step_range{"dynfilter::apply::cascade_step"};
    if (!mask || current.num_rows() == 0) { return 1.0; }
    auto const rows_before = current.num_rows();
    owned                  = cudf::apply_boolean_mask(current, mask->view(), stream, mr);
    current                = owned->view();
    return static_cast<double>(current.num_rows()) / static_cast<double>(rows_before);
  };

  auto const include_ast_masks = mode == dynamic_filter_apply_mode::include_ast_row_masks;

  if (include_ast_masks) {
    cudf::ast::tree tree;
    cudf::ast::expression const* root = nullptr;
    for (auto const col_idx : filters.filtered_columns()) {
      if (col_idx >= num_cols) { continue; }
      cudf::ast::expression const* col_ref = nullptr;
      for (auto const& f : filters.filters_for_column(col_idx)) {
        if (!f->is_available_on_device(device_id)) { continue; }
        auto const* lowerable = dynamic_cast<sirius::op::sirius_ast_lowerable const*>(f.get());
        if (!lowerable) { continue; }
        if (!col_ref) {
          col_ref =
            &tree.emplace<cudf::ast::column_reference>(static_cast<cudf::size_type>(col_idx));
        }
        auto const& fragment = lowerable->to_ast(tree, *col_ref, device_id);
        root                 = root ? &tree.emplace<cudf::ast::operation>(
                        cudf::ast::ast_operator::LOGICAL_AND, *root, fragment)
                                    : &fragment;
      }
    }
    if (root) {
      // Cross-column AST masks update only the scan-level gate.
      (void)cascade_step(cudf::compute_column(current, *root, stream, mr));
    }
  }

  // Use one filter-count snapshot for every gate measurement in this pass.
  auto const observed_filter_count = filters.filter_count();
  auto const entries               = collect_membership_entries(
    filters,
    gate,
    device_id,
    observed_filter_count,
    [num_cols](std::size_t col_idx) { return col_idx < num_cols; },
    already_applied);

  for (auto const& e : entries) {
    if (current.num_rows() == 0) { break; }
    auto const& probe = current.column(static_cast<cudf::size_type>(e.col_idx));
    auto const kept   = cascade_step(e.filter->compute_mask(probe, device_id, stream, mr));
    if (gate && !e.recorded) {
      gate->record_filter_keep_ratio(e.identity, kept, observed_filter_count);
    }
  }

  if (!owned) { return nullptr; }
  auto const* mode_name = mode == dynamic_filter_apply_mode::include_ast_row_masks
                            ? "include_ast_row_masks"
                            : "membership_masks_only";
  SIRIUS_LOG_DEBUG("[apply_dynamic_filters] device={} mode={} apply: {} -> {} rows.",
                   device_id,
                   mode_name,
                   input.num_rows(),
                   owned->num_rows());
  return owned;
}

std::unique_ptr<cudf::table> gather_view_survivors(
  cudf::table_view const& input,
  std::span<cudf::size_type const> output_positions,
  probe_position_fn const& probe_position,
  std::unique_ptr<cudf::column> residual_mask,
  sirius::op::sirius_dynamic_filter_set const* filters,
  dynamic_filter_gate* gate,
  int device_id,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr,
  scan_dynamic_filter_result* applied,
  sirius::op::dynamic_filter_mask_kernel kernel)
{
  nvtx3::scoped_range nvtx_range{"dynfilter::scan::fused_apply"};
  auto const num_rows = input.num_rows();
  if (num_rows == 0 || input.num_columns() == 0) { return nullptr; }
  if (residual_mask && residual_mask->size() != num_rows) {
    throw std::invalid_argument(
      "[gather_view_survivors] the residual mask does not cover the split's rows");
  }
  device_id = sirius::op::detail::resolve_dynamic_filter_device_id(device_id);

  // The same admission the DYNAMIC_FILTER operator applies: no filters, or a gate that has
  // disabled filtering for the channel size it last saw, means no membership masks here.
  std::size_t observed_filter_count = 0;
  std::vector<membership_entry> entries;
  if (filters && (!gate || gate->applicable(*filters))) {
    observed_filter_count = filters->filter_count();
    entries               = collect_membership_entries(
      *filters,
      gate,
      device_id,
      observed_filter_count,
      [&probe_position](std::size_t col_idx) { return probe_position(col_idx).has_value(); },
      /*already_applied=*/{});
  }
  if (!residual_mask && entries.empty()) { return nullptr; }

  bool const has_residual = residual_mask != nullptr;
  survivor_pass_inputs const inputs{input,
                                    output_positions,
                                    probe_position,
                                    entries,
                                    gate,
                                    observed_filter_count,
                                    device_id,
                                    stream,
                                    mr};
  std::optional<survivor_pass> pass;
  if (kernel == sirius::op::dynamic_filter_mask_kernel::fused) {
    pass = gather_fused(inputs, residual_mask);
    if (!pass) {
      SIRIUS_LOG_DEBUG(
        "[gather_view_survivors] device={} a membership filter has no fused probe; applying the "
        "cascade.",
        device_id);
    }
  }
  if (!pass) { pass = gather_cascade(inputs, residual_mask); }
  if (!pass->survivors) { return nullptr; }
  auto& survivors = pass->survivors;

  // Gate bookkeeping, identical for both kernels: marginal ratios chain through the applied
  // masks in cascade order; the scan-level ratio covers every applied mask.
  auto const rows_into_masks  = pass->rows_into_masks;
  cudf::size_type rows_before = rows_into_masks;
  std::size_t masks_applied   = 0;
  if (applied) {
    applied->applied.clear();
    applied->observed_filter_count = observed_filter_count;
  }
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (!pass->rows_after[i]) { continue; }
    auto const rows_after = *pass->rows_after[i];
    // An empty input has no marginal ratio; the cascade stops measuring there too.
    if (rows_before > 0 && gate && !entries[i].recorded) {
      gate->record_filter_keep_ratio(
        entries[i].identity,
        static_cast<double>(rows_after) / static_cast<double>(rows_before),
        observed_filter_count);
    }
    rows_before = rows_after;
    ++masks_applied;
    if (applied) { applied->applied.push_back(entries[i].identity); }
  }
  if (masks_applied > 0 && gate) {
    gate->record_keep_ratio(static_cast<std::size_t>(rows_into_masks),
                            static_cast<std::size_t>(survivors->num_rows()),
                            observed_filter_count);
  }
  SIRIUS_LOG_DEBUG(
    "[gather_view_survivors] device={} kernel={} residual={} membership_masks={} rows: {} -> {} "
    "-> {}.",
    device_id,
    sirius::op::to_string(kernel),
    has_residual,
    masks_applied,
    num_rows,
    rows_into_masks,
    survivors->num_rows());
  return std::move(survivors);
}

std::optional<double> dynamic_filter_gate::filter_keep_ratio(
  sirius::op::sirius_dynamic_filter const* filter, std::size_t observed_filter_count) const
{
  std::scoped_lock lock(_filter_ratios_mu);
  auto it = _filter_keep_ratios.find(filter);
  if (it == _filter_keep_ratios.end()) { return std::nullopt; }
  // Skipping an optional filter cannot affect correctness, so its verdict is permanent.
  if (filter_skippable(it->second.kept)) { return it->second.kept; }
  // New filters can change this filter's marginal selectivity.
  if (it->second.observed_filter_count < observed_filter_count) { return std::nullopt; }
  return it->second.kept;
}

void dynamic_filter_gate::record_filter_keep_ratio(sirius::op::sirius_dynamic_filter const* filter,
                                                   double kept,
                                                   std::size_t observed_filter_count)
{
  std::scoped_lock lock(_filter_ratios_mu);
  auto const it = _filter_keep_ratios.find(filter);
  if (it != _filter_keep_ratios.end() &&
      it->second.observed_filter_count >= observed_filter_count) {
    return;
  }
  _filter_keep_ratios.insert_or_assign(
    filter, filter_measurement{.kept = kept, .observed_filter_count = observed_filter_count});
  if (filter_skippable(kept)) {
    SIRIUS_LOG_DEBUG(
      "[apply_dynamic_filters] per-filter gate: marginal kept {:.3f} against {} filters -> SKIP "
      "filter permanently.",
      kept,
      observed_filter_count);
  }
}

bool dynamic_filter_gate::applicable(sirius::op::sirius_dynamic_filter_set const& filters) const
{
  if (!filters.has_filters()) { return false; }
  if (_state.load(std::memory_order_relaxed) != state::disabled) { return true; }
  return filters.filter_count() > _decided_filter_count.load(std::memory_order_relaxed);
}

void dynamic_filter_gate::record_keep_ratio(std::size_t rows_before,
                                            std::size_t rows_after,
                                            std::size_t observed_filter_count)
{
  if (rows_before == 0) { return; }

  std::scoped_lock decision_lock(_decision_mu);
  auto const current = _state.load(std::memory_order_relaxed);
  if (current == state::active) { return; }
  if (current == state::disabled &&
      observed_filter_count <= _decided_filter_count.load(std::memory_order_relaxed)) {
    return;  // same filters the disabling batch saw — no new information
  }
  auto const kept = static_cast<double>(rows_after) / static_cast<double>(rows_before);
  _decided_filter_count.store(observed_filter_count, std::memory_order_relaxed);
  _state.store(kept > _keep_threshold ? state::disabled : state::active, std::memory_order_relaxed);
  SIRIUS_LOG_DEBUG("[apply_dynamic_filters] selectivity gate: kept {:.3f} ({} filters) -> {}.",
                   kept,
                   observed_filter_count,
                   kept > _keep_threshold ? "DISABLED" : "ACTIVE");
}

std::unique_ptr<cudf::table> apply_dynamic_filters_gated_view(
  cudf::table_view const& input,
  sirius::op::sirius_dynamic_filter_set const& filters,
  dynamic_filter_gate& gate,
  rmm::cuda_stream_view stream,
  dynamic_filter_apply_mode mode,
  int device_id,
  std::span<sirius::op::sirius_dynamic_filter const* const> already_applied)
{
  if (!gate.applicable(filters)) { return nullptr; }
  // Attribute the result to the channel-size snapshot used to start this apply.
  auto const observed_filters = filters.filter_count();
  auto const rows_before      = input.num_rows();
  auto filtered =
    apply_dynamic_filters_to_view(input, filters, stream, mode, &gate, device_id, already_applied);
  if (!filtered) { return nullptr; }
  gate.record_keep_ratio(
    rows_before, static_cast<std::size_t>(filtered->num_rows()), observed_filters);
  return filtered;
}

}  // namespace sirius::op::scan
