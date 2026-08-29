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

//! @file
//! Sub-batch orchestration helpers for the DuckDB-native scan decoder's read -> host-to-device
//! overlap (see `submit_reads_and_stage_h2d` in `duckdb_native_decoder.cpp`). The decoder
//! partitions a split's coalesced file ranges ("pieces") into sub-batches, issues every
//! sub-batch's asynchronous read up front, and as each sub-batch completes enqueues its
//! host-to-device copies -- so the GPU uploads earlier sub-batches while the io service is still
//! reading later ones. These helpers hold the pure partitioning logic and the failure-safety
//! protocol of that loop, factored out so both are unit-testable without a GPU or an io service.

#include <cstddef>
#include <span>
#include <vector>

namespace sirius::op::scan {

/// A half-open range of pieces `[piece_begin, piece_end)` forming one IO sub-batch, with the
/// total file bytes those pieces cover.
struct read_sub_batch {
  std::size_t piece_begin = 0;
  std::size_t piece_end   = 0;
  std::size_t bytes       = 0;
};

/// @brief Partition pieces (in file order) into sub-batches of at least @p target_bytes each.
///
/// A sub-batch closes as soon as it reaches @p target_bytes, so every sub-batch except possibly
/// the last meets the target; the last takes whatever remains. Pieces are never split -- a piece
/// is the unit a coalesced read lands in, so splitting one would tear a segment across
/// sub-batches. A @p target_bytes of 0 yields one piece per sub-batch; empty input yields no
/// sub-batches.
inline std::vector<read_sub_batch> partition_read_sub_batches(
  std::span<std::size_t const> piece_bytes, std::size_t target_bytes)
{
  std::vector<read_sub_batch> out;
  read_sub_batch current;
  for (std::size_t i = 0; i < piece_bytes.size(); ++i) {
    current.piece_end = i + 1;
    current.bytes += piece_bytes[i];
    if (current.bytes >= target_bytes) {
      out.push_back(current);
      current = {i + 1, i + 1, 0};
    }
  }
  if (current.piece_end > current.piece_begin) { out.push_back(current); }
  return out;
}

/// @brief In-order sub-batch wait loop with the failure-safety protocol the pinned-staging
/// lifetime invariant requires.
///
/// For each sub-batch k in [0, count): `wait(k)` blocks on the sub-batch's read future (throwing
/// on IO failure or a short read), then `on_ready(k)` enqueues its host-to-device copies. If
/// either throws, the loop MUST NOT let the exception unwind while later reads still write into
/// the pinned staging blocks or earlier copies still read from them -- the blocks would return to
/// the shared pool and be reused mid-flight. So on any failure it first consumes every remaining
/// future via `join_remaining(j)` for j in (k, count) (which must swallow that future's own
/// errors), then runs `fail_safe()` (a stream synchronize covering every issued host-to-device
/// copy), and only then rethrows.
template <typename WaitFn, typename ReadyFn, typename JoinFn, typename FailSafeFn>
void await_read_sub_batches(std::size_t count,
                            WaitFn&& wait,
                            ReadyFn&& on_ready,
                            JoinFn&& join_remaining,
                            FailSafeFn&& fail_safe)
{
  std::size_t k = 0;
  try {
    for (; k < count; ++k) {
      wait(k);
      on_ready(k);
    }
  } catch (...) {
    // Both wait(k) and on_ready(k) run after sub-batch k's future was consumed, so the
    // outstanding futures are exactly (k, count).
    for (std::size_t j = k + 1; j < count; ++j) {
      join_remaining(j);
    }
    fail_safe();
    throw;
  }
}

}  // namespace sirius::op::scan
