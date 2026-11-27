// PROTOTYPE (throwaway): SIRIUS_PROTO_ONESHOT switch.
//   0 / unset: tip behaviour (construct every dynamic filter at the build PARTITION).
//   1: builds that arrive whole (1 partition or broadcast) construct one-shot from the concatenated
//      build at the join's build port, publish, THEN deposit (probe waits for the filter).
//   2: same, but deposit first (pre-stack order: the probe may start before the filter is published).
#pragma once
#include <cstdlib>
namespace sirius::op {
inline int proto_oneshot_mode() noexcept
{
  static int const mode = [] {
    char const* v = std::getenv("SIRIUS_PROTO_ONESHOT");
    return v ? std::atoi(v) : 0;
  }();
  return mode;
}
}  // namespace sirius::op
