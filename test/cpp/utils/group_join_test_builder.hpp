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

#include "helper/logical_type.hpp"
#include "op/sirius_physical_group_join.hpp"
#include "op/sirius_physical_operator.hpp"

#include <duckdb/common/helper.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace sirius::test {

inline op::groupjoin::group_join_spec make_count_group_join_spec(
  std::size_t preserved_key_idx,
  std::size_t counted_key_idx,
  std::optional<std::size_t> counted_value_idx,
  std::uint64_t max_state_bytes)
{
  op::groupjoin::group_join_spec spec;
  spec.form              = op::groupjoin::join_form::OUTER_PRESERVING;
  spec.preserved_key_idx = preserved_key_idx;
  spec.counted_key_idx   = counted_key_idx;
  spec.slots.push_back(op::groupjoin::slot_spec{
    counted_value_idx ? op::groupjoin::agg_op::COUNT_VALID : op::groupjoin::agg_op::COUNT_STAR,
    counted_value_idx,
    sirius::logical_type::make(type_id::BIGINT)});
  spec.max_state_bytes = max_state_bytes;
  return spec;
}

/// Spec for the INNER/DIRECT forms; @p arg_idx is required for every op except COUNT_STAR.
inline op::groupjoin::group_join_spec make_group_join_spec(op::groupjoin::join_form form,
                                                           op::groupjoin::agg_op op,
                                                           std::size_t preserved_key_idx,
                                                           std::size_t counted_key_idx,
                                                           std::optional<std::size_t> arg_idx,
                                                           sirius::logical_type output_type,
                                                           std::uint64_t max_state_bytes)
{
  op::groupjoin::group_join_spec spec;
  spec.form              = form;
  spec.preserved_key_idx = preserved_key_idx;
  spec.counted_key_idx   = counted_key_idx;
  spec.slots.push_back(op::groupjoin::slot_spec{op, arg_idx, std::move(output_type)});
  spec.max_state_bytes = max_state_bytes;
  return spec;
}

inline duckdb::unique_ptr<op::sirius_physical_group_join> make_group_join(
  std::size_t preserved_key_idx,
  std::size_t counted_key_idx,
  std::optional<std::size_t> counted_value_idx,
  duckdb::unique_ptr<op::sirius_physical_operator> preserved,
  duckdb::unique_ptr<op::sirius_physical_operator> counted)
{
  duckdb::vector<sirius::logical_type> output_types;
  output_types.push_back(sirius::logical_type::make(sirius::type_id::INTEGER));
  output_types.push_back(sirius::logical_type::make(sirius::type_id::BIGINT));
  auto join = duckdb::make_uniq<op::sirius_physical_group_join>(
    std::move(output_types),
    /*estimated_cardinality=*/1,
    make_count_group_join_spec(
      preserved_key_idx, counted_key_idx, counted_value_idx, std::uint64_t{1} << 20));
  join->children.push_back(std::move(preserved));
  join->children.push_back(std::move(counted));
  return join;
}

}  // namespace sirius::test
