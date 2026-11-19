// PROTO(op2): throwaway prototype switches, read once from the environment.
#pragma once

#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sirius::op::proto_op2 {

inline bool flag(char const* name)
{
  auto const* value = std::getenv(name);
  return value != nullptr && value[0] == '1';
}

/// SIRIUS_PROTO_OP2_SHADOW=1: at construction begin, do not build a key whose filter every
/// consumer endpoint would skip under the negligible-removed-share rule, judged against the
/// filters already published on its target channels (reliable estimates only).
inline bool shadow_skip()
{
  static bool const on = flag("SIRIUS_PROTO_OP2_SHADOW");
  return on;
}

/// SIRIUS_PROTO_OP2_LATCH=1: a decode that gave compaction up while it carried only part of the
/// requested join filters does not latch the scan as unprofitable.
inline bool latch_fix()
{
  static bool const on = flag("SIRIUS_PROTO_OP2_LATCH");
  return on;
}

/// SIRIUS_PROTO_OP2_ENDPOINT=1: the endpoint's negligible-removed-share rule does not count a
/// stronger filter that this batch's decode already applied.
inline bool endpoint_apply()
{
  static bool const on = flag("SIRIUS_PROTO_OP2_ENDPOINT");
  return on;
}

/// "join:value,join:value" pairs.
inline std::vector<std::pair<std::size_t, std::size_t>> pairs(char const* name)
{
  std::vector<std::pair<std::size_t, std::size_t>> out;
  auto const* value = std::getenv(name);
  if (value == nullptr) { return out; }
  std::string text{value};
  std::size_t pos = 0;
  while (pos < text.size()) {
    auto const end   = text.find(',', pos);
    auto const item  = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    auto const colon = item.find(':');
    if (colon != std::string::npos) {
      out.emplace_back(std::stoull(item.substr(0, colon)), std::stoull(item.substr(colon + 1)));
    }
    if (end == std::string::npos) { break; }
    pos = end + 1;
  }
  return out;
}

/// SIRIUS_PROTO_OP2_SKIP="join:condition,...": oracle skip of one key's filters.
inline bool oracle_skip(std::optional<std::size_t> join, std::size_t condition)
{
  static auto const list = pairs("SIRIUS_PROTO_OP2_SKIP");
  if (!join) { return false; }
  for (auto const& [j, c] : list) {
    if (j == *join && c == condition) { return true; }
  }
  return false;
}

/// SIRIUS_PROTO_OP3_BLOOM_KEYS="join:keys,...": oracle distinct count a join's Bloom keys are
/// sized on.
inline std::size_t oracle_bloom_keys(std::optional<std::size_t> join)
{
  static auto const list = pairs("SIRIUS_PROTO_OP3_BLOOM_KEYS");
  if (!join) { return 0; }
  for (auto const& [j, keys] : list) {
    if (j == *join) { return keys; }
  }
  return 0;
}

}  // namespace sirius::op::proto_op2
