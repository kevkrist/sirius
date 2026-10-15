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

#include "io/deferred_device_batch.hpp"
#include "io/types.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace sirius::io {
class sirius_datasource;
namespace cache {
class cache_handle;
}
}  // namespace sirius::io

namespace order_diagnostic {
struct snapshot {
  bool handle{};
  int producer{-1};
  bool inflight{};
  bool loading_overlap{};
  std::size_t ranges{};
  std::size_t bytes{};
  std::uintptr_t source_id{};
  std::uintptr_t stream{};
  int device{-1};
  char path[256]{};
};
void track(sirius::io::sirius_datasource const*, sirius::io::cache::cache_handle*) noexcept;
void untrack(sirius::io::sirius_datasource const*) noexcept;
void registered(sirius::io::deferred_batch_registry const*,
                std::shared_ptr<sirius::io::deferred_device_batch> const&) noexcept;
void extracted(sirius::io::deferred_batch_registry const*, cudaStream_t) noexcept;
struct peer_snapshot {
  bool first{};
  std::uint64_t generation{};
  std::vector<std::weak_ptr<sirius::io::deferred_device_batch>> entries;
};
peer_snapshot peers(sirius::io::deferred_batch_registry const*,
                    cudaStream_t,
                    sirius::io::deferred_device_batch const*) noexcept;
snapshot observe(sirius::io::sirius_datasource const*,
                 std::span<sirius::io::slice const>,
                 cudaStream_t,
                 int) noexcept;
void issue(sirius::io::deferred_device_batch const*, snapshot const&) noexcept;
std::int64_t now_ns() noexcept;
void waited(std::int64_t, std::int64_t, char const*) noexcept;

class await_scope {
 public:
  explicit await_scope(sirius::io::deferred_device_batch const*) noexcept;
  ~await_scope();
  await_scope(await_scope const&)            = delete;
  await_scope& operator=(await_scope const&) = delete;
  void entry(snapshot const&, std::size_t siblings, bool sibling_pending) noexcept;
  void sibling(snapshot const&) noexcept;
  void already_issued() noexcept;
  void unclaimed() noexcept;
  void adopt_cohort(std::uint64_t) noexcept;

 private:
  std::uint64_t _previous_cohort{};
  sirius::io::deferred_device_batch const* _previous_requested{};
  char const* _previous_role{};
  snapshot _previous_issue;
};
}  // namespace order_diagnostic
