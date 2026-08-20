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

// Cheap-conjunct filter cascade policy for sirius::expression_evaluator::select().
//
// This translation unit holds the policy of the cascade — the conjunct classifier and the
// decision procedure in try_select_cascade — following the house pattern of defining one class's
// concern-specific members across dedicated files (see specializations/*.cpp). All mechanism
// (compute_mask, cudf::apply_boolean_mask, cudf::bools_to_mask, NULL_LOGICAL_AND) is the
// existing evaluator/cuDF machinery, reused unmodified; expression_evaluator.cpp keeps the
// baseline select path.
//
// Correctness rests on Kleene partition invariance: for a top-level AND with children C and any
// partition C = A + B, AND(C) is TRUE for a row iff AND(A) and AND(B) are both TRUE, because
// Kleene AND is associative/commutative and a Kleene conjunction is TRUE iff all operands are
// TRUE (a NULL or FALSE anywhere makes it non-TRUE). cudf::apply_boolean_mask keeps exactly the
// valid-and-TRUE rows and NULL_LOGICAL_AND is Kleene AND (matching the lowering in
// specializations/conjunction.cpp), so every route below — cascaded, combined_masks,
// short_circuited, and the caller's monolithic fallback — produces the identical row set. In
// particular, a row where the cheap group evaluates to NULL is correctly dropped before the
// residual ever runs (TRUE AND NULL != TRUE), and a row where the cheap group is TRUE but the
// residual is NULL is dropped by the residual stage; the cascade therefore needs no
// null-handling code of its own. Row-set equivalence alone does not permit reordering expressions
// that can throw or otherwise be observable: the classifier only admits a narrow safe subset and
// refuses the whole cascade when a conjunct is fallible, unknown, or lacks measured residual
// benefit.

// sirius
#include <expression/ast/node.hpp>   // sirius::ast::node alternatives
#include <expression/ast/utils.hpp>  // sirius::ast::clone, sirius::ast::visit_references
#include <expression_evaluator/expression_evaluator.hpp>
#include <expression_evaluator/filter_cascade_internal.hpp>
#include <log/logging.hpp>

// cudf
#include <cudf/binaryop.hpp>
#include <cudf/copying.hpp>  // cudf::empty_like
#include <cudf/stream_compaction.hpp>
#include <cudf/strings/strings_column_view.hpp>
#include <cudf/table/table_view.hpp>
#include <cudf/transform.hpp>  // cudf::bools_to_mask
#include <cudf/types.hpp>
#include <cudf/utilities/traits.hpp>  // cudf::is_fixed_width

// standard library
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace sirius::detail {

// Contract and the conservative default arm are documented on the declaration in
// filter_cascade_internal.hpp.
namespace {

using conjunct_class = filter_cascade_conjunct_class;

constexpr conjunct_class matching_operand_classes(conjunct_class lhs, conjunct_class rhs) noexcept
{
  return lhs == rhs ? lhs : conjunct_class::refuse;
}

}  // namespace

filter_cascade_conjunct_class classify_filter_cascade_conjunct(sirius::ast::node const& n,
                                                               cudf::table_view const& input)
{
  namespace ast = sirius::ast;
  return std::visit(
    [&](auto const& alt) -> conjunct_class {
      using T = std::decay_t<decltype(alt)>;
      if constexpr (std::is_same_v<T, ast::reference>) {
        if (alt.column_index >= static_cast<uint32_t>(input.num_columns())) {
          return conjunct_class::refuse;
        }
        auto const type = input.column(static_cast<cudf::size_type>(alt.column_index)).type();
        if (cudf::is_fixed_width(type)) { return conjunct_class::cheap; }
        return type.id() == cudf::type_id::STRING ? conjunct_class::cascade_worthy_expensive
                                                  : conjunct_class::refuse;
      } else if constexpr (std::is_same_v<T, ast::constant>) {
        if (alt.return_type().is_varchar()) { return conjunct_class::cascade_worthy_expensive; }
        return alt.return_type().is_fixed_width() ? conjunct_class::cheap : conjunct_class::refuse;
      } else if constexpr (std::is_same_v<T, ast::comparison>) {
        if (!alt.left || !alt.right) { return conjunct_class::refuse; }
        return matching_operand_classes(classify_filter_cascade_conjunct(*alt.left, input),
                                        classify_filter_cascade_conjunct(*alt.right, input));
      } else if constexpr (std::is_same_v<T, ast::between>) {
        if (!alt.input || !alt.lower || !alt.upper) { return conjunct_class::refuse; }
        auto const value_class = classify_filter_cascade_conjunct(*alt.input, input);
        if (value_class == conjunct_class::refuse ||
            classify_filter_cascade_conjunct(*alt.lower, input) != value_class ||
            classify_filter_cascade_conjunct(*alt.upper, input) != value_class) {
          return conjunct_class::refuse;
        }
        return value_class;
      } else if constexpr (std::is_same_v<T, ast::conjunction>) {
        if ((alt.op != ast::conjunction::kind::op_and && alt.op != ast::conjunction::kind::op_or) ||
            alt.children.empty()) {
          return conjunct_class::refuse;
        }
        auto result = conjunct_class::cheap;
        for (auto const& child : alt.children) {
          if (!child) { return conjunct_class::refuse; }
          auto const child_class = classify_filter_cascade_conjunct(*child, input);
          if (child_class == conjunct_class::refuse) { return conjunct_class::refuse; }
          if (child_class == conjunct_class::cascade_worthy_expensive) {
            result = conjunct_class::cascade_worthy_expensive;
          }
        }
        return result;
      } else if constexpr (std::is_same_v<T, ast::in_list>) {
        if (!alt.probe || alt.values.empty()) { return conjunct_class::refuse; }
        auto const probe_class = classify_filter_cascade_conjunct(*alt.probe, input);
        if (probe_class == conjunct_class::refuse) { return conjunct_class::refuse; }
        for (auto const& value : alt.values) {
          if (!value || classify_filter_cascade_conjunct(*value, input) != probe_class) {
            return conjunct_class::refuse;
          }
        }
        return probe_class;
      } else if constexpr (std::is_same_v<T, ast::unary_op>) {
        if (!alt.child) { return conjunct_class::refuse; }
        auto const child_class = classify_filter_cascade_conjunct(*alt.child, input);
        switch (alt.op) {
          case ast::unary_op::kind::op_not: return child_class;
          case ast::unary_op::kind::op_is_null:
          case ast::unary_op::kind::op_is_not_null:
            return child_class == conjunct_class::cheap ? conjunct_class::cheap
                                                        : conjunct_class::refuse;
          default: return conjunct_class::refuse;
        }
      } else {
        // Casts and functions may throw; the remaining AST breakers are unmeasured. New
        // alternatives stay refused until both safety and cascade benefit are established.
        return conjunct_class::refuse;
      }
    },
    n.v);
}

}  // namespace sirius::detail

namespace sirius {

// Sub-evaluators produce, per row, the same value a monolithic evaluation would: every
// specialization's lowering decisions (restored-reference casts, narrow-domain comparisons)
// depend only on the conjunct's own operands and carrier types, never on sibling conjuncts.
// Future specializations must preserve this context-free property; the byte-equivalence tests in
// test_filter_cascade.cpp are its regression net.
std::optional<std::unique_ptr<cudf::table>> expression_evaluator::try_select_cascade(
  cudf::table_view input, std::span<cudf::size_type const> output_indices)
{
  namespace ast = sirius::ast;
  // The owning operator captured this immutable session/query policy before execution.
  if (!_filter_cascade_policy.enabled) { return std::nullopt; }
  auto const min_rows      = _filter_cascade_policy.min_rows;
  auto const max_pass_rate = _filter_cascade_policy.max_pass_rate;
  auto const num_rows      = input.num_rows();
  if (num_rows <= 0 || static_cast<std::uint64_t>(num_rows) < min_rows) { return std::nullopt; }
  if (_ast_expressions.size() != 1 || _ast_expressions[0] == nullptr) { return std::nullopt; }
  auto const& root = *_ast_expressions[0];
  if (!root.holds<ast::conjunction>()) { return std::nullopt; }
  auto const& conj = root.get<ast::conjunction>();
  if (conj.op != ast::conjunction::kind::op_and) { return std::nullopt; }

  using conjunct_class = detail::filter_cascade_conjunct_class;
  std::vector<ast::node const*> cheap;
  std::vector<ast::node const*> expensive;
  // Only the explicitly safe classes may be reordered. A fallible, observable, malformed, or
  // merely unmeasured conjunct refuses the whole optimization so the ordinary path preserves its
  // evaluation behavior.
  for (auto const& child : conj.children) {
    if (!child) { return std::nullopt; }  // malformed AST: let the ordinary path report it
    auto const classification = detail::classify_filter_cascade_conjunct(*child, input);
    if (classification == conjunct_class::refuse ||
        child->return_type().id() != sirius::type_id::BOOLEAN) {
      return std::nullopt;
    }
    switch (classification) {
      case conjunct_class::cheap: cheap.push_back(child.get()); break;
      case conjunct_class::cascade_worthy_expensive: expensive.push_back(child.get()); break;
      case conjunct_class::refuse: return std::nullopt;
    }
  }
  if (cheap.empty() || expensive.empty()) { return std::nullopt; }

  // The survivor gather currently compacts the whole input so residual reference indexes stay
  // unchanged. Refuse wasteful variable-width payloads unless the residual itself needs them, and
  // refuse unrelated omitted columns. A small exception preserves scan/decode composition: cheap
  // predicates commonly consume one non-projected INT32/INT64 carrier. Gathering at most one
  // 64-bit lane per row is bounded and avoids remapping the residual AST. Independently cap all
  // fixed-width lanes in the gathered input at 32 bytes per row: the pass-rate break-even assumes
  // a roughly 50-byte row, so wider projected payloads need a byte-aware cost model rather than
  // this row-rate heuristic.
  auto const num_columns = static_cast<std::size_t>(input.num_columns());
  std::vector<bool> projected(num_columns, output_indices.empty());
  std::vector<bool> residual_referenced(num_columns, false);
  std::vector<bool> cheap_referenced(num_columns, false);
  for (auto const output_index : output_indices) {
    if (output_index >= 0 && output_index < input.num_columns()) {
      projected[static_cast<std::size_t>(output_index)] = true;
    }
  }
  auto mark_references = [&](std::vector<ast::node const*> const& conjuncts,
                             std::vector<bool>& referenced) {
    for (auto const* conjunct : conjuncts) {
      ast::visit_references(*conjunct, [&](ast::reference const& ref) {
        if (ref.column_index < static_cast<uint32_t>(input.num_columns())) {
          referenced[ref.column_index] = true;
        }
      });
    }
  };
  mark_references(expensive, residual_referenced);
  mark_references(cheap, cheap_referenced);

  constexpr std::size_t max_fixed_width_gather_bytes_per_row = 32;
  constexpr std::size_t max_cheap_only_gather_bytes_per_row  = sizeof(std::uint64_t);
  std::size_t fixed_width_gather_bytes_per_row               = 0;
  std::size_t cheap_only_gather_bytes_per_row                = 0;
  for (std::size_t column_index = 0; column_index < num_columns; ++column_index) {
    auto const type        = input.column(static_cast<cudf::size_type>(column_index)).type();
    auto const fixed_width = cudf::is_fixed_width(type);
    std::size_t width      = 0;
    if (fixed_width) {
      width = static_cast<std::size_t>(cudf::size_of(type));
      if (width > max_fixed_width_gather_bytes_per_row - fixed_width_gather_bytes_per_row) {
        return std::nullopt;
      }
      fixed_width_gather_bytes_per_row += width;
    }

    if (!fixed_width && !residual_referenced[column_index]) { return std::nullopt; }
    if (projected[column_index] || residual_referenced[column_index]) { continue; }
    if (!cheap_referenced[column_index] || !fixed_width) { return std::nullopt; }

    if (width > max_cheap_only_gather_bytes_per_row - cheap_only_gather_bytes_per_row) {
      return std::nullopt;
    }
    cheap_only_gather_bytes_per_row += width;
  }

  // From here the cascade is committed: once the cheap mask and its survivor count exist, every
  // outcome below is cheaper than restarting on the monolithic path.

  // A single conjunct is borrowed in place (owned by the operator's expression, alive across
  // this call); a multi-conjunct group needs an owning AND wrapper, so its members are
  // deep-cloned (predicate trees are tiny — a few host allocations against a multi-million-row
  // kernel).
  auto make_group = [](std::vector<ast::node const*> const& parts)
    -> std::pair<ast::node const*, std::unique_ptr<ast::node>> {
    if (parts.size() == 1) { return {parts.front(), nullptr}; }
    ast::conjunction group;
    group.op = ast::conjunction::kind::op_and;
    group.children.reserve(parts.size());
    for (auto const* part : parts) {
      group.children.push_back(ast::clone(*part));
    }
    auto owned      = std::make_unique<ast::node>(std::move(group));
    auto const* ptr = owned.get();
    return {ptr, std::move(owned)};
  };

  auto const [cheap_root, cheap_owned] = make_group(cheap);
  expression_evaluator cheap_evaluator(
    cheap_root, _mr, _stream, _strategy, _min_ast_size, filter_cascade_policy{});
  auto cheap_mask = cheap_evaluator.compute_mask(input);

  // Survivor count: bools_to_mask reports how many entries are false-or-null, which is exactly
  // the set apply_boolean_mask would drop, so passed = num_rows - dropped. The transient bitmask
  // (num_rows/8 bytes) is discarded immediately. The count is the cascade's one host-blocking
  // 4-byte device-to-host sync — the price of the adaptive decision.
  cudf::size_type dropped = 0;
  {
    auto const mask_and_count = cudf::bools_to_mask(cheap_mask->view(), _stream, _mr);
    dropped                   = mask_and_count.second;
  }
  auto const passed    = num_rows - dropped;
  auto const pass_rate = static_cast<double>(passed) / static_cast<double>(num_rows);

  // The engaged path is the hottest filter route in the engine, so per-batch detail stays at
  // DEBUG; one INFO line per process on first engagement keeps activation evidence in every
  // run's log without per-batch volume.
  static std::atomic<bool> activation_logged{false};
  if (!activation_logged.exchange(true, std::memory_order_relaxed)) {
    SIRIUS_LOG_INFO(
      "[expression_evaluator] filter cascade engaged (first activation in this process): "
      "{} of {} rows (rate {:.3f}) pass the cheap prefilter ({} cheap / {} expensive "
      "conjuncts); per-batch decisions log at debug level",
      passed,
      num_rows,
      pass_rate,
      cheap.size(),
      expensive.size());
  }

  auto project = [&](cudf::table_view t) {
    return output_indices.empty() ? t : t.select(output_indices.begin(), output_indices.end());
  };

  if (passed == 0) {
    // Nothing survives the cheap prefilter, so skipping the residual is legal (a non-TRUE cheap
    // group already makes the whole AND non-TRUE). cudf::empty_like matches the zero-row
    // structure apply_boolean_mask itself returns for empty inputs.
    _last_filter_cascade_decision = filter_cascade_decision::short_circuited;
    SIRIUS_LOG_DEBUG(
      "[expression_evaluator] filter cascade: cheap prefilter dropped all {} rows "
      "({} cheap / {} expensive conjuncts)",
      num_rows,
      cheap.size(),
      expensive.size());
    return cudf::empty_like(project(input));
  }

  auto const [residual_root, residual_owned] = make_group(expensive);
  expression_evaluator residual_evaluator(
    residual_root, _mr, _stream, _strategy, _min_ast_size, filter_cascade_policy{});

  // A selective row count alone cannot make gathering an arbitrarily large STRING payload safe.
  // Bound the aggregate physical STRING storage that any survivor gather can copy. chars_size()
  // is intentionally deferred until a non-empty, row-selective branch would actually gather, so
  // all-drop and combined-mask batches pay no additional metadata synchronization. Counting the
  // full input's chars plus one offsets entry per row is conservative even when the cheap
  // predicate selects unusually long strings: a survivor gather cannot copy more chars than the
  // source contains, and it produces no more than num_rows + 1 offsets per residual column.
  constexpr std::uint64_t max_residual_string_gather_bytes_per_row = 32;
  auto const residual_string_gather_budget =
    static_cast<std::uint64_t>(num_rows) * max_residual_string_gather_bytes_per_row;
  std::uint64_t residual_string_gather_bytes = 0;
  bool residual_string_gather_within_budget  = true;
  if (pass_rate <= max_pass_rate) {
    for (std::size_t column_index = 0; column_index < num_columns; ++column_index) {
      auto const column = input.column(static_cast<cudf::size_type>(column_index));
      if (!residual_referenced[column_index] || column.type().id() != cudf::type_id::STRING) {
        continue;
      }

      auto const strings    = cudf::strings_column_view{column};
      auto const chars_size = strings.chars_size(_stream);
      auto const offsets_bytes =
        (static_cast<std::uint64_t>(num_rows) + 1) *
        static_cast<std::uint64_t>(cudf::size_of(strings.offsets().type()));
      if (chars_size < 0 || static_cast<std::uint64_t>(chars_size) >
                              residual_string_gather_budget - residual_string_gather_bytes) {
        residual_string_gather_within_budget = false;
        break;
      }
      residual_string_gather_bytes += static_cast<std::uint64_t>(chars_size);
      if (offsets_bytes > residual_string_gather_budget - residual_string_gather_bytes) {
        residual_string_gather_within_budget = false;
        break;
      }
      residual_string_gather_bytes += offsets_bytes;
    }
  }

  if (pass_rate <= max_pass_rate && residual_string_gather_within_budget) {
    // Selective prefilter: compact once, then run the expensive residual only on survivors.
    // apply_boolean_mask is a stable compaction and two stacked stable compactions compose to a
    // stable compaction, so output order equals input order equals the monolithic path's order.
    _last_filter_cascade_decision = filter_cascade_decision::cascaded;
    SIRIUS_LOG_DEBUG(
      "[expression_evaluator] filter cascade: {} of {} rows (rate {:.3f}) pass the cheap "
      "prefilter ({} cheap / {} expensive conjuncts); evaluating the residual on survivors",
      passed,
      num_rows,
      pass_rate,
      cheap.size(),
      expensive.size());
    auto const survivors = cudf::apply_boolean_mask(input, cheap_mask->view(), _stream, _mr);
    cheap_mask.reset();
    auto const residual_mask = residual_evaluator.compute_mask(survivors->view());
    return cudf::apply_boolean_mask(
      project(survivors->view()), residual_mask->view(), _stream, _mr);
  }

  // Either the prefilter is too unselective or its residual STRING payload is too wide for a
  // gather. The cheap mask is a sunk cost at this point, so ANDing it with the residual mask
  // (Kleene, matching the conjunction lowering) is strictly cheaper than recomputing the
  // monolithic predicate. Same row set either way.
  _last_filter_cascade_decision = filter_cascade_decision::combined_masks;
  if (!residual_string_gather_within_budget) {
    SIRIUS_LOG_DEBUG(
      "[expression_evaluator] filter cascade: {} of {} rows (rate {:.3f}) pass the cheap "
      "prefilter ({} cheap / {} expensive conjuncts); residual STRING storage exceeds "
      "{} encoded bytes per input row, combining masks without a gather",
      passed,
      num_rows,
      pass_rate,
      cheap.size(),
      expensive.size(),
      max_residual_string_gather_bytes_per_row);
  } else {
    SIRIUS_LOG_DEBUG(
      "[expression_evaluator] filter cascade: {} of {} rows (rate {:.3f}) pass the cheap "
      "prefilter ({} cheap / {} expensive conjuncts); pass rate above "
      "the immutable policy maximum, combining masks without a gather",
      passed,
      num_rows,
      pass_rate,
      cheap.size(),
      expensive.size());
  }
  auto const residual_mask = residual_evaluator.compute_mask(input);
  auto const combined      = cudf::binary_operation(cheap_mask->view(),
                                               residual_mask->view(),
                                               cudf::binary_operator::NULL_LOGICAL_AND,
                                               cudf::data_type{cudf::type_id::BOOL8},
                                               _stream,
                                               _mr);
  return cudf::apply_boolean_mask(project(input), combined->view(), _stream, _mr);
}

}  // namespace sirius
