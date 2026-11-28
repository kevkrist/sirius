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

#include "cuda/device_copy_batch.hpp"
#include "memory/host_or.hpp"
#include "memory/runtime_stream_pool.hpp"

#include <rmm/cuda_device.hpp>
#include <rmm/device_buffer.hpp>
#include <rmm/error.hpp>

#include <cuda/accumulation_partial.cuh>
#include <cuda/utility>

#include <cucascade/memory/fixed_size_host_memory_resource.hpp>
#include <cucascade/memory/memory_space.hpp>
#include <log/logging.hpp>
#include <op/dynamic_filter/detail/accumulated_bloom_builder.hpp>
#include <op/dynamic_filter/detail/union_plan.hpp>
#include <op/dynamic_filter/detail/union_transfer.hpp>
#include <op/dynamic_filter/dynamic_filter_replica_reservation.hpp>
#include <sirius/exception.hpp>
#include <telemetry/nvtx.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace sirius::op::detail {

namespace {

using stream_lease = sirius::memory::runtime_stream_pool::lease;

// ORs the scratch slot of every source into the leader's chunk. Slot i of the scratch holds
// source i's copy of the chunk.
__global__ void or_chunk_slots(uint4* destination,
                               uint4 const* scratch,
                               std::size_t slot_stride,
                               int source_count,
                               std::size_t count)
{
  auto const stride = static_cast<std::size_t>(gridDim.x) * blockDim.x;
  for (auto index = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x; index < count;
       index += stride) {
    uint4 value = destination[index];
    for (int source = 0; source < source_count; ++source) {
      uint4 const slot = scratch[static_cast<std::size_t>(source) * slot_stride + index];
      value.x |= slot.x;
      value.y |= slot.y;
      value.z |= slot.z;
      value.w |= slot.w;
    }
    destination[index] = value;
  }
}

/**
 * @brief Launches `or_chunk_slots` over one chunk of @p bytes, a whole number of 16-byte words.
 *
 * @param chunk The leader's chunk, which receives the union
 * @param scratch The first source slot; slot i starts at `i * slot_bytes`
 * @param slot_bytes Distance between source slots
 * @param sources Number of filled source slots
 * @param bytes Bytes of the chunk
 * @param stream The leader's stream
 */
void enqueue_or_chunk(std::byte* chunk,
                      std::byte const* scratch,
                      std::size_t slot_bytes,
                      std::size_t sources,
                      std::size_t bytes,
                      cudaStream_t stream)
{
  constexpr unsigned threads = 256;
  auto const words           = bytes / sizeof(uint4);
  auto const blocks =
    static_cast<unsigned>(std::min<std::size_t>(::cuda::ceil_div(words, threads), 4096));
  check_cuda(cudaGetLastError(), "pending CUDA error before or_chunk_slots");
  or_chunk_slots<<<blocks, threads, 0, stream>>>(reinterpret_cast<uint4*>(chunk),
                                                 reinterpret_cast<uint4 const*>(scratch),
                                                 slot_bytes / sizeof(uint4),
                                                 static_cast<int>(sources),
                                                 words);
  check_cuda(cudaGetLastError(), "or_chunk_slots", true);
}

[[nodiscard]] std::size_t multiply(std::size_t left, std::size_t right)
{
  if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
    throw accumulation_invariant_error{"array union byte count overflow"};
  }
  return left * right;
}

/**
 * @brief An event that is created and destroyed with its GPU current.
 */
class device_event {
 public:
  explicit device_event(int device) : _device{device}, _event{make_event_on(device)} {}

  ~device_event()
  {
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{_device}};
    _event.reset();
  }

  device_event(device_event&&) noexcept            = default;
  device_event& operator=(device_event&&) noexcept = delete;

  [[nodiscard]] cudaEvent_t get() const noexcept { return _event->get(); }

 private:
  int _device;
  std::optional<cuda_event> _event;
};

/// One chunk of one array.
struct chunk {
  std::size_t array;
  std::size_t offset;
  std::size_t bytes;
};

/**
 * @brief One island's workspace: scratch, streams and events, all on its leader's GPU except the
 * events its sources record.
 */
struct island_state {
  int device;                         // The leader's GPU
  std::uint8_t leader;                // Index into the request's members
  std::vector<std::uint8_t> sources;  // Contributing members other than the leader
  std::vector<std::uint8_t> others;   // Every member other than the leader
  bool input              = false;    // Copies its chunks to the host
  bool uploads            = false;    // Receives chunks from the host
  std::size_t input_index = 0;        // Its position among the inputs

  std::optional<stream_lease> ingress_lease;  // Absent for the root, which uses the caller's stream
  cudaStream_t ingress = nullptr;             // Carries the peer copies into scratch
  std::optional<stream_lease> download;       // Leader to host
  std::optional<stream_lease> upload;         // Host to leader
  std::optional<scoped_replica_reservation> scratch_lease;  // Absent for the root
  std::unique_ptr<rmm::device_buffer> scratch;  // Up to two buffers of one slot per source

  std::vector<device_event> source_ready;  // Per source: its earlier writes are complete
  std::optional<device_event> ready;       // The leader's earlier writes are complete
  std::vector<device_event> ingress_done;  // Per scratch buffer: its peer copies landed
  std::vector<device_event> merged;        // Per scratch buffer: it was ORed into the leader
  std::vector<device_event> down_done;     // Per window chunk: the leader's copy is on the host
  std::vector<device_event> up_done;       // Per window chunk: the host's copy is on the leader

  island_state(int leader_device, union_plan::island const& roles)
    : device{leader_device}, leader{roles.leader}, sources{roles.sources}, others{roles.others}
  {
  }

  island_state(island_state&&) noexcept = default;

  ~island_state()
  {
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{device}};
    scratch.reset();
    // The reservation is detached only after the scratch it accounts for is freed.
    scratch_lease.reset();
    upload.reset();
    download.reset();
    ingress_lease.reset();
  }
};

/**
 * @brief One `unite` call: its plan, its workspace, and the order in which transfers are submitted.
 *
 * Ordering rules, with chunks numbered over all arrays, `buffer = chunk % 2` and
 * `slot = chunk % window`:
 * - A scratch buffer is refilled only after the merge that read it (`merged[buffer]` of two
 *   chunks earlier).
 * - A download follows its leader's merge of that chunk, and refills its host slot only after the
 *   readers of the previous content: the host OR, which finished before the download is submitted,
 *   or the other leaders' uploads when a single input is passed through (`up_done[slot]`).
 * - An upload follows the download of the same chunk from its own leader, so a leader's chunk is
 *   never overwritten before it was read.
 * - A fan-out copy follows whatever last wrote the leader's chunk.
 * An event is recorded again for every chunk; each wait on it is submitted before its next record.
 */
class union_run {
 public:
  union_run(union_request const& request, union_or_runner runner)
    : _request{request}, _runner{runner}
  {
    _report.device_scratch_bytes.assign(request.members.size(), 0);
    for (std::size_t array = 0; array < request.shapes.size(); ++array) {
      auto const& shape = request.shapes[array];
      for (std::size_t offset = 0; offset < shape.bytes; offset += shape.chunk_bytes) {
        _chunks.push_back({array, offset, std::min(shape.chunk_bytes, shape.bytes - offset)});
      }
      if (shape.bytes != 0) { _slot_bytes = std::max(_slot_bytes, shape.chunk_bytes); }
    }
  }

  /**
   * @brief Plans the roles and acquires all workspace; submits nothing.
   *
   * @return false iff the smallest pinned window was refused
   */
  [[nodiscard]] bool prepare()
  {
    auto const members = _request.members.size();
    // std::vector<bool> has no contiguous storage to view.
    auto const contributed = std::make_unique<bool[]>(members);
    std::ranges::transform(_request.members, contributed.get(), &union_member::contributed);
    auto plan = union_plan::make(_request.island_ids, {contributed.get(), members}, _request.root);
    _joined.push_back(_request.root_stream);
    for (std::size_t member = 0; member < members; ++member) {
      join(_request.members[member].stream, device_of(member));
    }
    if (plan.moves_nothing() || _chunks.empty()) {
      _report.islands = static_cast<std::uint8_t>(plan.islands.size());
      create_completion_events();
      return true;
    }
    for (std::size_t index = 0; index < plan.islands.size(); ++index) {
      if (plan.islands[index].sources.empty()) {
        _islands.emplace_back(device_of(plan.islands[index].leader), plan.islands[index]);
      } else if (!add_merging_island(plan.islands[index])) {
        // Its members follow as single-member islands, which need no scratch.
        plan             = plan.split(index);
        _report.degraded = true;
        --index;
      }
    }
    _inputs       = plan.inputs();
    _crosses_host = plan.crosses_host();
    for (std::size_t index = 0; index < _islands.size(); ++index) {
      auto& entry         = _islands[index];
      auto const& roles   = plan.islands[index];
      entry.input         = _crosses_host && roles.contributes();
      entry.uploads       = plan.uploads(roles);
      _report.peer_copies = _report.peer_copies || !entry.others.empty();
    }
    if (_crosses_host && !acquire_window(plan.host_slots())) { return false; }
    acquire_streams_and_events();
    create_completion_events();
    _report.islands       = static_cast<std::uint8_t>(_islands.size());
    _report.host_inputs   = static_cast<std::uint8_t>(_crosses_host ? _inputs : 0);
    _report.window_chunks = static_cast<std::uint8_t>(_window);
    return true;
  }

  /// Every stream the call may have submitted work to.
  [[nodiscard]] std::span<::cuda::stream_ref const> streams() const noexcept { return _joined; }

  /// Submits every transfer, running the host ORs in between.
  void submit()
  {
    if (_islands.empty()) { return; }
    order_after_earlier_writes();
    auto const count = _chunks.size();
    if (_crosses_host && _inputs >= 2) {
      // Only the host OR blocks this thread, so the transfers that feed later ORs are submitted
      // ahead of it: downloads one window ahead, merges two chunks ahead of the downloads. Merge
      // (j + 2) records the event download j waits on again, so it may not be submitted earlier.
      std::size_t downloaded = 0;
      std::size_t merged     = 0;
      for (std::size_t sequence = 0; sequence < count; ++sequence) {
        for (; downloaded < std::min(sequence + _window, count); ++downloaded) {
          if (merged == downloaded) { submit_merge(merged++); }
          submit_download(downloaded);
        }
        for (; merged < std::min(downloaded + 2, count); ++merged) {
          submit_merge(merged);
        }
        combine_on_host(sequence);
        submit_upload(sequence);
        submit_fan_out(sequence);
      }
      return;
    }
    for (std::size_t sequence = 0; sequence < count; ++sequence) {
      submit_merge(sequence);
      if (_crosses_host) {
        submit_download(sequence);
        submit_upload(sequence);
      }
      submit_fan_out(sequence);
    }
  }

  /**
   * @brief Makes the caller's stream wait for every other stream of `streams()` and waits for it on
   * the host: the call's only blocking wait besides the host ORs.
   */
  void wait() const
  {
    for (std::size_t index = 0; index < _completed.size(); ++index) {
      {
        rmm::cuda_set_device_raii guard{rmm::cuda_device_id{_joined_devices[index]}};
        check_cuda(cudaEventRecord(_completed[index].get(), _joined[index + 1].get()),
                   "cudaEventRecord(completed)");
      }
      check_cuda(cudaStreamWaitEvent(_request.root_stream.get(), _completed[index].get(), 0),
                 "cudaStreamWaitEvent(completed)");
    }
    nvtx_scoped_range wait_range{"dynfilter::accum::late_wait"};
    check_cuda(cudaStreamSynchronize(_request.root_stream.get()),
               "cudaStreamSynchronize(array union)");
  }

  [[nodiscard]] union_report take_report() noexcept { return std::move(_report); }

 private:
  /// Registers a stream the call submits work to.
  void join(::cuda::stream_ref stream, int device)
  {
    _joined.push_back(stream);
    _joined_devices.push_back(device);
  }

  /// One event per registered stream, for the final wait.
  void create_completion_events()
  {
    _completed.reserve(_joined_devices.size());
    for (auto const device : _joined_devices) {
      _completed.emplace_back(device);
    }
  }

  [[nodiscard]] int device_of(std::size_t member) const
  {
    return _request.members[member].space->get_device_id();
  }

  [[nodiscard]] std::byte* chunk_of(std::size_t member, chunk const& piece) const noexcept
  {
    return _request.members[member].arrays[piece.array] + piece.offset;
  }

  [[nodiscard]] cudaStream_t stream_of(std::size_t member) const noexcept
  {
    return _request.members[member].stream.get();
  }

  /**
   * @brief Adds an island with sources once its merge scratch is acquired: from the caller's
   * resource for the root, from a reservation in the leader's space otherwise.
   *
   * @return false, adding nothing, iff the scratch was refused
   */
  [[nodiscard]] bool add_merging_island(union_plan::island const& roles)
  {
    auto const buffers = std::min<std::size_t>(2, _chunks.size());
    auto const bytes   = multiply(multiply(buffers, roles.sources.size()), _slot_bytes);
    if (bytes > std::numeric_limits<std::size_t>::max() - (rmm::CUDA_ALLOCATION_ALIGNMENT - 1)) {
      throw accumulation_invariant_error{"array union scratch alignment overflow"};
    }
    auto const charge = tracked_replica_allocation_bytes(bytes);
    island_state entry{device_of(roles.leader), roles};
    rmm::cuda_set_device_raii guard{rmm::cuda_device_id{entry.device}};
    bool refused = false;
    try {
      if (roles.leader == _request.root) {
        entry.ingress = _request.root_stream.get();
        entry.scratch =
          std::make_unique<rmm::device_buffer>(bytes, _request.root_stream, _request.root_scratch);
      } else {
        auto& space = *_request.members[roles.leader].space;
        entry.ingress_lease.emplace(sirius::memory::runtime_stream_pool::acquire(space));
        entry.ingress = entry.ingress_lease->get().get();
        auto lease =
          scoped_replica_reservation::try_acquire(space, charge, ::cuda::stream_ref{entry.ingress});
        if (lease) {
          entry.scratch_lease.emplace(std::move(*lease));
          entry.scratch = std::make_unique<rmm::device_buffer>(
            bytes, ::cuda::stream_ref{entry.ingress}, entry.scratch_lease->allocator());
        } else {
          refused = true;
        }
      }
    } catch (rmm::out_of_memory const&) {
      refused = true;
    }
    if (refused) {
      SIRIUS_LOG_INFO(
        "[unite] GPU {} refused {} bytes of merge scratch: its island of {} GPUs exchanges through "
        "the host instead of copying device to device.",
        entry.device,
        charge,
        roles.others.size() + 1);
      return false;
    }
    if (entry.ingress_lease) { join(::cuda::stream_ref{entry.ingress}, entry.device); }
    _report.device_scratch_bytes[roles.leader] = charge;
    _islands.push_back(std::move(entry));
    return true;
  }

  /**
   * @brief Takes the pinned window: @p slots chunk slots for each chunk in flight, two chunks in
   * flight when the pool allows it, else one.
   *
   * @return false iff one chunk in flight was refused too
   */
  [[nodiscard]] bool acquire_window(std::size_t slots)
  {
    auto* const host = _request.host;
    if (host == nullptr || host->get_block_size() == 0) {
      throw accumulation_invariant_error{
        "an array union across islands requires fixed HOST blocks"};
    }
    _block_bytes      = host->get_block_size();
    _slot_blocks      = ::cuda::ceil_div(_slot_bytes, _block_bytes);
    _window_slots     = slots;
    auto const wanted = std::min<std::size_t>(2, _chunks.size());
    for (_window = wanted; _window != 0; --_window) {
      auto const bytes = multiply(multiply(multiply(_window, slots), _slot_blocks), _block_bytes);
      try {
        _window_blocks = host->allocate_multiple_blocks(bytes);
      } catch (std::bad_alloc const&) {
        SIRIUS_LOG_INFO(
          "[unite] the pinned HOST pool refused a window of {} bytes ({} chunk(s) in "
          "flight for {} input(s)).",
          bytes,
          _window,
          _inputs);
        continue;
      }
      if (_window_blocks->size_bytes() < bytes) {
        throw accumulation_invariant_error{"the array union's HOST window is smaller than asked"};
      }
      _report.host_window_bytes = _window_blocks->size_bytes();
      _report.degraded          = _report.degraded || _window != wanted;
      _copies.reserve(_slot_blocks);
      if (_inputs >= 2) {
        _parts = static_cast<unsigned>(
          std::clamp<std::size_t>(std::min<std::size_t>(_runner.max_parts, 2 * _inputs),
                                  1,
                                  std::max<std::size_t>(1, _slot_bytes / minimum_part_bytes)));
        _or_inputs.resize(_parts * _inputs);
      }
      return true;
    }
    return false;
  }

  void acquire_streams_and_events()
  {
    auto const buffers = std::min<std::size_t>(2, _chunks.size());
    std::size_t inputs = 0;
    for (auto& entry : _islands) {
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{entry.device}};
      auto const& space = *_request.members[entry.leader].space;
      for (auto const source : entry.sources) {
        entry.source_ready.emplace_back(device_of(source));
      }
      if (!entry.sources.empty()) {
        for (std::size_t buffer = 0; buffer < buffers; ++buffer) {
          entry.ingress_done.emplace_back(entry.device);
          entry.merged.emplace_back(entry.device);
        }
      }
      entry.ready.emplace(entry.device);
      if (entry.input) {
        entry.input_index = inputs++;
        entry.download.emplace(sirius::memory::runtime_stream_pool::acquire(space));
        join(entry.download->get(), entry.device);
      }
      if (entry.uploads) {
        entry.upload.emplace(sirius::memory::runtime_stream_pool::acquire(space));
        join(entry.upload->get(), entry.device);
      }
      for (std::size_t slot = 0; slot < _window; ++slot) {
        if (entry.input) { entry.down_done.emplace_back(entry.device); }
        if (entry.uploads) { entry.up_done.emplace_back(entry.device); }
      }
    }
  }

  /// Orders the first read or overwrite of every array after the writes its stream already holds.
  void order_after_earlier_writes()
  {
    for (auto& entry : _islands) {
      for (std::size_t index = 0; index < entry.sources.size(); ++index) {
        auto const source = entry.sources[index];
        {
          rmm::cuda_set_device_raii guard{rmm::cuda_device_id{device_of(source)}};
          check_cuda(cudaEventRecord(entry.source_ready[index].get(), stream_of(source)),
                     "cudaEventRecord(source ready)");
        }
        rmm::cuda_set_device_raii guard{rmm::cuda_device_id{entry.device}};
        check_cuda(cudaStreamWaitEvent(entry.ingress, entry.source_ready[index].get(), 0),
                   "cudaStreamWaitEvent(source ready)");
      }
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{entry.device}};
      check_cuda(cudaEventRecord(entry.ready->get(), stream_of(entry.leader)),
                 "cudaEventRecord(leader ready)");
      if (entry.input) {
        check_cuda(cudaStreamWaitEvent(entry.download->get().get(), entry.ready->get(), 0),
                   "cudaStreamWaitEvent(leader ready)");
      }
      if (entry.uploads) {
        check_cuda(cudaStreamWaitEvent(entry.upload->get().get(), entry.ready->get(), 0),
                   "cudaStreamWaitEvent(leader ready)");
      }
      if (entry.uploads || !entry.sources.empty()) { continue; }
      // Nothing writes this leader's chunks any more: its members wait for it once.
      for (auto const member : entry.others) {
        rmm::cuda_set_device_raii member_guard{rmm::cuda_device_id{device_of(member)}};
        check_cuda(cudaStreamWaitEvent(stream_of(member), entry.ready->get(), 0),
                   "cudaStreamWaitEvent(leader ready)");
      }
    }
  }

  /// Merge: peer copies of every source's chunk into scratch, then their OR into the leader's.
  void submit_merge(std::size_t sequence)
  {
    auto const& piece = _chunks[sequence];
    auto const buffer = sequence % 2;
    for (auto& entry : _islands) {
      if (entry.sources.empty()) { continue; }
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{entry.device}};
      if (sequence >= 2) {
        check_cuda(cudaStreamWaitEvent(entry.ingress, entry.merged[buffer].get(), 0),
                   "cudaStreamWaitEvent(scratch reuse)");
      }
      auto* const slots = static_cast<std::byte*>(entry.scratch->data()) +
                          buffer * entry.sources.size() * _slot_bytes;
      for (std::size_t index = 0; index < entry.sources.size(); ++index) {
        auto const source = entry.sources[index];
        check_cuda(cudaMemcpyPeerAsync(slots + index * _slot_bytes,
                                       entry.device,
                                       chunk_of(source, piece),
                                       device_of(source),
                                       piece.bytes,
                                       entry.ingress),
                   "cudaMemcpyPeerAsync(ingress)");
      }
      check_cuda(cudaEventRecord(entry.ingress_done[buffer].get(), entry.ingress),
                 "cudaEventRecord(ingress)");
      auto const leader_stream = stream_of(entry.leader);
      check_cuda(cudaStreamWaitEvent(leader_stream, entry.ingress_done[buffer].get(), 0),
                 "cudaStreamWaitEvent(ingress)");
      enqueue_or_chunk(chunk_of(entry.leader, piece),
                       slots,
                       _slot_bytes,
                       entry.sources.size(),
                       piece.bytes,
                       leader_stream);
      check_cuda(cudaEventRecord(entry.merged[buffer].get(), leader_stream),
                 "cudaEventRecord(merged)");
    }
  }

  /// The pinned blocks of host slot @p slot for the chunk at window position @p position.
  [[nodiscard]] std::size_t first_block(std::size_t position, std::size_t slot) const noexcept
  {
    return (position * _window_slots + slot) * _slot_blocks;
  }

  /// Submits one batch of copies between a leader's chunk and the blocks of a host slot.
  void submit_host_copy(cudaStream_t stream,
                        std::byte* device_bytes,
                        std::size_t bytes,
                        std::size_t block,
                        bool to_host)
  {
    _copies.clear();
    for (std::size_t offset = 0; offset < bytes; ++block) {
      auto* const host_bytes = _window_blocks->at(block).data();
      auto const count       = std::min(_block_bytes, bytes - offset);
      if (to_host) {
        _copies.add(host_bytes, device_bytes + offset, count);
      } else {
        _copies.add(device_bytes + offset, host_bytes, count);
      }
      offset += count;
    }
    check_cuda(_copies.enqueue(::cuda::stream_ref{stream}),
               "device_copy_batch::enqueue(array union)");
  }

  /// Cross, first half: every contributing leader copies its chunk to its host slot.
  void submit_download(std::size_t sequence)
  {
    auto const& piece   = _chunks[sequence];
    auto const position = sequence % _window;
    for (auto& entry : _islands) {
      if (!entry.input) { continue; }
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{entry.device}};
      auto const stream = entry.download->get().get();
      if (!entry.sources.empty()) {
        check_cuda(cudaStreamWaitEvent(stream, entry.merged[sequence % 2].get(), 0),
                   "cudaStreamWaitEvent(merged)");
      }
      if (_inputs == 1 && sequence >= _window) {
        // Passed through: the other leaders' uploads of the previous chunk here read this slot.
        for (auto const& reader : _islands) {
          if (!reader.uploads) { continue; }
          check_cuda(cudaStreamWaitEvent(stream, reader.up_done[position].get(), 0),
                     "cudaStreamWaitEvent(window reuse)");
        }
      }
      submit_host_copy(stream,
                       chunk_of(entry.leader, piece),
                       piece.bytes,
                       first_block(position, entry.input_index),
                       true);
      check_cuda(cudaEventRecord(entry.down_done[position].get(), stream),
                 "cudaEventRecord(download)");
    }
  }

  /// Cross, between the halves: waits for the inputs of one chunk and ORs them into its output
  /// slot.
  void combine_on_host(std::size_t sequence)
  {
    using clock         = std::chrono::steady_clock;
    using milliseconds  = std::chrono::duration<double, std::milli>;
    auto const position = sequence % _window;
    auto const arrived  = clock::now();
    for (auto const& entry : _islands) {
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{entry.device}};
      if (entry.input) {
        check_cuda(cudaEventSynchronize(entry.down_done[position].get()),
                   "cudaEventSynchronize(download)");
      }
      if (entry.uploads && sequence >= _window) {
        // The uploads of the previous chunk at this window position read the output slot.
        check_cuda(cudaEventSynchronize(entry.up_done[position].get()),
                   "cudaEventSynchronize(upload)");
      }
    }
    auto const started = clock::now();
    {
      nvtx_scoped_range or_range{"dynfilter::accum::host_or"};
      or_job job{this, position, _chunks[sequence].bytes};
      _runner.run(
        _runner.executor,
        _parts,
        [](void* context, unsigned part) noexcept {
          auto const& current = *static_cast<or_job const*>(context);
          current.owner->combine_part(current.position, current.bytes, part);
        },
        &job);
    }
    _report.host_wait_ms += milliseconds{started - arrived}.count();
    _report.host_or_ms += milliseconds{clock::now() - started}.count();
  }

  /// ORs one part of a chunk: an equal share of its bytes, cut at 64-byte boundaries.
  void combine_part(std::size_t position, std::size_t bytes, unsigned part) noexcept
  {
    constexpr std::size_t cut = 64;
    auto const share          = [&](unsigned index) {
      return index >= _parts ? bytes : bytes / _parts * index / cut * cut;
    };
    auto const inputs = std::span{_or_inputs}.subspan(part * _inputs, _inputs);
    auto const out    = first_block(position, _inputs);
    for (auto offset = share(part); offset < share(part + 1);) {
      auto const block  = offset / _block_bytes;
      auto const within = offset % _block_bytes;
      auto const count  = std::min(_block_bytes - within, share(part + 1) - offset);
      for (std::size_t input = 0; input < _inputs; ++input) {
        inputs[input] =
          _window_blocks->at(first_block(position, input) + block).subspan(within, count);
      }
      sirius::memory::host_or(inputs, _window_blocks->at(out + block).subspan(within, count));
      offset += count;
    }
  }

  /// Cross, second half: every receiving leader copies the union from the host into its chunk.
  void submit_upload(std::size_t sequence)
  {
    auto const& piece   = _chunks[sequence];
    auto const position = sequence % _window;
    // The single input's slot when it is passed through, else the host OR's output slot.
    auto const* const passed =
      _inputs == 1 ? std::to_address(std::ranges::find_if(_islands, &island_state::input))
                   : nullptr;
    for (auto& entry : _islands) {
      if (!entry.uploads) { continue; }
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{entry.device}};
      auto const stream = entry.upload->get().get();
      if (passed != nullptr) {
        check_cuda(cudaStreamWaitEvent(stream, passed->down_done[position].get(), 0),
                   "cudaStreamWaitEvent(download)");
      }
      submit_host_copy(stream,
                       chunk_of(entry.leader, piece),
                       piece.bytes,
                       first_block(position, passed != nullptr ? 0 : _inputs),
                       false);
      check_cuda(cudaEventRecord(entry.up_done[position].get(), stream), "cudaEventRecord(upload)");
    }
  }

  /// Fan-out: every other member copies its leader's chunk once that chunk is the union.
  void submit_fan_out(std::size_t sequence)
  {
    auto const& piece = _chunks[sequence];
    for (auto const& entry : _islands) {
      // A leader that neither uploads nor merges was waited for once, before the first chunk.
      auto const* const settled = entry.uploads            ? &entry.up_done[sequence % _window]
                                  : !entry.sources.empty() ? &entry.merged[sequence % 2]
                                                           : nullptr;
      for (auto const member : entry.others) {
        rmm::cuda_set_device_raii guard{rmm::cuda_device_id{device_of(member)}};
        if (settled != nullptr) {
          check_cuda(cudaStreamWaitEvent(stream_of(member), settled->get(), 0),
                     "cudaStreamWaitEvent(fan-out)");
        }
        check_cuda(cudaMemcpyPeerAsync(chunk_of(member, piece),
                                       device_of(member),
                                       chunk_of(entry.leader, piece),
                                       entry.device,
                                       piece.bytes,
                                       stream_of(member)),
                   "cudaMemcpyPeerAsync(fan-out)");
      }
    }
  }

  /// One host OR handed to the executor.
  struct or_job {
    union_run* owner;
    std::size_t position;
    std::size_t bytes;
  };

  /// Below this many bytes a part is not worth another thread.
  static constexpr std::size_t minimum_part_bytes = std::size_t{128} << 10;

  union_request const& _request;
  union_or_runner _runner;
  union_report _report;
  std::vector<chunk> _chunks;     // Every chunk of every array, in submission order
  std::size_t _slot_bytes   = 0;  // The largest chunk: the size of one scratch or host slot
  std::size_t _inputs       = 0;  // Contributing islands
  bool _crosses_host        = false;
  std::size_t _window       = 0;                       // Chunks in flight per host input
  std::size_t _window_slots = 0;                       // Host slots per chunk in flight
  std::size_t _block_bytes  = 0;                       // Size of one pinned block
  std::size_t _slot_blocks  = 0;                       // Pinned blocks per host slot
  unsigned _parts           = 1;                       // Parts of one host OR
  std::vector<std::span<std::byte const>> _or_inputs;  // Per part: its view of every input
  sirius::cuda::device_copy_batch _copies;
  std::vector<::cuda::stream_ref> _joined;  // The caller's stream, then every other stream used
  std::vector<int> _joined_devices;         // The GPU of each stream after the caller's
  std::vector<device_event> _completed;     // Per stream after the caller's: its work is complete
  // Released after the islands, whose streams read and write it.
  cucascade::memory::fixed_multiple_blocks_allocation _window_blocks;
  std::vector<island_state> _islands;  // The root's island first
};

void validate(union_request const& request)
{
  auto const members = request.members.size();
  bool valid = members != 0 && members == request.island_ids.size() && request.root < members &&
               members <= std::numeric_limits<std::uint8_t>::max();
  for (auto const& shape : request.shapes) {
    valid = valid && shape.chunk_bytes != 0 && shape.chunk_bytes % sizeof(uint4) == 0 &&
            shape.bytes % sizeof(uint4) == 0;
  }
  for (auto const& member : request.members) {
    valid = valid && member.space != nullptr && member.arrays.size() == request.shapes.size();
  }
  if (!valid) { throw accumulation_invariant_error{"inconsistent array union request"}; }
}

}  // namespace

std::optional<union_report> unite(union_request const& request, union_or_runner runner)
{
  validate(request);
  std::unique_ptr<union_run> run;
  try {
    run = std::make_unique<union_run>(request, runner);
    if (!run->prepare()) { return std::nullopt; }
  } catch (std::bad_alloc const&) {
    // Nothing was submitted: a refused host allocation only refuses the call.
    return std::nullopt;
  }
  try {
    join_on_failure(run->streams(), [&] {
      run->submit();
      run->wait();
    });
  } catch (unjoined_gpu_work const&) {
    // Unfinished work may still touch the workspace: never free it or reuse its streams.
    (void)run.release();
    throw;
  }
  return run->take_report();
}

}  // namespace sirius::op::detail
