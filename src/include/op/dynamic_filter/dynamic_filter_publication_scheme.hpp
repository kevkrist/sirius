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
 * @brief How an accumulated multi-partition Bloom is reduced onto its root GPU and replicated
 *
 * Both schemes are root-centric (root = the final contributor's GPU) and produce identical filters.
 */
enum class dynamic_filter_publication_scheme : std::uint8_t {
  /// Pull every remote partial into the root in 4 MiB chunks alternating copy and OR on one root
  /// stream, drain it, then copy the full filter to every target and wait per target. Ingress and
  /// egress never overlap.
  root_serial,
  /// Chunk-major pipeline: per chunk, every remote partial slice is pulled into a double-buffered
  /// root scratch on a copy stream, OR-ed on a second root stream, and pulled by every target as
  /// soon as that chunk's OR event fires. Ingress, OR and egress overlap on the full-duplex root
  /// link; one host wait per target. Requires direct peer DMA on every (target, root) pair and
  /// otherwise falls back to `root_serial`.
  root_pipelined,
};

[[nodiscard]] constexpr std::string_view to_string(
  dynamic_filter_publication_scheme scheme) noexcept
{
  switch (scheme) {
    case dynamic_filter_publication_scheme::root_serial: return "root_serial";
    case dynamic_filter_publication_scheme::root_pipelined: return "root_pipelined";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::optional<dynamic_filter_publication_scheme>
parse_dynamic_filter_publication_scheme(std::string_view text) noexcept
{
  if (text == "root_serial") { return dynamic_filter_publication_scheme::root_serial; }
  if (text == "root_pipelined") { return dynamic_filter_publication_scheme::root_pipelined; }
  return std::nullopt;
}

}  // namespace sirius::op
