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

/**
 * @file union_plan.hpp
 * @brief Who does what when `unite` makes equally shaped arrays on several GPUs equal to their
 * bitwise OR.
 *
 * Vocabulary, shared with `union_transfer.hpp`:
 * - A member is one GPU's copy of the arrays, named by its index in the request.
 * - An island is a set of members that can copy directly between each other (see
 *   `sirius::memory::peer_islands`).
 * - Each island has one leader, the only member that exchanges data with the host.
 * - A member contributes if its arrays may hold set bits; the arrays of the others are all zero. An
 *   island contributes if any member does, and its leader is then an input of the host stage.
 *
 * A plan only assigns roles. It is plain host data without any GPU state, fixed before the first
 * transfer is submitted.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sirius::op::detail {

/**
 * @brief The roles of every member in one `unite` call.
 */
struct union_plan {
  /**
   * @brief One island's roles.
   */
  struct island {
    std::uint8_t leader;                ///< The member that exchanges data with the host
    bool leader_contributes;            ///< Whether the leader's own arrays may hold set bits
    std::vector<std::uint8_t> sources;  ///< Contributing members other than the leader
    std::vector<std::uint8_t> others;   ///< Every member other than the leader, in member order

    [[nodiscard]] bool contributes() const noexcept
    {
      return leader_contributes || !sources.empty();
    }
  };

  std::vector<island> islands;  ///< The root's island first, the rest by their first member

  /**
   * @brief Assigns the roles.
   *
   * The root leads its island. Any other island is led by its first contributing member, or by its
   * first member when none contributes, so that a contributing island never copies its leader's
   * data in from elsewhere.
   *
   * @throw std::invalid_argument if the spans differ in size, are empty, hold more than 255
   * members, or @p root is not a member
   * @param island_ids One island id per member; members with equal ids form an island
   * @param contributed Whether each member contributes
   * @param root The member whose island comes first and which leads it
   */
  [[nodiscard]] static union_plan make(std::span<std::uint8_t const> island_ids,
                                       std::span<bool const> contributed,
                                       std::size_t root);

  /**
   * @brief This plan with island @p index replaced by one island per member, so that none of
   * them needs device scratch. The other islands are unchanged.
   *
   * @pre `index < islands.size()`
   */
  [[nodiscard]] union_plan split(std::size_t index) const;

  /// Number of contributing islands: the inputs of the host stage
  [[nodiscard]] std::size_t inputs() const noexcept;

  /// Whether leaders exchange data through the host: several islands and something to move
  [[nodiscard]] bool crosses_host() const noexcept;

  /// Whether no data moves at all: nothing was contributed, or there is one member
  [[nodiscard]] bool moves_nothing() const noexcept;

  /**
   * @brief Whether the leader of @p entry receives the union from the host: every leader when the
   * host combines several inputs, every leader but the single input otherwise.
   */
  [[nodiscard]] bool uploads(island const& entry) const noexcept;

  /**
   * @brief Pinned chunk slots held per chunk in flight: one per input, plus one for the combined
   * chunk when there are several inputs. Zero without a host stage.
   */
  [[nodiscard]] std::size_t host_slots() const noexcept;

  /// Device scratch chunk slots the leader of @p entry holds per chunk in flight
  [[nodiscard]] static std::size_t merge_slots(island const& entry) noexcept
  {
    return entry.sources.size();
  }
};

}  // namespace sirius::op::detail
