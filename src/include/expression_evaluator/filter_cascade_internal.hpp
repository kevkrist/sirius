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

#pragma once

// Internal declaration shared between filter_cascade.cpp and its unit tests, following the
// gpu_expression_translator_internal.hpp precedent: the classifier is an implementation detail
// of sirius::expression_evaluator::try_select_cascade, exposed here so tests can exercise each
// classification arm directly.

// sirius
#include <expression/ast/node.hpp>  // sirius::ast::node

// cudf
#include <cudf/table/table_view.hpp>

// standard library
#include <cstdint>

namespace sirius::detail {

/**
 * @brief Eligibility class for one top-level filter-cascade conjunct.
 */
enum class filter_cascade_conjunct_class : std::uint8_t {
  cheap,                     ///< Safe fixed-width prefilter work.
  cascade_worthy_expensive,  ///< Safe, measured-expensive string-carried residual work.
  refuse,                    ///< Keep the original monolithic expression and evaluation order.
};

/**
 * @brief Classifies one conjunct of a filter's top-level AND for the filter cascade in
 * sirius::expression_evaluator::select()
 *
 * `cheap` means an elementwise fixed-width-carried expression. `cascade_worthy_expensive` is the
 * deliberately narrow measured subset: direct string-carried comparisons, BETWEEN, and IN-list
 * predicates, plus safe logical composition around them. `refuse` covers casts, functions,
 * CASE, COALESCE, TRY, aggregates, malformed trees, unsupported carriers, and future AST nodes.
 * A caller must abandon the entire cascade when any top-level conjunct returns `refuse`; these
 * expressions can throw, be observable, or merely be cheap AST breakers for which the extra
 * cascade launches have no demonstrated benefit.
 *
 * The runtime carrier decides because cost lives in the materialized column, not the declared
 * type: under compressed materialization a reference's declared type and physical carrier differ,
 * and decode-time predicate substitution turns a string filter into a BOOL8 mask reference —
 * fixed-width, hence cheap. A STRING carrier can feed the whitelisted expensive predicate shapes;
 * every other non-fixed-width carrier refuses the optimization.
 *
 * Reordering is only row-set preserving for expressions whose evaluation has no observable
 * behavior beyond its value. The conservative `refuse` default is therefore a correctness gate,
 * not just a cost choice. New node alternatives remain refused until their safety and benefit are
 * established explicitly.
 *
 * @param n     Root of the conjunct's AST subtree
 * @param input Table the filter runs over; supplies each ast::reference's bounds check and
 *              runtime carrier type
 * @return The conjunct's cascade eligibility class
 */
[[nodiscard]] filter_cascade_conjunct_class classify_filter_cascade_conjunct(
  sirius::ast::node const& n, cudf::table_view const& input);

}  // namespace sirius::detail
