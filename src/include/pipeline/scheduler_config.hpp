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

#include <string>
#include <string_view>
#include <unordered_map>

namespace sirius::pipeline {

/**
 * @brief When a BUILD_PROBE hash join lets the task creator walk into its probe side
 *
 * - on_build_deposited: only once every partition's folded build batch sits in the join's build
 *   repository (the probe scan therefore starts after the whole build CONCAT shuffle).
 * - on_partitioned_and_published: as soon as the build PARTITION pipeline has finished AND every
 *   producer registered on each dynamic-filter channel the join publishes into has reached a
 *   terminal publication state (published, failed or closed). The probe scan then overlaps the
 *   build CONCAT shuffle and the hash-table builds while still seeing every filter it was planned
 *   to consume. Joins that publish no dynamic filter keep the on_build_deposited rule, since the
 *   build landing is the only throttle on an unfiltered probe.
 */
enum class probe_activation_policy { on_build_deposited, on_partitioned_and_published };

/// ADL-discoverable string conversion so yaml_reader can parse probe_activation_policy values.
inline bool string_to_enum(std::string_view sv, probe_activation_policy& out)
{
  static const std::unordered_map<std::string_view, probe_activation_policy> map = {
    {"on_build_deposited", probe_activation_policy::on_build_deposited},
    {"on_partitioned_and_published", probe_activation_policy::on_partitioned_and_published},
  };
  auto it = map.find(sv);
  if (it == map.end()) { return false; }
  out = it->second;
  return true;
}

inline bool enum_to_string(probe_activation_policy policy, std::string& s)
{
  switch (policy) {
    case probe_activation_policy::on_build_deposited: s = "on_build_deposited"; return true;
    case probe_activation_policy::on_partitioned_and_published:
      s = "on_partitioned_and_published";
      return true;
    default: return false;
  }
}

/// Scheduling policy read from `sirius.scheduler`.
struct scheduler_config {
  /// See probe_activation_policy.
  probe_activation_policy probe_activation = probe_activation_policy::on_partitioned_and_published;
};

}  // namespace sirius::pipeline
