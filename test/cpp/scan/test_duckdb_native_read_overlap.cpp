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

// CPU-only tests for the decoder's sub-batched read -> H2D overlap helpers
// (op/scan/duckdb_native_read_overlap.hpp): the piece partitioning that forms
// IO sub-batches, and the failure-safety protocol of the in-order wait loop --
// on a mid-split failure every outstanding read future must be joined and the
// fail-safe (stream synchronize in production) must run BEFORE the exception
// unwinds, or the pinned staging blocks would return to the shared pool while
// reads still write into them and copies still read from them.

#include "exec/semi_future.hpp"
#include "op/scan/duckdb_native_read_overlap.hpp"

#include <catch.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using sirius::op::scan::await_read_sub_batches;
using sirius::op::scan::partition_read_sub_batches;
using sirius::op::scan::read_sub_batch;

TEST_CASE("partition_read_sub_batches - respects piece boundaries and the byte target",
          "[scan][decode][overlap]")
{
  // Pieces close a sub-batch as soon as the target is reached; the tail takes the remainder.
  std::vector<std::size_t> pieces = {100, 100, 50, 300, 10, 10};
  auto const batches              = partition_read_sub_batches(pieces, 200);

  REQUIRE(batches.size() == 3);
  CHECK(batches[0].piece_begin == 0);
  CHECK(batches[0].piece_end == 2);
  CHECK(batches[0].bytes == 200);
  CHECK(batches[1].piece_begin == 2);
  CHECK(batches[1].piece_end == 4);
  CHECK(batches[1].bytes == 350);
  CHECK(batches[2].piece_begin == 4);
  CHECK(batches[2].piece_end == 6);
  CHECK(batches[2].bytes == 20);

  // Every piece lands in exactly one sub-batch, in order.
  std::size_t covered = 0;
  std::size_t bytes   = 0;
  for (auto const& b : batches) {
    REQUIRE(b.piece_begin == covered);
    covered = b.piece_end;
    bytes += b.bytes;
  }
  REQUIRE(covered == pieces.size());
  REQUIRE(bytes == 570);
}

TEST_CASE("partition_read_sub_batches - forced-small sub-batches split at every piece",
          "[scan][decode][overlap]")
{
  // target_bytes = 0 forces one piece per sub-batch -- the shape the event-based
  // release tests use to exercise many in-flight sub-batches.
  std::vector<std::size_t> pieces = {5, 6, 7};
  auto const batches              = partition_read_sub_batches(pieces, 0);
  REQUIRE(batches.size() == 3);
  for (std::size_t i = 0; i < 3; ++i) {
    CHECK(batches[i].piece_begin == i);
    CHECK(batches[i].piece_end == i + 1);
    CHECK(batches[i].bytes == pieces[i]);
  }
}

TEST_CASE("partition_read_sub_batches - single batch and empty input", "[scan][decode][overlap]")
{
  std::vector<std::size_t> pieces = {10, 20, 30};
  auto const one                  = partition_read_sub_batches(pieces, SIZE_MAX);
  REQUIRE(one.size() == 1);
  CHECK(one[0].piece_begin == 0);
  CHECK(one[0].piece_end == 3);
  CHECK(one[0].bytes == 60);

  REQUIRE(partition_read_sub_batches({}, 100).empty());
}

TEST_CASE("await_read_sub_batches - success path waits and enqueues in order",
          "[scan][decode][overlap]")
{
  std::vector<std::string> events;
  await_read_sub_batches(
    3,
    [&](std::size_t k) { events.push_back("wait" + std::to_string(k)); },
    [&](std::size_t k) { events.push_back("ready" + std::to_string(k)); },
    [&](std::size_t k) { events.push_back("join" + std::to_string(k)); },
    [&]() { events.push_back("fail_safe"); });
  REQUIRE(events ==
          std::vector<std::string>{"wait0", "ready0", "wait1", "ready1", "wait2", "ready2"});
}

TEST_CASE(
  "await_read_sub_batches - a mid-split short read joins all in-flight sub-batches "
  "before unwinding",
  "[scan][decode][overlap]")
{
  // Forced short read at sub-batch 1 while sub-batches 2 and 3 are still in flight, modelled
  // with real sirius::exec::semi_future objects the way production consumes them: wait -> get()
  // (short count -> throw), join -> get_try() (swallows even an errored future). The unwind-safety
  // contract: join(2), join(3), then fail_safe, all BEFORE the exception reaches the caller.
  std::vector<std::size_t> const expected_bytes = {100, 100, 100, 100};
  std::vector<sirius::exec::semi_future<std::size_t>> futs;
  futs.push_back(sirius::exec::make_semi_future<std::size_t>(std::size_t{100}));
  futs.push_back(sirius::exec::make_semi_future<std::size_t>(std::size_t{60}));  // short read
  futs.push_back(sirius::exec::make_semi_future<std::size_t>(
    std::make_exception_ptr(std::runtime_error("io error on a later sub-batch"))));
  futs.push_back(sirius::exec::make_semi_future<std::size_t>(std::size_t{100}));

  std::vector<std::string> events;
  bool caught = false;
  try {
    await_read_sub_batches(
      futs.size(),
      [&](std::size_t k) {
        events.push_back("wait" + std::to_string(k));
        auto const got = std::move(futs[k]).get();
        if (got != expected_bytes[k]) {
          throw std::runtime_error("short coalesced host read on sub-batch " + std::to_string(k));
        }
      },
      [&](std::size_t k) { events.push_back("ready" + std::to_string(k)); },
      [&](std::size_t k) {
        events.push_back("join" + std::to_string(k));
        if (futs[k].valid()) { (void)std::move(futs[k]).get_try(); }
      },
      [&]() { events.push_back("fail_safe"); });
  } catch (std::runtime_error const& e) {
    caught = true;
    CHECK(std::string(e.what()).find("short coalesced host read") != std::string::npos);
    // The safety work already happened when the exception reaches the caller.
    CHECK(events ==
          std::vector<std::string>{"wait0", "ready0", "wait1", "join2", "join3", "fail_safe"});
  }
  REQUIRE(caught);

  // Every future was consumed -- none is left to write into released staging.
  for (auto const& f : futs) {
    REQUIRE_FALSE(f.valid());
  }
}

TEST_CASE("await_read_sub_batches - an H2D enqueue failure also joins the remainder",
          "[scan][decode][overlap]")
{
  std::vector<std::string> events;
  bool caught = false;
  try {
    await_read_sub_batches(
      3,
      [&](std::size_t k) { events.push_back("wait" + std::to_string(k)); },
      [&](std::size_t k) {
        events.push_back("ready" + std::to_string(k));
        if (k == 0) { throw std::runtime_error("h2d enqueue failed"); }
      },
      [&](std::size_t k) { events.push_back("join" + std::to_string(k)); },
      [&]() { events.push_back("fail_safe"); });
  } catch (std::runtime_error const&) {
    caught = true;
  }
  REQUIRE(caught);
  REQUIRE(events == std::vector<std::string>{"wait0", "ready0", "join1", "join2", "fail_safe"});
}
