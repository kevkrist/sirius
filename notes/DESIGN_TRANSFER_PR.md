# DESIGN_TRANSFER_PR: production design of the transfer PR (stacked on #2570) [xfer-PR]

2026-11-27. Architect output. No code was written, built or run for this file. Code refs are `pr2570` = `ae939824`
(read with `git show`), prototypes `proto/xfer-d2d` (`b7d0eb81`) and `proto/xfer-host`.
Inputs: PROTO_XFER_D2D, VERIFY_PROTO_XFER_D2D, PROTO_XFER_HOST, VERIFY_PROTO_XFER_HOST, MB_TRANSFER, pr_split,
USER DECISIONS in OPPORTUNITIES.md.
Reading caveat: the three AgentDocs books were sampled (principle / guideline summaries), not read cover to cover
(context budget). The four reference engines were not consulted: none has a multi-GPU transfer layer.

**Amended 2026-11-28 after step 0** (measuring spike `proto/xfer-compose`, notes/PROTO_XFER_COMPOSE.md): result and decision in
section 14; sections 2, 3.4, 4, 5, 6, 10, 11, 12, 13 changed where marked "(step 0)". Q1 is decided: **CPU OR**. The user's
answers to Q2 / Q4 / Q6 / Q8 are recorded in section 13.

Status of every claim below: **[M]** measured and verified in the notes, **[m]** measured in the standalone
microbench only, **[D]** design decision of this document (unmeasured), **[?]** open question for a human (section 13).

---

## 1. Goal, scope, non-goals

Goal: replace the Bloom finish transports of `accumulated_bloom_builder::finish_impl` (today: all-or-nothing choice
between the PEER `chunk_pipeline` and a serial HOST-staged star, 186 ms/GiB on this machine [M]) by **one** mechanism
that uses device-to-device copies inside every P2P-capable GPU group ("island") and a pipelined host hop between
islands; the same mechanism with only singleton islands is the no-P2P path.

In scope
- Topology discovery (islands from real pool peer access).
- One transfer mechanism for the accumulated Bloom finish (merge + cross + fan-out), replacing BOTH tip transports.
- Workspace reservation, degradation, failure paths, observability, tests, benchmark gates.

Non-goals (each has a reason; see section 13 where a human may overrule)
- Replica relay (`replica_relay`), `host_bounce_scope`, batched exact-key gather: zero TPC-H effect [M]
  (VERIFY_PROTO_XFER_HOST section 7: replica switch off = same times), tens of microseconds per filter [M]. Not built.
- Reduce tree / hypercube inside one large island (full P2P, G >= 4): model only, no machine to measure.
- Chunk size clamp (2-8 MiB, owned by #2569), where the finish runs (#2570), eager scans (S6). Untouched.
- No env switches, no modes, no size thresholds, no leg-graph framework.

Separability: everything sits below `accumulated_bloom_builder::finish()`. No change in the session, publisher logic,
planner or PARTITION, apart from the renamed report fields the publisher logs/counts (section 9).

---

## 2. Design in one page

Vocabulary
- **Member**: one replica GPU of the publication (one `accumulation_partial`).
- **Island**: a set of members with mutual pool peer access between every two of them. Singletons are islands.
- **Leader**: the one member of an island that talks to the host. The root leads its island.
- **Contributing**: a member that received at least one `enqueue_add` (`partial::contributed`); an island contributes
  if any member does. A non-contributing array is all zero.
- **Input**: a contributing island's leader (a source of the cross stage). `Kc` = number of inputs, `K` = islands.

Per chunk, three stages; a stage that has nothing to do does not exist for that publication:

| Stage | Where | What | Exists when |
|---|---|---|---|
| A merge | inside an island, P2P | each contributing non-leader is peer-copied into the leader's scratch and ORed into the leader's array (GPU kernel `or_bloom_chunk`) | island has a contributing non-leader |
| B cross | between leaders, through pinned host slots | each input downloads its chunk; the host ORs the `Kc` slots on the CPU (`Kc >= 2`) or passes the single slot through (`Kc == 1`); every leader whose chunk is not yet the union uploads it | `K >= 2` and `Kc >= 1` |
| C fan-out | inside an island, P2P | every non-leader peer-copies the leader's final chunk | island has a non-leader |

Degenerate cases fall out of the same code, no branches by machine class:

| Topology | K | Stages | Equivalent to |
|---|---|---|---|
| 1 GPU | 1 | none | nothing moves |
| full P2P | 1 | A + C | today's PEER `chunk_pipeline` (which is deleted; gate: no regression) |
| no P2P | G singletons | B only | `proto/xfer-host` `host_or` / `fanout` [M 45-58 ms/GiB] |
| this machine {0,1},{2,3} | 2 | A + B + C | `proto/xfer-compose` mode `compose` [M 51.7-55.5 ms/GiB, engine tie with `pipe`, section 14]; `proto/xfer-d2d pipe` is the GPU-OR variant of B [M 52] |
| odd sizes, e.g. {0,1},{2} or {0,1,2},{3} | 2 | A/C only where an island has >= 2 members | same code |
| zero contributions | any | none | nothing moves (all arrays zero) |

Why the cross stage ORs on the CPU (the one real choice in this design; Q1, **decided by step 0: CPU OR**, section 14)
- It is the only cross stage that is flat in `K`: every leader link carries B up and B down whatever `K` is.
  A GPU-side cross OR (the `pipe` exchange, or a star through the root) makes some leader receive `(Kc-1) x B`:
  optimal at `K = 2`, 2x worse at 4 singletons (gpuor 95 vs hostor 45 ms/GiB [M]), worse beyond.
  The prototype had three hand-written GPU cross variants and only a serial one for `K > 2`.
- It needs **no device scratch for the cross stage**. Device scratch remains only for stage A. That removes the
  remote cross-scratch lease of `pipe` (32 MiB on a GPU the task does not run on) and makes every device-memory
  refusal degradable without losing the filter (section 6).
- At `K = 2` it ties with the GPU exchange on this machine, in the microbench (51.7-53.4 vs 51.6-52.5 ms/GiB at >= 174 MiB)
  and in the engine (+3 ms over 8 queries, n = 16 per arm) [M, step 0] (same port load: each leader 2 x B in, 2 x B out).
- Cost: a bounded CPU worker facility and per-chunk host waits on the publishing thread when `Kc >= 2`
  (section 5.4). Verified workable in the engine by `proto/xfer-host` (suite -394 ms [M]), better under link
  contention than serial legs (x2.1 vs x4.3 [M]).
- The composition (P2P merge + 2-input host OR + P2P fan-out) is measured inside the engine: hard gate passed (step 0,
  section 14). Two rules came out of it and are now part of the design: stage A is enqueued ahead of the host loop
  (section 5) and a chunk OR uses `min(8, 2 x Kc)` threads, never more (section 3.4).

Design principles applied: one deep module with a one-call interface (complexity pulled down; `finish_impl` shrinks
to validation + one call); policy (plan: who leads, what degrades) separated from mechanism (executor: streams,
events, slots); special cases defined out of existence (machine classes and OOM fallbacks are just different plans);
no pass-through layers; no information leak of transport details into the builder or publisher.

---

## 3. Module boundaries

```
src/memory/peer_islands.{hpp,cpp}            topology: islands from pool peer access            (new, no CUDA kernels)
src/memory/host_or.{hpp,cpp}                 CPU word-OR kernel + bounded worker facility       (new)
src/op/dynamic_filter/detail/union_plan.{hpp,cpp}   pure policy: roles + workspace needs, host-only, GPU-free (new)
src/op/dynamic_filter/detail/union_transfer.hpp     the deep module's interface                 (new)
src/cuda/union_transfer.cu                   executor: streams, events, slots, kernels          (new; takes or_bloom_chunk)
src/cuda/sirius_dynamic_bloom_filter.cu      finish_impl: validate, build request, call, make_filters (shrinks ~250 lines)
```

Dependency direction: `bloom builder -> union_transfer -> {union_plan, peer_islands, host_or, device_copy_batch,
runtime_stream_pool, scoped_replica_reservation}`. `union_transfer` knows nothing about Bloom filters, keys,
sessions or publication: it makes N equally-shaped byte arrays on N GPUs equal to their bitwise OR. That is the
general-purpose statement; the Bloom builder is its only (special-purpose) client.

Deleted: `accumulated_bloom_builder::impl::chunk_pipeline`, the HOST loop in `finish_impl`, `enqueue_or_chunk` (moves),
`publication_transport`, the `force_host` parameter. `find_pair_without_pool_access` loses its last DF caller (keep
it only if manager initialization still uses it; otherwise delete with `pool_peer_access_scan.hpp`).

### 3.1 `sirius::memory::peer_islands` (topology)

```cpp
/// Island id per space, in the order of `spaces`. Two spaces share an id only if each pool grants the other's
/// device (`try_grant_pool_access` both ways). Ids are dense, assigned in order of first appearance.
[[nodiscard]] std::vector<std::uint8_t> peer_islands(std::span<cucascade::memory::memory_space const* const> spaces);
```

- Source of truth: `try_grant_pool_access` (the per-pair primitive every existing peer copy already uses). No PCI
  parsing, no `nvidia-smi`, no hard-coded pairs.
- Algorithm: mutual-access matrix (N x (N-1) grant calls), then a deterministic greedy clique cover in ascending
  input order: a space joins the first island all of whose members it has mutual access with, else opens a new one.
  Always legal (every island is a clique). Exact for the pair / full / none topologies; for a non-transitive access
  graph it is legal but not necessarily minimal, which costs speed only, never correctness. Document that.
- Called **once per builder, in `try_create`** (publication begin, off the finish path) and stored in the builder.
  No process-wide cache: grants are documented as uncached ("callers must coordinate later permission changes"), and
  a per-builder derivation has no invalidation problem. This removes the prototype's N^2 grant calls per finish.
- Throws what `try_grant_pool_access` throws (`cucascade::cuda_error`, `std::invalid_argument`); `try_create`
  propagates like any other creation error.
- A subset of an island is an island, so restricting a publication to fewer GPUs needs no recomputation.

### 3.2 `detail::union_plan` (policy; pure, host-only)

```cpp
struct union_plan {
  struct island {
    std::uint8_t leader;                 // index into the request's members
    std::vector<std::uint8_t> sources;   // contributing non-leaders (stage A)
    std::vector<std::uint8_t> others;    // every non-leader (stage C)
    bool contributes;
  };
  std::vector<island> islands;           // the root's island first
  [[nodiscard]] std::size_t inputs() const noexcept;        // Kc
  [[nodiscard]] bool crosses_host() const noexcept;         // islands.size() >= 2 && inputs() >= 1
  [[nodiscard]] bool moves_nothing() const noexcept;        // inputs() == 0 || one member

  /// Roles for `island_ids` (peer_islands), contribution flags and the root's index.
  static union_plan make(std::span<std::uint8_t const> island_ids, std::span<bool const> contributed, std::size_t root);
  /// The same plan with island `index` replaced by one singleton island per member (degradation, section 6).
  [[nodiscard]] union_plan split(std::size_t index) const;
};
```

Leader rule (deterministic): the root leads its island; any other island is led by its first contributing member in
member order, else by its first member. A contributing leader saves its own merge leg and, when it is the island's only
contributor, the island needs no scratch at all.

The plan is fixed before the first enqueue and never changes afterwards (keeps the tip's contract).

### 3.3 `detail::union_transfer` (the deep module)

```cpp
struct union_member {
  cucascade::memory::memory_space* space;      // GPU space (scratch lease, stream lease)
  ::cuda::stream_ref stream;                   // orders every use and free of this member's arrays (partial stream)
  std::span<std::byte* const> arrays;          // one pointer per array; null for an empty array
  bool contributed;
};
struct union_shape { std::size_t bytes; std::size_t chunk_bytes; };

struct union_request {
  std::span<union_member const> members;
  std::span<std::uint8_t const> island_ids;    // peer_islands of members' spaces, or a refinement (tests)
  std::span<union_shape const> shapes;         // per array, identical on every member
  std::size_t root;                            // index of the member on the current device
  ::cuda::stream_ref root_stream;              // the publication stream
  rmm::device_async_resource_ref root_scratch; // borrows the task's reservation
  cucascade::memory::fixed_size_host_memory_resource* host;  // pinned fixed-block pool
};

struct union_report {
  std::uint8_t islands;            // K
  std::uint8_t host_inputs;        // Kc if the host was crossed, else 0
  std::uint8_t window_chunks;      // W in effect (0 without a cross stage)
  bool degraded;                   // an island was split or the window was shrunk
  std::size_t host_window_bytes;   // pinned bytes held
  std::vector<std::size_t> device_scratch_bytes;  // per member, tracked allocation charge
};

/// Makes every member's arrays equal to the bitwise OR of all members' arrays, then waits for all of it.
/// @return nullopt, with nothing enqueued and nothing held, iff the minimum pinned window was refused.
/// @throw accumulation_cuda_error / unjoined_gpu_work as today's finish; on unjoined_gpu_work the module has
///        leaked its own workspace and the caller must retain the arrays.
template <host_or_executor Executor>
[[nodiscard]] std::optional<union_report> unite(union_request const& request, Executor& or_executor);
```

- One call. Preparation, enqueue, the host OR loop, the final wait, cleanup and every degradation are inside.
- `host_or_executor` is a C++20 concept (policy injection without inheritance):
  `e.run(parts, body)` runs `body(i)` for `i in [0, parts)` and returns when all ran; `body` is `noexcept`;
  `e.max_parts()` bounds the split. Production passes the engine facility (3.4); tests pass an inline executor
  (single thread, no pool), which also keeps the fault-injection binary free of worker threads.
- `island_ids` is data, so tests can pass any **refinement** of the real topology (pretend less P2P): all singletons
  (no-P2P machine), `{0,1},{2},{3}` (three islands), `{0,1},{2}` (odd). `unite` validates a debug-only precondition
  that the ids are a refinement of real access (a peer copy on a non-peer pair would silently be driver-staged).
  This replaces `finish_impl`'s `force_host` flag: `force_host` == all singletons.

### 3.4 `sirius::memory::host_or` (CPU OR)

```cpp
/// out[i] = in[0][i] | ... | in[n-1][i] over one byte range. Runtime-dispatched AVX-512 with non-temporal stores
/// (fenced before return), generic 8-byte fallback. noexcept, no allocation. `out` must not alias an input.
void host_or(std::span<std::span<std::byte const> const> inputs, std::span<std::byte> out) noexcept;

class host_or_workers;   // models host_or_executor
```

Requirements on `host_or_workers` (the prototype's private pool is not acceptable as is):
1. Engine-lifetime owner next to the runtime stream pools (`sirius_memory_reservation_manager`), reached the same way
   `runtime_stream_pool::acquire` is; no function-local static threads.
2. Bounded: `T` workers, compile-time default 8, capped at `hardware_concurrency / 4`, minimum 1 (the calling thread
   always takes a part). **One chunk job is split into `min(T, 2 x Kc)` parts, not `T`** (step 0) [M]: with two inputs
   3-4 threads are the best point (50.7-52.8 ms/GiB at >= 281 MiB), 8 cost 5-11%, 12 cost 10-47%; with four inputs 6-8
   are best and 4 cost 23%. With 8 threads the OR itself is 2x faster than with 4 and the finish is still slower, so the rule is about
   not disturbing the transfers, not about OR speed. Cause not isolated (the prototype's helpers spin 500 us between
   jobs): the developer repeats this sweep with the production workers (step 2) and keeps the rule only if it still
   holds. No config knob in this PR [?] Q3.
3. No thread spins while no `unite` is active. Spin-then-park inside an active job is allowed and must be bounded.
4. Concurrent publications share the workers per chunk job; a publication is never queued behind another one's
   whole finish (the prototype serialised whole jobs under one mutex).
5. NUMA: out of scope (single-node machine); the slots come from the root's host space as today.
6. (step 0) The OR needs its parts' worth of free cores: process pinned to 2 cores -> 88-96 ms/GiB instead of 52 (4 or 8
   cores: no loss) [M]. Still 2x the tip; accepted, stated in the PR.

---

## 4. Transfer plan (bytes and resources)

Per array of B bytes, chunk c = `chunk_bytes` (2-8 MiB), W = window depth (chunks in flight per input, 2).

| Resource | Amount | Held |
|---|---|---|
| Device scratch, root leader | `min(2, chunks) x |sources of root island| x c` from the task reservation (`root_scratch`). Always <= `publication_scratch_bytes` (`2 x (G-1)` slots), which stays unchanged as the task's upper bound | during `unite` |
| Device scratch, other leader | `min(2, chunks) x |sources| x c`, zero when the leader is the island's only contributor; leased with `scoped_replica_reservation::try_acquire` on that GPU at prepare time | during `unite` |
| Pinned host window | `Kc >= 2`: `(Kc + 1) x W x c` (inputs + one OR output per chunk in flight); `Kc == 1`: `1 x W x c`; no cross stage: 0. From the existing fixed-block HOST pool, 1 MiB blocks, one `device_copy_batch` per leg (as today) | during `unite` |
| Streams | per leader with sources: one ingress stream `I`; per input leader: one download stream `X` (step 0: as measured; not the partial stream); per leader that uploads: one upload stream `U`. Leased from `runtime_stream_pool` (no `cudaStreamCreate` per finish) | during `unite` |
| CPU | `min(T, 2 x Kc)` threads including the publishing thread, only while `Kc >= 2` | per chunk |

This machine, G = 4, islands {0,1},{2,3}, all contributing, 8 MiB chunks: root scratch 16 MiB, other leader 16 MiB,
pinned 48 MiB [M, step 0, engine debug line] (tip: 8 MiB scratch, 8 MiB pinned; `pipe`: 32 / 32 / 32 MiB [M]).
Single contributor: pinned 16 MiB, no scratch anywhere [M]. No P2P, G = 4: 0 device scratch,
80 MiB pinned. Full P2P: the tip's PEER numbers, no pinned memory.

Byte flow, G = 4, two pair islands, all contributing: P2P 2 x B (merge) + 2 x B (fan-out); host 2 x B up + 2 x B
down. Tip: 4 x B up + 6 x B down, serial. Each leader port: 2 x B in, 2 x B out (the bound of this topology, ~39
ms/GiB [m]).

Single contributor (the S = 0 shape of the 57 MiB q3 / q5 / q8 filters): `Kc == 1`, pass-through. The holder downloads
once; every other leader uploads from that slot; islands fan out by P2P. No CPU OR, no host wait before the final one,
no device scratch.

---

## 5. Streams and event ordering

Notation: `P(m)` = member m's partial stream (exists today; orders every use and free of m's arrays). Per island with
leader L: `I(L)` ingress stream, `X(L)` download stream (input leaders), `U(L)` upload stream. Chunks are numbered globally across arrays; `p = k mod 2`
(scratch parity), `w = k mod W` (window slot). `cudaStreamWaitEvent` binds to the most recent `cudaEventRecord` at
call time, so an event object is re-recorded per chunk and every wait on it is enqueued before its next record.

Once, before the first chunk
- every member: record `ready(m)` on `P(m)` (all inserts and the zero fill precede it; pre: every `enqueue_add`
  returned).

Enqueue schedule (step 0; this is what was measured, and the lookahead is worth 5-10% [M]). Only the host OR blocks
the publishing thread, so everything else is enqueued ahead of it:
- `Kc >= 2`: before the host OR of chunk k, the downloads of chunks `<= k + W - 1` and the merges of chunks
  `<= k + W + 1` are enqueued (merges run 2 chunks ahead of the downloads). The bound 2 is exact: download j must be
  enqueued after merge j and before merge j + 2, because it waits on `merged[j mod 2]`, which merge j + 2 re-records.
  Coupling the merge to the host loop (merge k + 2 enqueued after the OR of chunk k) fails the microbench gate
  (55-63 instead of 52-55 ms/GiB).
- `Kc <= 1`: nothing blocks; chunks are enqueued one after the other in the order below.

Per chunk k (dependencies; enqueue order within one chunk = this order)
1. **Merge** (island with sources): `I(L)` waits `ready(s)` of each source (first use) and `merged[p]` of chunk k-2
   (scratch slot reuse); peer-copies each source chunk into `scratch[p][i]`; records `ingress_done[p]`.
   `P(L)` waits `ingress_done[p]`; runs the OR kernel into L's chunk; records `merged[p]`.
2. **Download** (input leaders, cross stage only): on `X(L)` (step 0: a stream of its own, so the leader's next merge
   OR is not queued behind a download); `X(L)` waits `ready(L)` once, then per chunk `merged[p]` (leader with
   sources) and, in pass-through only, the `up_done` events of the chunk k-W uploads that read slot w; batch copy
   chunk -> input slot `in[L][w]`; records `down_done[L][w]`.
3. **Host OR** (`Kc >= 2`): the publishing thread `cudaEventSynchronize`s `down_done[.][w]` of chunk k and the
   `up_done` events of chunk k-W (they read output slot `out[w]`), then ORs the `Kc` input slots into `out[w]` through
   the executor. `Kc == 1`: nothing here; the upload source is the single input slot and step 4 waits `down_done`
   as a stream event instead of on the host.
4. **Upload** (every leader whose chunk is not the union: all leaders when `Kc >= 2`; all but the input when
   `Kc == 1`): `U(L)` waits `ready(L)` (first use; a non-contributing leader has no download to order it) and, in
   pass-through, the input's `down_done`; batch copy slot -> L's chunk; records `up_done[L][w]` and `settled(L)`.
   For a leader that does not upload, `settled(L)` is `merged[p]` (has sources) or `ready(L)`.
5. **Fan-out**: each non-leader m: `P(m)` waits `settled(L)`; peer-copies L's chunk into m's chunk.

Final: host-synchronize every `I`, every `X`, every `U`, the publication stream, then every `P` (one wait each; the prototype
measured 19 per finish against 1932 at tip [M]). Only then are slots, scratch and stream leases released.

Why separate streams: a leader's two host-link directions overlap (download on `X`, upload on `U`), an upload held by
an event never blocks that GPU's downloads, and ingress overlaps the previous chunk's OR. This is the event graph of
both verified prototypes (VERIFY_PROTO_XFER_D2D section 5, VERIFY_PROTO_XFER_HOST 5.1: no ordering hole found [M]).

Device currency: every enqueue on another GPU runs under `rmm::cuda_set_device_raii`; every event is created on the
device of the stream that records it; every leased stream and event is owned by an RAII member that makes its device
current in its destructor (fixes the prototype's constructor exception-safety note).

---

## 6. Workspace acquisition, degradation, failure paths

Preparation acquires everything and enqueues nothing. Order (single pass, no loops, no retries):

1. `union_plan::make`. `moves_nothing()` -> return a report, done.
2. For each island with sources, acquire its merge scratch (root: `root_scratch`; other leader: lease).
   **Refused (`rmm::out_of_memory` / no lease) -> `plan = plan.split(island)`, `degraded = true`.** The island's
   members become singleton leaders that go through the host; they need no device scratch. Other islands keep P2P.
3. Acquire the pinned window for the final plan's `Kc` at `W = 2`. Refused -> `W = 1`, `degraded = true`.
   Refused again -> release everything, return `nullopt` (the caller skips the filter, as today for a refused
   workspace).
4. Lease streams, create events. A CUDA failure here throws; nothing was enqueued, RAII releases.

| Event | Tip | This design |
|---|---|---|
| Root merge scratch refused (task reservation) | PEER -> HOST loop; if its 1 slot is refused too, filter skipped | root island split, publication completes through the host, `degraded` |
| Other leader's scratch lease refused | n/a (prototype: whole gain lost silently) | that island split, rest unchanged, `degraded` |
| Pinned window refused at W = 2 | n/a | W = 1, `degraded` |
| Pinned window refused at W = 1 | filter skipped at 1 chunk (8 MiB) | filter skipped at `(Kc + 1)` chunks (16-40 MiB at G = 4) |
| CUDA error before any enqueue | throw | throw |
| CUDA error after enqueue | `join_on_failure` over partial streams, then free | `join_on_failure` over **every** stream the call touched (all `I`, `U`, `P`, publication), **then** release slots / scratch / leases in one place |
| A stream cannot be joined | leak staging + scratch, `leak_storage` | module leaks its slots, scratch and stream leases; throws `unjoined_gpu_work`; builder sets `leak_storage` |
| `cudaEventSynchronize` fails inside the host OR loop | n/a | same path as "CUDA error after enqueue" |

Rules that make the failure paths simple
- **One drain, one release.** No sub-object drains or frees in its destructor while another object's stream may
  still use its memory. This removes by construction the verified defect of `~island_reduce` (window of island 0
  released before island 1's streams were drained) and the reliance on an outer join for the fan-out copies.
- Degradation is plan refinement, not a second code path: after `split`, the same executor runs.
- The tip's serial HOST loop is **deleted**, not kept as a floor (Q4: user agrees). Its only merit was a 1-chunk pinned floor.
- Device-memory refusal never loses the filter any more. This is a behaviour change (Q2: accepted by the user, the tests
  of 10.2 change): on a full-P2P machine
  under GPU memory pressure the Publishing Task now spends a host bounce where the tip gave the filter up at once.

---

## 7. Correctness invariants (each needs a test or a stated proof in the PR description)

- **I1 Union.** On return, every member's array equals the bitwise OR of all members' arrays as they were after
  their last insert. Independent of topology, leader choice, degradation, W, executor.
- **I2 Read after inserts.** Every read of a member's chunk follows its zero fill and inserts (`ready(m)` or the
  member's own stream order).
- **I3 Overwrite after read.** A source's chunk is overwritten (fan-out) only after its ingress copy; a leader's
  chunk is overwritten (upload) only after its own download of that chunk.
- **I4 Slot reuse.** A scratch slot is refilled only after the OR that read it (`merged[p]` of k-2); an input slot
  only after its readers (host OR by control flow; pass-through uploads by `up_done`); an output slot only after
  the uploads that read it (host wait on `up_done` of k-W).
- **I5 Fixed plan.** No transport or role changes after the first enqueue.
- **I6 Quiescence.** On return or on any exception other than `unjoined_gpu_work`, no work enqueued by the call is
  pending on any stream, and all workspace is back (pinned pool and device bytes at their baseline).
- **I7 Nothing for nothing.** Zero contributions or one member: no copy, no allocation, no host wait.
- **I8 Peer legality.** A peer copy is only issued between members of one island (mutual grant established).
- **I9 Bounded workspace.** The amounts of section 4 are upper bounds; root scratch never exceeds
  `publication_scratch_bytes`.
- **I10 No change for one island.** With one island the call sequence, bytes and scratch equal the tip's PEER
  pipeline (leased streams aside).

---

## 8. Interface changes in the Bloom builder

- `finish(root, stream, scratch_mr)`: signature unchanged. Doc: "peer copies inside every group of GPUs with mutual
  pool access; pinned HOST staging with a host OR between groups".
- `publication::transport` (`publication_transport {PEER_COPY, HOST_STAGED}`) -> `publication::route`:
  `enum class publication_route { NONE, PEER, PEER_AND_HOST, HOST }` derived from the report (`K == 1` -> PEER,
  all singletons -> HOST, else PEER_AND_HOST; nothing moved -> NONE) plus `bool degraded`.
- `finish_impl(root, stream, scratch_mr, force_host, observation)` (test seam) ->
  `finish_impl(root, stream, scratch_mr, island_override, executor, observation)`; `workspace_observation` becomes
  the `union_report` (per-GPU scratch, window bytes, W, K, Kc).
- `try_create` computes and stores `peer_islands` of the targets.
- `publication_scratch_bytes`: unchanged (still a valid upper bound for the root).
- `build_filter_construction` / publisher: `host_staged` := route is `HOST` or `PEER_AND_HOST` (keeps the meaning of
  `accumulation_publications_host_staged` and of the "(Bloom through HOST staging)" log suffix).

## 9. Observability

- New counter `accumulation_publications_degraded` (in `dynamic_filter_stats.hpp`, next to `..._host_staged`).
- One INFO line per degraded publication (which island was split or that W shrank, bytes asked); the normal route
  line stays at DEBUG and names K, Kc, W, scratch per leader, window bytes. Fixes "silent fallback" (verifier).
- NVTX: keep `dynfilter::accum::finish`; add `dynfilter::accum::host_or` around each CPU OR (as the prototype).
- No process-wide observation globals, no test counters in production code.

---

## 10. Unit-test plan

Fixtures: 4 GPUs on this machine; forced topologies through `island_override` (refinements only):
`{0,1},{2,3}` (real), `{0},{1},{2},{3}`, `{0,1},{2},{3}`, `{0},{1},{2,3}`, 3 GPUs `{0,1},{2}` and `{0},{2,3}`,
2 GPUs `{0,1}` and `{0},{1}`, 1 GPU. Not formable here: an island of 3 or more (Q6: user accepts this box as sufficient).

### 10.1 New tests

| Test | Checks |
|---|---|
| `peer_islands` unit (host logic with an injected access matrix + one real-device case) | pair / full / none / odd / non-transitive graph -> legal cliques, deterministic ids; real box yields {0,1},{2,3} |
| `union_plan` properties (pure, no GPU; every topology up to G = 8 x every contribution mask x every root) | partition; root leads its island; leader rule; `sources` = contributing non-leaders; `Kc`; `split` yields singletons and never touches other islands; workspace formula |
| Bit identity (I1) | sizes 96 B .. 70 MiB with ragged tails, 2 keys of different size (1 to 8+ chunks, both parities, W wrap) x all 16 contribution masks x every root x every forced topology x {pool executor, inline executor}; every replica `memcmp`'d with the OR of the partials read before `finish` |
| Inserts in flight (I2) | `finish` started with zero fill and inserts delayed (delay injection of `scoped_cuda_status_fault`), 4 GPUs, 3 sources |
| Concurrent publications | >= 8 builders finishing at once on shared workers; identity + baseline memory |
| Degradation: root scratch refused | failing `scratch_mr`: publishes, identical bits, `degraded`, zero device scratch on the split island, route HOST / PEER_AND_HOST |
| Degradation: other leader lease refused | fill that GPU's reservation: same checks; the root island still uses P2P |
| Degradation: window | HOST pool sized for W = 1 only: publishes with `window_chunks == 1`; pool below the minimum: `nullopt`, partials unchanged, nothing enqueued |
| Workspace bounds (I9) and quiescence (I6) | report equals the section 4 formulas per topology; pinned + device bytes back at baseline after success, refusal, and every injected fault |
| Fault injection on >= 3 GPUs without forcing singletons (new cases in `test_dynamic_filter_accumulation_fault.cpp`) | failing `copy_peer` on the other leader's ingress, failing `wait_event`, failing `record_event`, failing `copy_batch` mid-chunk on a download and on an upload, failing `synchronize_event` inside the host OR loop, `synchronize_stream_unjoined` on a leased upload stream: error escapes with its CUDA cause, `all_submitted_streams_joined()`, baseline memory (or leak + `leak_storage` for the unjoined case) |
| One island = tip PEER (I10) | G = 2 on {0,1}: copy counts, scratch bytes, no pinned allocation |
| I7 | zero contributions and 1 GPU: zero copies, zero allocations (fault harness call recorder) |
| I8 | debug-build precondition: an override that is not a refinement is rejected |

### 10.2 Existing tests whose expectations change (the ones `proto/xfer-host` broke, plus two fault cases)

| Test (file:line at `pr2570`) | Pins today | New expectation |
|---|---|---|
| `accumulated publication matches the complete partial union and bounded workspace` (`test_dynamic_filter_accumulation.cpp:2078`; asserts `:2131`, `:2134`) | host window == 1 chunk, device scratch == 1 chunk | union checks unchanged; workspace == section 4 formula for the fixture's topology (generate over forced topologies) |
| `HOST scratch uses the unused task reservation and returns its excess` (`:2182`, assert `:2208`) | the HOST route takes 1 chunk of device scratch from the task reservation | the invariant moves to where scratch still exists: rename to "merge scratch uses the unused task reservation and returns its excess", run on a topology whose root island has a source; add: all-singleton route takes zero device scratch |
| `optional publication scratch OOM deposits without rescheduling the PARTITION task` (`test_in_task_publication.cpp:852`, assert `:895`) | exactly one device-scratch request | with a P2P root island: one request, refused -> still deposits without rescheduling AND the filter is published through the host (`degraded == 1`); with singletons: zero requests |
| `publication scratch allocation failure retires ordinary-pool partials` (`:921`, assert `:953`) | failing scratch allocator -> `finish` returns no publication | scratch failure now publishes (assert that); keep the retirement coverage by exhausting the pinned window instead (the one remaining refusal) -> no publication, partials retired |
| `a tracked task thread admits a merge scratch larger than its reservation as its task would` (5th failure, `hostor` arm) | `accumulations_skipped_admission == 1` after a refused PEER scratch | `skipped_admission == 0`, `degraded == 1`, filter published (Q2 accepted) |
| `HOST publication submits every target upload before waiting for any` (`test_dynamic_filter_accumulation_fault.cpp:177`) | exactly 1 submission during an injected delay (the tip loop's order) | replace by the pipeline property: while chunk k's upload on one leader is delayed, the downloads of chunk k+1 and the other leaders' uploads of chunk k are submitted (`submissions_during_delay >=` that count) |
| `HOST exhaustion refuses preparation without changing any partial` (`:232`) | 1-chunk window refused -> refusal | two steps: W = 2 refused -> publishes at W = 1; minimum refused -> refusal, partials unchanged |

Tests using `force_host` switch to the all-singleton override; tests asserting `transport` switch to `route`.
Suites that must stay green: single-GPU `[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]` (638 cases),
4-GPU `[dynamic_filter][multi_gpu],[device_transfer],[publisher],[construction_plan],[physical_partition],[build_filter]`
(159 cases), `sirius_cuda_fault_test "[cuda_status_fault][multi_gpu]~[.]"`, `[multi_gpu]~[dynamic_filter]~[.]`,
then `pixi run make test`.

---

## 11. Benchmark gates (all under `flock $R/machine.lock`; config untouched; protocol as in the VERIFY notes)

Microbench (lifecycle MB4, builder-level `finish`, median of 5 after 1 warm-up)

| Gate | Pass |
|---|---|
| G = 4, all four contributing, 281 and 1434 MiB | <= 56 ms/GiB (step 0 spike 52.5 / 51.7 [M], `pipe` 52 [M], tip 186) |
| G = 4, all four, 57 MiB | <= 62 ms/GiB (spike 55.5, `pipe` 55-56) |
| G = 4, root only (S = 0), 57-1434 MiB | <= 44 ms/GiB (spike 39-41, tip 61) |
| G = 4, one pair / one GPU of the other pair | <= 46 ms/GiB (spike 40-44) |
| G = 4, GPUs 0+2 (two inputs, no merge), 174-1434 MiB | <= 46 ms/GiB (spike 42.7-44.4; 57 MiB: <= 52, spike 48.9) |
| G = 4, forced all singletons, all four | <= 60 ms/GiB (spike 42.8-54.3, `hostor` 45-58) |
| G = 4, all four, 4 and 16 MiB (step 0) | <= 0.60 / 1.65 ms (spike 0.53 / 1.51; `pipe` 0.39 / 1.06; tip 0.87 / 3.37). The CPU OR's known weak spot; must stay under the tip |
| OR thread sweep 2 / 4 / 8 at Kc = 2 and 4 / 8 at Kc = 4 (step 0) | the `min(T, 2 x Kc)` point is within 3% of the best point of the sweep |
| G = 2 on the P2P pair {0,1} (full-P2P stand-in) | within 2% of tip (5.57 / 28.4 ms at 281 / 1434 MiB) |
| Fixed cost, 1 MiB, root only and all four | <= tip + 0.10 ms (stream leases instead of creates) |
| Contention: second process saturating the links | degradation factor <= 2.5 (hostor x2.1, tip x4.3) |
| Small host: process pinned to 8 and to 4 cores (`taskset`), all four, 281 / 1434 MiB | within 3% of the unpinned run (spike: 52.0-52.8 at 8, 51.5-52.5 at 4). Pinned to 2 cores: report (spike 88-96); fail if slower than tip |

Engine (TPC-H SF1000, 4 GPUs; ABBA, 4 fresh processes per query, 1 warm + 5 hot, first hot dropped, n = 8 per arm)

| Gate | Pass |
|---|---|
| Suite vs `pr2570` tip | <= -370 ms (-2.4%) (both prototypes: -370..-410 [M]; step 0 spike: -364 on the 8 queries it ran) |
| q21 / q3 / q7 | <= -190 / -33 / -33 ms (spike -203 / -38 / -39) |
| q8 / q10 / q9 | <= -18 / -20 / -15 ms (spike -25 / -32 / -25; q10 and q9 are noisy, n >= 16) |
| Controls q6, q2, q15, q17, q19, q20 | within +-3 ms |
| No query slower than tip beyond its same-arm spread | required |
| Results | `result.txt` md5 identical for 22 queries (q11: sorted content) |
| nsys q21, q3 | `accum::finish` <= 90 / 25 ms; host-staged bytes 4 x B; nothing in flight after `finish` |
| Debug-log pass | every multi-GPU Bloom finish logs route PEER_AND_HOST, 0 degraded lines |

Not gateable here (state in the PR): full P2P at G >= 3, no P2P with one uplink per GPU, more than 2 real islands,
behaviour under eager scans (S6) and a concurrent CONCAT move.

---

## 12. Ordered implementation plan (developer agent; architect -> developer -> reviewer per repo rule)

Branch from `pr2570` tip; nothing is pushed to sirius-db; no PR is opened.

0. **DONE 2026-11-28, gate PASSED, Q1 = CPU OR (section 14).** Composition spike (throwaway branch `proto/xfer-compose`, bfbf2a67; it is a
   reference for the event graph, not code to copy: private thread pool, raw streams, no failure paths). Was: take `proto/xfer-d2d`, replace
   stage B of `island_reduce` by `proto/xfer-host`'s `host_or` / `fanout` over the island leaders (section 3 seam of
   both notes). Run the microbench gates of section 11 (rows 1-4) and ABBA on q21, q3, q7.
   **Pass -> continue. Fail (worse than `pipe` by more than the gate) -> stop and escalate Q1** (the alternative is a
   GPU-OR cross stage for `Kc <= 2` plus host OR beyond, i.e. two cross stages).
1. `peer_islands` + unit tests. No behaviour change.
2. `host_or` kernel + `host_or_workers` + unit tests (bit identity against a scalar reference, sizes with ragged
   tails; worker lifecycle; concurrent jobs). No caller yet. In step 8, repeat the OR thread sweep of PROTO_XFER_COMPOSE 3.1 with
   these workers and confirm the `min(T, 2 x Kc)` rule (section 3.4 item 2).
3. `union_plan` + exhaustive property tests (GPU-free).
4. `union_transfer`, stage A + C only (one island), with `unite`'s final interface. Switch `finish_impl` to it for
   the one-island case and delete `chunk_pipeline`. Gate: I10 test, G = 2 microbench within 2%, full 4-GPU suite
   green (multi-island publications still take the tip HOST loop in this commit).
5. Stage B (download / host OR / upload, pass-through) + degradation + single drain/release. Delete the tip HOST
   loop, `force_host`, `publication_transport`. Update the tests of 10.2; add 10.1 identity, degradation, bounds.
6. Fault-injection cases of 10.1 (>= 3 GPUs).
7. Observability (section 9): route, `degraded` counter, log lines, NVTX; publisher / stats call sites.
8. Microbench gates, then engine gates (section 11). Record tables in `notes/IMPL_TRANSFER_PR.md`.
9. Docs: `docs/super-sirius/` DF / memory pages (transport description), header comments. Reviewer agent on the
   whole diff (review harness: adversarial tests, sanitizers, microbench).

Commits 1-3 are independent and reviewable alone; 4 and 5 each leave the tree green, so the PR can be read commit by
commit. Estimated shape: about +1100 / -450 lines of source, +900 of tests.

---

## 13. Questions: decided, and still open

### 13.1 Decided (2026-11-27 user answers; Q1 closed by step 0 on 2026-11-28)

- **Q1. Where is the cross-island OR done?** User: "measure in the engine first". Measured (section 14): CPU OR and GPU OR
  tie in the microbench and in the engine. **Decision: CPU OR for every `Kc >= 2`** (one mechanism, flat in `K`, no cross
  scratch, same code as the no-P2P path). The GPU exchange of `pipe` is not built. D2D stays inside the pairs (stages A
  and C), which is the user's 2026-11-19 direction; only the cross-pair OR is on the CPU.
- **Q2. Device-scratch refusal publishes through the host instead of skipping the filter.** User: accepted; **change the
  tests** (section 10.2, five tests).
- **Q4. Delete the tip's serial HOST loop.** User: delete it. Cost stated: under pinned-pool exhaustion the filter is
  skipped at 16-40 MiB of free pinned memory instead of 8 MiB.
- **Q6. Coverage of machines we do not have.** User: this 4-GPU box is sufficient for now. Islands of >= 3 GPUs, real
  no-P2P with one uplink per GPU and > 2 real islands are covered by forced refinements and the pure plan tests only;
  state that in the PR. No full-P2P run is required before merge.
- **Q8. Behaviour under eager scans (S6).** User: the transfer PR **lands independently** of S6. No combined gate.

### 13.2 Still open (unanswered; the design proceeds on the recommendation, each is reversible)

- **Q3. CPU worker budget and owner.** Proceeding with: `T` = 8 workers (cap `cores / 4`), `min(T, 2 x Kc)` parts per
  chunk (step 0), owned by the memory reservation manager, no config knob. Step 0 adds a fact: the finish needs
  `2 x Kc` free cores (4 here) or it slows down up to 1.8x (2 cores), and extra threads hurt. Knob or no knob?
  Different owner (an existing engine pool)?
- **Q5. Pinned window accounting.** Proceeding with: keep the tip's behaviour (window taken from the fixed-block HOST
  pool without a reservation; bounded, transient: 48 MiB here, 80 MiB at 4 singletons, `(G + 1) x 16 MiB` worst case).
  Charging it to a host reservation needs a reservation passed into `finish` (publisher change, touches #2570 code).
- **Q7. Replica relay / stage-once replica copies / batched exact-key gather** are left out (no measured TPC-H effect,
  <= 0.4 ms per filter). Proceeding with: not in this PR. Confirm, or ask for a follow-up commit.
- **Q9. Non-transitive access graphs.** Proceeding with: greedy clique cover (legal, not necessarily minimal; no known
  machine). Alternative: minimum clique cover (exponential in general, trivial at G <= 8).

---

## 14. Step 0 result (2026-11-28; full tables in notes/PROTO_XFER_COMPOSE.md)

Spike: branch `proto/xfer-compose` (bfbf2a67, worktree `$R/wt-xfer-compose`), one binary, `SIRIUS_PROTO_XFER_D2D=`
`compose` (this design: P2P merge -> host stage with CPU OR -> P2P fan-out) or `pipe` (GPU OR on the two leaders,
`proto/xfer-d2d` unchanged) or unset (tip loop). Runs `$R/runs/xfer_compose`. Config untouched, all under the lock.

Correctness: bit identity against the tip loop, 832 cases (4 GPUs, two 3-GPU topologies, 4 GPUs forced singletons),
0 mismatches. TPC-H `result.txt` md5 identical across the three arms for the 8 queries (12 processes each).

Microbench, builder-level `finish`, G = 4, root GPU 0, ms/GiB at 57 / 174 / 281 / 1434 MiB:

| batches on | tip loop | GPU OR (`pipe`) | CPU OR (`compose`) | CPU OR, forced singletons |
|---|---|---|---|---|
| all four (`Kc` = 2) | 188 / 186 / 186 / 186 | 54.9 / 52.5 / 52.0 / 51.6 | 55.5 / 53.4 / 52.5 / 51.7 | 54.3 / 46.6 / 45.0 / 42.8 |
| GPUs 0+2 (`Kc` = 2, no merge) | 104 / 103 / 103 / 103 | 44.8 / 43.4 / 43.3 / 43.0 | 48.9 / 44.4 / 43.7 / 42.7 | 48.9 / 42.7 / 41.3 / 39.7 |
| pair 0-1, root only, GPU 2 only (`Kc` = 1) | 104 / 61 / 104 | 40.3-44.6 / 39.2-41.0 / 39.3-41.0 | same event graph, same numbers | 42.8-53.4 / 38.7-43.2 / 38.7-43.4 |

Small arrays, all four, 4 / 16 MiB: tip 0.87 / 3.37 ms, GPU OR 0.39 / 1.06, CPU OR 0.53 / 1.51.

Engine, TPC-H SF1000, 4 GPUs, three arms, two mirrored ABBA blocks, n = 16 hot iterations per arm and query, ms:

| q | tip | CPU OR | GPU OR | CPU OR - tip | GPU OR - tip | CPU - GPU |
|---|---|---|---|---|---|---|
| 21 | 1445.2 | 1242.1 | 1242.1 | -203.1 | -203.1 | 0.0 |
| 3 | 659.8 | 622.2 | 621.5 | -37.6 | -38.3 | +0.7 |
| 7 | 745.6 | 707.0 | 706.9 | -38.6 | -38.7 | +0.1 |
| 8 | 936.9 | 911.7 | 911.7 | -25.2 | -25.3 | +0.1 |
| 10 | 853.7 | 821.4 | 824.3 | -32.3 | -29.4 | -3.0 |
| 9 | 1422.7 | 1398.0 | 1393.0 | -24.6 | -29.7 | +5.1 |
| 6 (control) | 379.8 | 379.8 | 380.0 | -0.1 | +0.1 | -0.2 |
| 2 (control) | 149.2 | 146.6 | 146.3 | -2.7 | -2.9 | +0.2 |
| sum | 6593 | 6229 | 6226 | **-364** | **-367** | **+3** |

Engine DEBUG finish lines (same binary, three modes): q21 1394 MiB 261 -> 76.2 (GPU OR) / 74.8 ms (CPU OR), i.e.
192 -> 56 / 55 ms/GiB; q3 280 MiB 57.0 -> 19.9 / 19.7; q7 278 MiB 51.3 -> 14.5 / 14.3; q8 174 MiB 35.3 -> 12.5 / 12.4;
q10 109 MiB 20.4 -> 6.05 / 6.26; q9 166 MiB 31.7 -> 9.73 / 9.81. Publishing-thread time inside the CPU-OR finish of
q21: 31 ms OR + 12 ms event waits of 75 ms.

Hard gate of step 0: microbench rows 1-4 pass (and row 5), q21 / q3 / q7 pass, controls within +-3 ms. **PASS.**

Decision: **CPU OR** (tie within noise; it is the simpler and more robust of the two: one cross stage for any island
count, the same code is the no-P2P path, no cross scratch on a remote GPU, every device-memory refusal degradable).
GPU OR would add a second cross stage for 2 islands on top of the host OR that 3+ islands and no-P2P machines need
anyway.

What step 0 changed in this document
1. Stage A must be enqueued ahead of the host loop (section 5, "Enqueue schedule"): worth 5-10%, the first cut failed
   the gate without it.
2. Downloads run on a stream of their own `X(L)`, not on `P(L)` (sections 4, 5): this is the measured form.
3. OR parts per chunk = `min(T, 2 x Kc)` (section 3.4): more threads make the finish slower (8 instead of 4 at
   `Kc` = 2: +5-11%; 12: +10-47%).
4. New gates (section 11): small arrays (4 / 16 MiB), two inputs without merge, thread-rule check, 4- and 8-core pin.
5. Known limits, accepted: 4-16 MiB two-input arrays 35-40% slower than a GPU OR would be (<= 0.45 ms each); with fewer
   free cores than OR parts the finish degrades (2 cores: 88-96 ms/GiB).

Not measured in step 0: the other 14 queries, nsys, link contention with a second process, behaviour under S6, forced
3-island topologies in the engine. On this machine the all-singleton form (no P2P used at all) is faster in the
microbench than the island form (42.8 vs 51.7 ms/GiB at 1434 MiB, all four); known since PROTO_XFER_HOST, the user's
2026-11-19 direction (D2D inside pairs) stands and the design follows it.
