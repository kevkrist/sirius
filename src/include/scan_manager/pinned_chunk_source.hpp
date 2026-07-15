// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace sirius::scan_manager {

/// Which arm of a MIXED pinned entry holds a given logical chunk.
enum class chunk_kind : std::uint8_t { raw, compressed };

/// One logical chunk's location within a mixed entry: which arm holds it, and
/// its dense index in that arm. Present only for mixed entries; a homogeneous
/// entry leaves pinned_entry::logical_order empty and serves a single arm. A
/// mixed entry's order is valid only when it is a one-to-one cover of every
/// dense slot in both arms.
struct chunk_source {
  chunk_kind kind{chunk_kind::raw};
  std::size_t arm_index{0};
};

}  // namespace sirius::scan_manager
