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

#include "diagnostics.hpp"

#include "io/cache/prefetching_cache.hpp"
#include "io/sirius_datasource.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace order_diagnostic {
namespace {
thread_local std::uint64_t cohort{};
thread_local sirius::io::deferred_device_batch const* requested{};
thread_local char const* role = "other";
thread_local snapshot current_issue;
struct event {
  char const* kind{};
  char const* role{};
  std::uint64_t cohort{};
  std::int64_t time_ns{};
  std::int64_t duration_ns{};
  snapshot state;
  std::size_t siblings{};
  bool sibling_pending{};
};
struct registry_key {
  void const* registry;
  cudaStream_t stream;
  bool operator==(registry_key const&) const = default;
};
struct key_hash {
  std::size_t operator()(registry_key const& key) const noexcept
  {
    return std::hash<void const*>{}(key.registry) ^ std::hash<void*>{}(key.stream);
  }
};
struct registration_group {
  std::uint64_t generation{};
  bool claimed{};
  std::vector<std::weak_ptr<sirius::io::deferred_device_batch>> entries;
};
struct shared_state {
  std::mutex mutex;
  std::unordered_map<sirius::io::sirius_datasource const*, sirius::io::cache::cache_handle*>
    handles;
  std::unordered_map<registry_key, registration_group, key_hash> registry;
  std::vector<event> events;
  std::atomic<std::uint64_t> next_cohort{1};
  std::atomic<std::uint64_t> dropped{};
  std::atomic<std::uint64_t> already_issued{};
  std::atomic<std::uint64_t> unclaimed{};
  std::int64_t origin{};
};
shared_state& state()
{
  static shared_state value;
  return value;
}
void record(event const& value) noexcept
{
  try {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.events.size() < s.events.capacity())
      s.events.push_back(value);
    else
      s.dropped.fetch_add(1);
  } catch (...) {
    state().dropped.fetch_add(1);
  }
}
void copy_path(char (&destination)[256], char const* source) noexcept
{
  if (source == nullptr) return;
  std::strncpy(destination, source, sizeof(destination) - 1);
}
std::string json_string(char const* value)
{
  std::string result = "\"";
  for (; *value; ++value) {
    auto c = static_cast<unsigned char>(*value);
    if (c == '\\' || c == '"') result += '\\';
    if (c < 32)
      result += '?';
    else
      result += static_cast<char>(c);
  }
  result += '"';
  return result;
}
}  // namespace
std::int64_t now_ns() noexcept
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
           std::chrono::steady_clock::now().time_since_epoch())
    .count();
}
void track(sirius::io::sirius_datasource const* source,
           sirius::io::cache::cache_handle* handle) noexcept
{
  try {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.handles[source] = handle;
  } catch (...) {
    state().dropped.fetch_add(1);
  }
}
void untrack(sirius::io::sirius_datasource const* source) noexcept
{
  auto& s = state();
  std::lock_guard lock(s.mutex);
  s.handles.erase(source);
}
void registered(sirius::io::deferred_batch_registry const* registry,
                std::shared_ptr<sirius::io::deferred_device_batch> const& batch) noexcept
{
  try {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto& group = s.registry[{registry, batch->stream()}];
    std::erase_if(group.entries, [](auto const& weak) { return weak.expired(); });
    if (group.entries.empty()) {
      group.generation = s.next_cohort.fetch_add(1);
      group.claimed    = false;
    }
    group.entries.push_back(batch);
  } catch (...) {
    state().dropped.fetch_add(1);
  }
}
void extracted(sirius::io::deferred_batch_registry const* registry, cudaStream_t stream) noexcept
{
  auto& s = state();
  std::lock_guard lock(s.mutex);
  s.registry.erase({registry, stream});
}
peer_snapshot peers(sirius::io::deferred_batch_registry const* registry,
                    cudaStream_t stream,
                    sirius::io::deferred_device_batch const* requested_batch) noexcept
{
  try {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    auto it = s.registry.find({registry, stream});
    if (it != s.registry.end() && !it->second.claimed) {
      auto const contains_requested =
        std::ranges::any_of(it->second.entries, [requested_batch](auto const& weak) {
          auto batch = weak.lock();
          return batch && batch.get() == requested_batch;
        });
      if (!contains_requested) return {};
      peer_snapshot result{true, it->second.generation, it->second.entries};
      it->second.claimed = true;
      return result;
    }
  } catch (...) {
    state().dropped.fetch_add(1);
  }
  return {};
}

snapshot observe(sirius::io::sirius_datasource const* source,
                 std::span<sirius::io::slice const> slices,
                 cudaStream_t stream,
                 int device) noexcept
{
  snapshot result;
  result.source_id = reinterpret_cast<std::uintptr_t>(source);
  result.stream    = reinterpret_cast<std::uintptr_t>(stream);
  result.device    = device;
  result.ranges    = slices.size();
  for (auto const& slice : slices)
    result.bytes += slice.size();
  try {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    copy_path(result.path, source->get_io_object().object_path().c_str());
    auto it = s.handles.find(source);
    if (it == s.handles.end()) {
      s.dropped.fetch_add(1);
      return result;
    }
    auto* handle    = it->second;
    result.handle   = static_cast<bool>(*handle);
    result.producer = static_cast<int>(handle->producer_state());
    auto chunks     = handle->chunks();
    auto cache      = source->io_ctx()->cache();
    if (chunks && cache) {
      auto const chunk_size = cache->chunk_size();
      auto const file_size  = source->get_io_object().size();
      for (auto const& slice : slices) {
        if (slice.size() == 0 || slice.dst == nullptr || slice.offset() >= file_size) continue;
        auto const size  = std::min(slice.size(), file_size - slice.offset());
        auto const first = slice.offset() / chunk_size * chunk_size;
        auto const last  = (slice.offset() + size - 1) / chunk_size * chunk_size;
        auto position =
          std::lower_bound(chunks->begin(), chunks->end(), first, [](auto* chunk, auto offset) {
            return chunk->offset < offset;
          });
        for (; position != chunks->end() && (*position)->offset <= last; ++position) {
          if ((*position)->state.get_state() == sirius::io::cache::chunk_state::loading) {
            result.loading_overlap = true;
            break;
          }
        }
        if (result.loading_overlap) break;
      }
    }
    result.inflight = handle->is_prefetch_in_flight();
  } catch (...) {
    state().dropped.fetch_add(1);
  }
  return result;
}
await_scope::await_scope(sirius::io::deferred_device_batch const* batch) noexcept
  : _previous_cohort(cohort),
    _previous_requested(requested),
    _previous_role(role),
    _previous_issue(current_issue)
{
  cohort    = state().next_cohort.fetch_add(1);
  requested = batch;
  role      = "own";
}
await_scope::~await_scope()
{
  cohort        = _previous_cohort;
  requested     = _previous_requested;
  role          = _previous_role;
  current_issue = _previous_issue;
}
void await_scope::entry(snapshot const& value, std::size_t siblings, bool sibling_pending) noexcept
{
  record({"entry", "own", cohort, now_ns(), 0, value, siblings, sibling_pending});
}
void await_scope::sibling(snapshot const& value) noexcept
{
  record({"entry", "sibling", cohort, now_ns(), 0, value});
}
void await_scope::already_issued() noexcept { state().already_issued.fetch_add(1); }
void await_scope::unclaimed() noexcept { state().unclaimed.fetch_add(1); }
void await_scope::adopt_cohort(std::uint64_t generation) noexcept { cohort = generation; }
void issue(sirius::io::deferred_device_batch const* batch, snapshot const& value) noexcept
{
  role          = requested == nullptr ? "other" : requested == batch ? "own" : "sibling";
  current_issue = value;
  record({"issue", role, cohort, now_ns(), 0, value});
}
void waited(std::int64_t begin, std::int64_t end, char const* path) noexcept
{
  snapshot value = current_issue;
  copy_path(value.path, path);
  record({"wait", role, cohort, begin, end - begin, value});
}
}  // namespace order_diagnostic

extern "C" void sirius_order_diag_reset()
{
  auto& s = order_diagnostic::state();
  std::lock_guard lock(s.mutex);
  s.events.clear();
  s.events.reserve(262144);
  s.dropped.store(0);
  s.already_issued.store(0);
  s.unclaimed.store(0);
  s.origin = order_diagnostic::now_ns();
}
extern "C" void sirius_order_diag_dump(char const* path)
{
  auto& s = order_diagnostic::state();
  std::lock_guard lock(s.mutex);
  std::ofstream output(path);
  output << "{\"dropped\":" << s.dropped.load()
         << ",\"already_issued_awaits\":" << s.already_issued.load()
         << ",\"unclaimed_open_awaits\":" << s.unclaimed.load() << ",\"events\":[";
  bool comma = false;
  for (auto const& e : s.events) {
    if (comma) output << ',';
    comma         = true;
    auto const& v = e.state;
    output << "{\"kind\":" << order_diagnostic::json_string(e.kind)
           << ",\"role\":" << order_diagnostic::json_string(e.role) << ",\"cohort\":" << e.cohort
           << ",\"time_ns\":" << e.time_ns - s.origin << ",\"duration_ns\":" << e.duration_ns
           << ",\"source\":" << order_diagnostic::json_string(v.path)
           << ",\"source_id\":" << v.source_id << ",\"stream\":" << v.stream
           << ",\"device\":" << v.device << ",\"handle\":" << v.handle
           << ",\"producer\":" << v.producer << ",\"inflight\":" << v.inflight
           << ",\"loading_overlap\":" << v.loading_overlap
           << ",\"pending\":" << (v.inflight && v.loading_overlap) << ",\"ranges\":" << v.ranges
           << ",\"bytes\":" << v.bytes << ",\"open_siblings\":" << e.siblings
           << ",\"any_sibling_pending\":" << e.sibling_pending << '}';
  }
  output << "]}\n";
  if (!output) throw std::runtime_error("diagnostic output failed");
}
