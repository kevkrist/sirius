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
#include <optional>
#include <string_view>

namespace sirius::op {

/**
 * @brief How a scan folds its residual row filter and the membership dynamic filters into the
 *        survivor mask of a split
 *
 * Both kernels produce the same mask, the same per-filter marginal keep ratios and the same
 * scan-level ratio; they differ only in the number of passes over the split.
 */
enum class dynamic_filter_mask_kernel : std::uint8_t {
  /// One probe kernel per membership filter, each stencilled by the running conjunction, and a
  /// streaming AND + count pass after the residual and after every mask (2 + 2K passes for K
  /// filters, K intermediate mask columns).
  cascade,
  /// One kernel per split: every row is passed through the residual and the membership probes in
  /// the gate's order with early-out, writing the conjunction and the per-step survivor counts in
  /// a single pass (no intermediate mask columns). A lone membership filter without a residual
  /// keeps the plain probe kernel and takes its count from the survivor gather.
  fused,
};

[[nodiscard]] constexpr std::string_view to_string(dynamic_filter_mask_kernel kernel) noexcept
{
  switch (kernel) {
    case dynamic_filter_mask_kernel::cascade: return "cascade";
    case dynamic_filter_mask_kernel::fused: return "fused";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::optional<dynamic_filter_mask_kernel> parse_dynamic_filter_mask_kernel(
  std::string_view text) noexcept
{
  if (text == "cascade") { return dynamic_filter_mask_kernel::cascade; }
  if (text == "fused") { return dynamic_filter_mask_kernel::fused; }
  return std::nullopt;
}

}  // namespace sirius::op
