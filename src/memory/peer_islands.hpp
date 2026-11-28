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

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cucascade::memory {
class memory_space;
}

namespace sirius::memory {

/**
 * @brief Groups @p count devices into islands: sets in which every two members can copy directly
 * between each other's allocation pools.
 *
 * A device joins the first island all of whose members it has access with in both directions, else
 * it opens a new one; devices are visited in index order. Every island is therefore legal for peer
 * copies. When access is not transitive (A-B and B-C but not A-C) the grouping is legal but may use
 * more islands than necessary, which costs transfer speed only.
 *
 * @param count Number of devices
 * @param access Row-major `count x count` matrix; `access[owner * count + accessor]` is whether
 * `accessor` can read and write `owner`'s pool. The diagonal is ignored
 * @return One island id per device; ids are dense and numbered in order of first appearance
 */
[[nodiscard]] std::vector<std::uint8_t> islands_from_access(std::size_t count,
                                                            std::span<bool const> access);

/**
 * @brief The islands of @p spaces, from `try_grant_pool_access` between every two of them.
 *
 * Nothing is cached: each call asks for every grant again.
 *
 * @throw Whatever `try_grant_pool_access` throws
 * @param spaces GPU spaces on distinct devices
 * @return One island id per space, as `islands_from_access` numbers them
 */
[[nodiscard]] std::vector<std::uint8_t> peer_islands(
  std::span<cucascade::memory::memory_space const* const> spaces);

}  // namespace sirius::memory
