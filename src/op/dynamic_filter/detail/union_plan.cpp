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

#include "op/dynamic_filter/detail/union_plan.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace sirius::op::detail {

union_plan union_plan::make(std::span<std::uint8_t const> island_ids,
                            std::span<bool const> contributed,
                            std::size_t root)
{
  auto const members = island_ids.size();
  if (members == 0 || members != contributed.size() || root >= members ||
      members > std::numeric_limits<std::uint8_t>::max()) {
    throw std::invalid_argument{"[union_plan::make] inconsistent members"};
  }
  union_plan plan;
  auto const add_island = [&](std::uint8_t id, std::size_t leader) {
    island entry{static_cast<std::uint8_t>(leader), contributed[leader], {}, {}};
    for (std::size_t member = 0; member < members; ++member) {
      if (island_ids[member] != id || member == leader) { continue; }
      entry.others.push_back(static_cast<std::uint8_t>(member));
      if (contributed[member]) { entry.sources.push_back(static_cast<std::uint8_t>(member)); }
    }
    plan.islands.push_back(std::move(entry));
  };
  add_island(island_ids[root], root);
  for (std::size_t first = 0; first < members; ++first) {
    auto const id = island_ids[first];
    // An island is added at its first member.
    if (id == island_ids[root] || std::ranges::find(island_ids.first(first), id) !=
                                    island_ids.begin() + static_cast<std::ptrdiff_t>(first)) {
      continue;
    }
    auto leader = first;
    for (auto member = first; member < members; ++member) {
      if (island_ids[member] == id && contributed[member]) {
        leader = member;
        break;
      }
    }
    add_island(id, leader);
  }
  return plan;
}

union_plan union_plan::split(std::size_t index) const
{
  union_plan plan;
  plan.islands.reserve(islands.size() + islands[index].others.size());
  for (std::size_t position = 0; position < islands.size(); ++position) {
    auto const& entry = islands[position];
    if (position != index) {
      plan.islands.push_back(entry);
      continue;
    }
    plan.islands.push_back({entry.leader, entry.leader_contributes, {}, {}});
    for (auto const member : entry.others) {
      plan.islands.push_back(
        {member, std::ranges::find(entry.sources, member) != entry.sources.end(), {}, {}});
    }
  }
  return plan;
}

std::size_t union_plan::inputs() const noexcept
{
  return static_cast<std::size_t>(
    std::ranges::count_if(islands, [](island const& entry) { return entry.contributes(); }));
}

bool union_plan::crosses_host() const noexcept { return islands.size() >= 2 && inputs() >= 1; }

bool union_plan::moves_nothing() const noexcept
{
  return inputs() == 0 || (islands.size() == 1 && islands.front().others.empty());
}

bool union_plan::uploads(island const& entry) const noexcept
{
  return crosses_host() && (inputs() >= 2 || !entry.contributes());
}

std::size_t union_plan::host_slots() const noexcept
{
  if (!crosses_host()) { return 0; }
  auto const count = inputs();
  return count >= 2 ? count + 1 : 1;
}

}  // namespace sirius::op::detail
