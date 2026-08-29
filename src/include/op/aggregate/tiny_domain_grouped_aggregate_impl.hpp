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

#include <cudf/aggregation.hpp>
#include <cudf/table/table.hpp>
#include <cudf/table/table_view.hpp>

#include <rmm/cuda_stream_view.hpp>
#include <rmm/resource_ref.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace sirius::op {

/** Result of attempting the bounded tiny-domain local GROUP BY path. */
struct tiny_domain_grouped_aggregate_attempt {
  std::unique_ptr<cudf::table> table;
  std::string fallback_reason;
  std::size_t num_groups          = 0;
  bool register_private_attempted = false;
  bool used_register_private      = false;
  bool used_sampled_preflight     = false;

  [[nodiscard]] explicit operator bool() const noexcept { return table != nullptr; }
};

/**
 * Attempt an exact local GROUP BY for one or two byte-packable keys and at most 64 groups.
 *
 * The function returns a reason and no table when preflight key shape, carrier type, or group count
 * is outside the bounded strategy; the caller may then run ordinary cuDF groupby over the original
 * input. The exact Q1 specialization may launch after a sampled prefix; a later unseen key or
 * invalid one-byte STRING shape synchronizes, discards speculative output, and returns the same
 * fallback contract. Allocation and CUDA failures propagate. Arithmetic overflow or any invalid
 * post-launch state outside those expected sampled-key conditions throws because falling back after
 * a partial custom reduction could hide an implementation bug. On success, output columns are
 * group keys followed by aggregate carriers in exactly the order of @p aggregates.
 */
[[nodiscard]] tiny_domain_grouped_aggregate_attempt try_tiny_domain_grouped_aggregate(
  cudf::table_view input,
  std::vector<int> const& group_idx,
  std::vector<cudf::aggregation::Kind> const& aggregates,
  std::vector<int> const& aggregate_idx,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr);

}  // namespace sirius::op
