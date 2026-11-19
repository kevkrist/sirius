#!/usr/bin/env python3
"""Adds the event-chained two-island exchange (mode "pipe") to island_reduce. Usage: pipe.py <file>"""
import sys
p = sys.argv[1]
s = open(p).read()
def rep(a, b, n=1):
    global s
    assert s.count(a) == n, (s.count(a), a)
    s = s.replace(a, b)

rep('''enum class proto_xfer_mode { off, star, exchange };''',
    '''enum class proto_xfer_mode { off, star, exchange, pipe };''')
rep(''' * "1" / "exchange": the same, and with two islands the leaders exchange both ways at once.
 */''',
    ''' * "1" / "exchange": the same, and with two islands the leaders exchange both ways at once.
 * "pipe": the two-island exchange with every leg ordered by events and one host wait at the end.
 */''')
rep('''  if (value == "star") { return proto_xfer_mode::star; }
''',
    '''  if (value == "star") { return proto_xfer_mode::star; }
  if (value == "pipe") { return proto_xfer_mode::pipe; }
''')
rep('''    std::unique_ptr<rmm::cuda_stream> xfer;       // Leader GPU: every cross-island leg
''',
    '''    std::unique_ptr<rmm::cuda_stream> xfer;       // Leader GPU: every cross-island leg
    std::unique_ptr<rmm::cuda_stream> up;         // Leader GPU, "pipe" only: uploads and their OR
''')
rep('''    std::byte* cross_scratch{};                   // One slot: a chunk arriving from another island
''',
    '''    std::byte* cross_scratch{};  // Chunks arriving from another island: one slot, "pipe" two
''')
rep('''    window_type window;
  };''',
    '''    std::optional<cuda_event> down_done[2];  // "pipe": the leader's chunk is in its window
    std::optional<cuda_event> up_done[2];    // "pipe": the other leader's window was read
    window_type window;
    window_type window_odd;  // "pipe": the window of odd chunks
  };''')
rep('''      _exchange{mode == proto_xfer_mode::exchange && _islands.size() == 2}
''',
    '''      _pipe{mode == proto_xfer_mode::pipe && _islands.size() == 2},
      _exchange{_pipe || (mode == proto_xfer_mode::exchange && _islands.size() == 2)}
''')
rep('''      if (entry.xfer) { (void)cudaStreamSynchronize(entry.xfer->value()); }
      (void)cudaStreamSynchronize(entry.leader->stream->value());
      entry.scratch.reset();
      entry.window.reset();
      entry.ingress.reset();
      entry.xfer.reset();
''',
    '''      if (entry.xfer) { (void)cudaStreamSynchronize(entry.xfer->value()); }
      if (entry.up) { (void)cudaStreamSynchronize(entry.up->value()); }
      (void)cudaStreamSynchronize(entry.leader->stream->value());
      entry.scratch.reset();
      entry.window.reset();
      entry.window_odd.reset();
      entry.ingress.reset();
      entry.xfer.reset();
      entry.up.reset();
''')
rep('''    return (parities() * _islands.front().sources.size() + 1) * _slot;
''',
    '''    return (parities() * _islands.front().sources.size() + cross_slots()) * _slot;
''')
rep('''  [[nodiscard]] bool exchanges() const noexcept { return _exchange; }
''',
    '''  [[nodiscard]] bool exchanges() const noexcept { return _exchange; }
  [[nodiscard]] bool pipelined() const noexcept { return _pipe; }
''')
rep('''      entry.settled.emplace();
      entry.leader_ready.emplace();
''',
    '''      entry.settled.emplace();
      entry.leader_ready.emplace();
      if (_pipe) {
        entry.up = std::make_unique<rmm::cuda_stream>(rmm::cuda_stream::flags::non_blocking);
        for (std::size_t parity = 0; parity < 2; ++parity) {
          entry.down_done[parity].emplace();
          entry.up_done[parity].emplace();
        }
      }
''')
rep('''      } else if (auto const bytes = merge_bytes + (_exchange ? _slot : 0); bytes != 0) {''',
    '''      } else if (auto const bytes = merge_bytes + (_exchange ? cross_slots() * _slot : 0);
                 bytes != 0) {''')
rep('''        _host_bytes += entry.window->size_bytes();
      }
''',
    '''        _host_bytes += entry.window->size_bytes();
        if (_pipe && parities() == 2) {
          entry.window_odd = host->allocate_multiple_blocks(_slot);
          if (entry.window_odd->size_bytes() < _slot) {
            throw accumulation_invariant_error{
              "accumulated publication HOST window is smaller than a chunk"};
          }
          _host_bytes += entry.window_odd->size_bytes();
        }
      }
''')
# run(): the pipe mode enqueues everything, then waits once.
rep('''    if (_contributing != 0) { enqueue_merge(0); }
    for (std::size_t sequence = 0; _contributing != 0 && sequence < _chunks.size(); ++sequence) {
''',
    '''    if (_pipe) {
      for (auto& entry : _islands) {
        if (_contributing == 0) { break; }
        rmm::cuda_set_device_raii guard{rmm::cuda_device_id{entry.leader->device_id}};
        check_cuda(cudaStreamWaitEvent(entry.up->value(), entry.leader_ready->get(), 0),
                   "cudaStreamWaitEvent(island leader ready)");
      }
      for (std::size_t sequence = 0; _contributing != 0 && sequence < _chunks.size(); ++sequence) {
        enqueue_merge(sequence);
        cross_pipe(sequence);
        enqueue_fan_out(_chunks[sequence]);
      }
    } else if (_contributing != 0) {
      enqueue_merge(0);
    }
    for (std::size_t sequence = 0;
         !_pipe && _contributing != 0 && sequence < _chunks.size();
         ++sequence) {
''')
rep('''      check_cuda(cudaStreamSynchronize(entry.xfer->value()), "cudaStreamSynchronize(island xfer)");
    }
''',
    '''      check_cuda(cudaStreamSynchronize(entry.xfer->value()), "cudaStreamSynchronize(island xfer)");
      if (entry.up) {
        check_cuda(cudaStreamSynchronize(entry.up->value()), "cudaStreamSynchronize(island up)");
      }
    }
''')
rep('''  [[nodiscard]] static std::byte* chunk_of(partial const& owner, chunk const& piece) noexcept
''',
    '''  [[nodiscard]] std::size_t cross_slots() const noexcept { return _pipe ? parities() : 1; }

  [[nodiscard]] static std::byte* chunk_of(partial const& owner, chunk const& piece) noexcept
''')
# submit(): explicit stream.
rep('''  void submit(island& entry, window_type& window, std::byte* device_bytes, std::size_t bytes,
              bool to_host)
  {
''',
    '''  void submit(island& entry, window_type& window, std::byte* device_bytes, std::size_t bytes,
              bool to_host)
  {
    submit_on(entry, entry.xfer->value(), window, device_bytes, bytes, to_host);
  }

  void submit_on(island& entry, cudaStream_t stream, window_type& window, std::byte* device_bytes,
                 std::size_t bytes, bool to_host)
  {
''')
rep('''    check_cuda(_copies.enqueue(::cuda::stream_ref{entry.xfer->value()}),
               "device_copy_batch::enqueue(island staging)");''',
    '''    check_cuda(_copies.enqueue(::cuda::stream_ref{stream}),
               "device_copy_batch::enqueue(island staging)");''')
rep('''  /// (C) Peer copy of the leader's final chunk to every other GPU of its island.
''',
    '''  /**
   * @brief (B) Two islands, no host wait: per chunk each contributing leader downloads into its
   * own window of that chunk parity (`xfer` stream); the other leader uploads from it into a
   * scratch slot and ORs it into its chunk (`up` stream). A window is refilled only after the
   * upload that read it. SEAM: same contract as `cross_star` / `cross_exchange`.
   */
  void cross_pipe(std::size_t sequence)
  {
    auto const& piece = _chunks[sequence];
    auto const parity = sequence % 2;
    auto const window = [&](island& entry) -> window_type& {
      return parity == 0 ? entry.window : entry.window_odd;
    };
    auto const wait_for = [](cudaStream_t stream, std::optional<cuda_event> const& event) {
      check_cuda(cudaStreamWaitEvent(stream, event->get(), 0), "cudaStreamWaitEvent(island pipe)");
    };
    auto const record = [](std::optional<cuda_event> const& event, cudaStream_t stream) {
      check_cuda(cudaEventRecord(event->get(), stream), "cudaEventRecord(island pipe)");
    };
    // Leader chunk -> its window, after its island's merge and after the reader of chunk - 2.
    auto const down = [&](island& from, island& to) {
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{from.leader->device_id}};
      auto const stream = from.xfer->value();
      if (!from.sources.empty()) { wait_for(stream, from.merged[parity]); }
      if (sequence >= 2) { wait_for(stream, to.up_done[parity]); }
      submit_on(from, stream, window(from), chunk_of(*from.leader, piece), piece.bytes, true);
      record(from.down_done[parity], stream);
    };
    // The other leader's window -> this leader: ORed in, or in place when this island is empty.
    auto const up = [&](island& to, island& from, bool merge) {
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{to.leader->device_id}};
      auto const stream = to.up->value();
      wait_for(stream, from.down_done[parity]);
      if (merge) {
        auto* const slot = to.cross_scratch + parity * _slot;
        submit_on(to, stream, window(from), slot, piece.bytes, false);
        record(to.up_done[parity], stream);
        // The OR follows this leader's own download and its island's merge of the chunk.
        wait_for(stream, to.down_done[parity]);
        if (!to.sources.empty()) { wait_for(stream, to.merged[parity]); }
        enqueue_or_chunk(chunk_of(*to.leader, piece), slot, _slot, 1, piece.bytes, stream);
      } else {
        submit_on(to, stream, window(from), chunk_of(*to.leader, piece), piece.bytes, false);
        record(to.up_done[parity], stream);
      }
      record(to.settled, stream);
    };
    auto& left  = _islands[0];
    auto& right = _islands[1];
    if (left.contributed && right.contributed) {
      down(left, right);
      down(right, left);
      up(left, right, true);
      up(right, left, true);
      return;
    }
    auto& from = left.contributed ? left : right;
    auto& to   = left.contributed ? right : left;
    down(from, to);
    {
      rmm::cuda_set_device_raii guard{rmm::cuda_device_id{from.leader->device_id}};
      record(from.settled, from.xfer->value());
    }
    up(to, from, false);
  }

  /// (C) Peer copy of the leader's final chunk to every other GPU of its island.
''')
rep('''  bool _exchange;
  std::vector<chunk> _chunks;''',
    '''  std::vector<chunk> _chunks;''')
rep('''  std::size_t _slot;             // Bytes of one slot: the largest chunk of any key
''',
    '''  std::size_t _slot;             // Bytes of one slot: the largest chunk of any key
  bool _pipe;
  bool _exchange;
''')
rep('''            island_route = reduce.exchanges() ? "island peer copies + HOST exchange"
                                              : "island peer copies + HOST star";''',
    '''            island_route = reduce.pipelined()   ? "island peer copies + HOST pipelined exchange"
                           : reduce.exchanges() ? "island peer copies + HOST exchange"
                                                : "island peer copies + HOST star";''')
open(p, 'w').write(s)
print("ok")
