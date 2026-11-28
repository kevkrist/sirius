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

#include "catch.hpp"
#include "memory/peer_islands.hpp"
#include "memory/pool_peer_access.hpp"
#include "operator/operator_test_utils.hpp"
#include "utils/sirius_test_env.hpp"

#include <cuda_runtime_api.h>

#include <cucascade/memory/memory_space.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using sirius::memory::islands_from_access;

/// The islands of `count` devices in which exactly the `linked` pairs have access both ways.
std::vector<std::uint8_t> islands_of(std::size_t count,
                                     std::initializer_list<std::pair<int, int>> linked)
{
  auto const access = std::make_unique<bool[]>(count * count);
  for (auto const [left, right] : linked) {
    access[left * count + right] = true;
    access[right * count + left] = true;
  }
  return islands_from_access(count, {access.get(), count * count});
}
}  // namespace

TEST_CASE("islands_from_access groups devices with mutual access", "[peer_islands]")
{
  using ids = std::vector<std::uint8_t>;
  CHECK(islands_of(0, {}).empty());
  CHECK(islands_of(1, {}) == ids{0});
  CHECK(islands_of(4, {}) == ids{0, 1, 2, 3});
  CHECK(islands_of(4, {{0, 1}, {2, 3}}) == ids{0, 0, 1, 1});
  CHECK(islands_of(4, {{0, 2}, {1, 3}}) == ids{0, 1, 0, 1});
  CHECK(islands_of(3, {{0, 1}}) == ids{0, 0, 1});
  CHECK(islands_of(3, {{1, 2}}) == ids{0, 1, 1});
  CHECK(islands_of(4, {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}}) == ids{0, 0, 0, 0});
  // Not transitive: 0-1 and 1-2 without 0-2. Device 2 may not join {0, 1}.
  CHECK(islands_of(3, {{0, 1}, {1, 2}}) == ids{0, 0, 1});
  // A chain 0-1-2-3 is covered by its first and last link.
  CHECK(islands_of(4, {{0, 1}, {1, 2}, {2, 3}}) == ids{0, 0, 1, 1});
}

TEST_CASE("islands_from_access requires access in both directions", "[peer_islands]")
{
  std::size_t const count = 2;
  bool access[4]          = {false, true, false, false};
  CHECK(islands_from_access(count, access) == std::vector<std::uint8_t>{0, 1});
  access[2] = true;
  CHECK(islands_from_access(count, access) == std::vector<std::uint8_t>{0, 0});
  CHECK_THROWS_AS(islands_from_access(3, access), std::invalid_argument);
}

TEST_CASE("peer_islands agrees with the pool grants of the visible GPUs",
          "[peer_islands][multi_gpu]")
{
  int devices = 0;
  REQUIRE(cudaGetDeviceCount(&devices) == cudaSuccess);
  if (devices < 2) { return; }
  auto manager = sirius::test::operator_utils::initialize_memory_manager(devices);
  std::vector<cucascade::memory::memory_space const*> spaces;
  for (int device = 0; device < devices; ++device) {
    spaces.push_back(manager->get_memory_space(cucascade::memory::Tier::GPU, device));
  }
  auto const ids = sirius::memory::peer_islands(spaces);
  REQUIRE(ids.size() == spaces.size());
  CHECK(ids.front() == 0);
  for (std::size_t left = 0; left < spaces.size(); ++left) {
    for (std::size_t right = left + 1; right < spaces.size(); ++right) {
      auto const mutual =
        sirius::memory::try_grant_pool_access(*spaces[left], spaces[right]->get_device_id()) &&
        sirius::memory::try_grant_pool_access(*spaces[right], spaces[left]->get_device_id());
      // Members of one island always have mutual access.
      if (ids[left] == ids[right]) { CHECK(mutual); }
    }
  }
  CHECK(sirius::memory::peer_islands(spaces) == ids);
}
