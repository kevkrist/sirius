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

#include "memory/peer_islands.hpp"

#include "memory/pool_peer_access.hpp"

#include <cucascade/memory/memory_space.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>

namespace sirius::memory {

std::vector<std::uint8_t> islands_from_access(std::size_t count, std::span<bool const> access)
{
  if (count > std::numeric_limits<std::uint8_t>::max() || access.size() != count * count) {
    throw std::invalid_argument{"[islands_from_access] the access matrix does not match the count"};
  }
  auto const mutual = [&](std::size_t left, std::size_t right) {
    return access[left * count + right] && access[right * count + left];
  };
  std::vector<std::uint8_t> ids(count);
  std::uint8_t islands = 0;
  for (std::size_t device = 0; device < count; ++device) {
    std::uint8_t island = 0;
    for (; island < islands; ++island) {
      bool fits = true;
      for (std::size_t member = 0; member < device && fits; ++member) {
        fits = ids[member] != island || mutual(member, device);
      }
      if (fits) { break; }
    }
    ids[device] = island;
    islands     = std::max<std::uint8_t>(islands, island + 1);
  }
  return ids;
}

std::vector<std::uint8_t> peer_islands(
  std::span<cucascade::memory::memory_space const* const> spaces)
{
  auto const count = spaces.size();
  // std::vector<bool> has no contiguous storage to view.
  auto const access = std::make_unique<bool[]>(count * count);
  for (std::size_t owner = 0; owner < count; ++owner) {
    for (std::size_t accessor = 0; accessor < count; ++accessor) {
      access[owner * count + accessor] =
        owner == accessor ||
        try_grant_pool_access(*spaces[owner], spaces[accessor]->get_device_id());
    }
  }
  return islands_from_access(count, std::span<bool const>{access.get(), count * count});
}

}  // namespace sirius::memory
