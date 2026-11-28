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
#include "op/dynamic_filter/detail/union_plan.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {
using sirius::op::detail::union_plan;

constexpr std::size_t max_members = 8;

struct plan_case {
  std::array<std::uint8_t, max_members> ids;
  std::array<bool, max_members> contributed;
  std::size_t members;
  std::size_t root;
};

/// Calls `visit` for every grouping of up to `max_members` members into islands, every set of
/// contributing members and every root.
template <class Visit>
void for_each_case(Visit&& visit)
{
  for (std::size_t members = 1; members <= max_members; ++members) {
    // Restricted growth strings enumerate each set partition once.
    std::array<std::uint8_t, max_members> ids{};
    for (;;) {
      for (unsigned mask = 0; mask < (1U << members); ++mask) {
        plan_case current{ids, {}, members, 0};
        for (std::size_t member = 0; member < members; ++member) {
          current.contributed[member] = (mask >> member & 1U) != 0;
        }
        for (current.root = 0; current.root < members; ++current.root) {
          visit(current);
        }
      }
      auto position = members;
      while (position-- > 1) {
        auto const limit = *std::max_element(ids.begin(), ids.begin() + position) + 1;
        if (ids[position] < limit) {
          ++ids[position];
          std::fill(ids.begin() + position + 1, ids.begin() + members, std::uint8_t{0});
          break;
        }
      }
      if (position == 0 || members == 1) { break; }
    }
  }
}

/// The members of `entry`, leader first.
std::vector<std::uint8_t> members_of(union_plan::island const& entry)
{
  std::vector<std::uint8_t> members{entry.leader};
  members.insert(members.end(), entry.others.begin(), entry.others.end());
  return members;
}

/// Counts the roles of `plan` that break a rule `union_plan::make` documents.
std::size_t violations(plan_case const& input, union_plan const& plan)
{
  std::size_t broken = 0;
  auto const expect  = [&](bool holds) { broken += holds ? 0 : 1; };
  std::array<int, max_members> seen{};
  std::size_t contributing_islands = 0;
  for (std::size_t position = 0; position < plan.islands.size(); ++position) {
    auto const& entry              = plan.islands[position];
    auto const id                  = input.ids[entry.leader];
    bool any                       = false;
    std::size_t first              = input.members;
    std::size_t first_contributing = input.members;
    for (std::size_t member = 0; member < input.members; ++member) {
      if (input.ids[member] != id) { continue; }
      first = std::min(first, member);
      if (input.contributed[member]) {
        first_contributing = std::min(first_contributing, member);
        any                = true;
      }
    }
    contributing_islands += any ? 1 : 0;
    expect(entry.contributes() == any);
    expect(entry.leader_contributes == input.contributed[entry.leader]);
    if (position == 0) {
      expect(entry.leader == input.root);
    } else {
      expect(id != input.ids[input.root]);
      expect(entry.leader == (any ? first_contributing : first));
    }
    expect(std::ranges::is_sorted(entry.others));
    expect(std::ranges::is_sorted(entry.sources));
    for (auto const member : members_of(entry)) {
      ++seen[member];
      expect(input.ids[member] == id);
    }
    for (auto const member : entry.others) {
      auto const is_source = std::ranges::find(entry.sources, member) != entry.sources.end();
      expect(member != entry.leader);
      expect(is_source == input.contributed[member]);
    }
    expect(union_plan::merge_slots(entry) == entry.sources.size());
  }
  for (std::size_t member = 0; member < max_members; ++member) {
    expect(seen[member] == (member < input.members ? 1 : 0));
  }
  auto const distinct = static_cast<std::size_t>(
                          *std::max_element(input.ids.begin(), input.ids.begin() + input.members)) +
                        1;
  expect(plan.islands.size() == distinct);
  expect(plan.inputs() == contributing_islands);
  expect(plan.crosses_host() == (distinct >= 2 && contributing_islands >= 1));
  expect(plan.moves_nothing() == (contributing_islands == 0 || input.members == 1));
  expect(plan.host_slots() == (!plan.crosses_host()        ? 0
                               : contributing_islands >= 2 ? contributing_islands + 1
                                                           : 1));
  for (auto const& entry : plan.islands) {
    expect(plan.uploads(entry) ==
           (plan.crosses_host() && (contributing_islands >= 2 || !entry.contributes())));
  }
  return broken;
}
}  // namespace

TEST_CASE("union_plan assigns every member one role under the leader rule",
          "[dynamic_filter][union_plan]")
{
  std::size_t cases  = 0;
  std::size_t broken = 0;
  for_each_case([&](plan_case const& input) {
    ++cases;
    auto const plan = union_plan::make(
      {input.ids.data(), input.members}, {input.contributed.data(), input.members}, input.root);
    broken += violations(input, plan);
  });
  // Bell numbers 1, 2, 5, 15, 52, 203, 877, 4140, times contribution masks and roots.
  CHECK(cases == 1 * 2 * 1 + 2 * 4 * 2 + 5 * 8 * 3 + 15 * 16 * 4 + 52 * 32 * 5 + 203 * 64 * 6 +
                   877 * 128 * 7 + 4140 * 256 * 8);
  CHECK(broken == 0);
}

TEST_CASE("union_plan::split turns one island into single-member islands and keeps the others",
          "[dynamic_filter][union_plan]")
{
  std::size_t broken = 0;
  auto const expect  = [&](bool holds) { broken += holds ? 0 : 1; };
  auto const same    = [](union_plan::island const& left, union_plan::island const& right) {
    return left.leader == right.leader && left.leader_contributes == right.leader_contributes &&
           left.sources == right.sources && left.others == right.others;
  };
  for_each_case([&](plan_case const& input) {
    // Splitting is exercised up to 6 members; larger cases add no new shape.
    if (input.members > 6) { return; }
    auto const plan = union_plan::make(
      {input.ids.data(), input.members}, {input.contributed.data(), input.members}, input.root);
    for (std::size_t index = 0; index < plan.islands.size(); ++index) {
      auto const pieces  = members_of(plan.islands[index]);
      auto const divided = plan.split(index);
      expect(divided.islands.size() == plan.islands.size() + pieces.size() - 1);
      if (divided.islands.size() != plan.islands.size() + pieces.size() - 1) { continue; }
      for (std::size_t position = 0; position < index; ++position) {
        expect(same(divided.islands[position], plan.islands[position]));
      }
      for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
        auto const& single = divided.islands[index + piece];
        expect(single.leader == pieces[piece]);
        expect(single.leader_contributes == input.contributed[pieces[piece]]);
        expect(single.sources.empty() && single.others.empty());
      }
      for (std::size_t position = index + 1; position < plan.islands.size(); ++position) {
        expect(same(divided.islands[position + pieces.size() - 1], plan.islands[position]));
      }
      expect(divided.islands.front().leader == input.root);
    }
  });
  CHECK(broken == 0);
}

TEST_CASE("union_plan::make rejects inconsistent members", "[dynamic_filter][union_plan]")
{
  std::array<std::uint8_t, 2> const ids{0, 1};
  std::array<bool, 2> const contributed{true, false};
  CHECK_THROWS_AS(union_plan::make(ids, contributed, 2), std::invalid_argument);
  CHECK_THROWS_AS(union_plan::make(ids, std::span{contributed}.first(1), 0), std::invalid_argument);
  CHECK_THROWS_AS(union_plan::make({}, {}, 0), std::invalid_argument);
}

TEST_CASE("union_plan sizes the workspace of the measured machine shapes",
          "[dynamic_filter][union_plan]")
{
  std::array<bool, 4> const all{true, true, true, true};
  std::array<bool, 4> const root_only{true, false, false, false};
  std::array<std::uint8_t, 4> const pairs{0, 0, 1, 1};
  std::array<std::uint8_t, 4> const singles{0, 1, 2, 3};
  std::array<std::uint8_t, 4> const full{0, 0, 0, 0};

  auto plan = union_plan::make(pairs, all, 0);
  CHECK(plan.inputs() == 2);
  CHECK(plan.host_slots() == 3);
  CHECK(union_plan::merge_slots(plan.islands[0]) == 1);
  CHECK(union_plan::merge_slots(plan.islands[1]) == 1);

  plan = union_plan::make(pairs, root_only, 0);
  CHECK(plan.inputs() == 1);
  CHECK(plan.host_slots() == 1);
  CHECK(union_plan::merge_slots(plan.islands[0]) == 0);
  CHECK_FALSE(plan.uploads(plan.islands[0]));
  CHECK(plan.uploads(plan.islands[1]));

  plan = union_plan::make(singles, all, 0);
  CHECK(plan.inputs() == 4);
  CHECK(plan.host_slots() == 5);

  plan = union_plan::make(full, all, 2);
  CHECK_FALSE(plan.crosses_host());
  CHECK(plan.host_slots() == 0);
  CHECK(union_plan::merge_slots(plan.islands[0]) == 3);
  CHECK(plan.islands[0].leader == 2);
}
