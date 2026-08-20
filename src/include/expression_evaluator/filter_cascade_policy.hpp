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

#include <cstdint>

namespace sirius {

/**
 * @brief Immutable policy snapshot for one filter evaluator or scan.
 *
 * DuckDB extension settings are connection-local. Planning copies their current values into this
 * object so worker threads never consult mutable process state and every batch in a planned
 * operator observes one coherent policy.
 */
struct filter_cascade_policy {
  /// Whether the cheap-conjunct cascade may be considered.
  bool enabled = false;

  /// Minimum non-empty input size at which the fixed cascade overhead can be worthwhile.
  std::uint64_t min_rows = 1ULL << 20;

  /// Gather survivors only when the cheap-prefilter pass rate is at most this value.
  double max_pass_rate = 0.75;
};

/**
 * @brief Default policy for planned filters.
 *
 * Direct evaluator users remain opted out unless they explicitly pass this value; planner and
 * scan boundaries use it when the session has not overridden the setting.
 *
 * @return Default policy for planned filters and scans
 */
[[nodiscard]] constexpr filter_cascade_policy default_filter_cascade_policy() noexcept
{
  return {.enabled = false};
}

}  // namespace sirius
