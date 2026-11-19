# PROTO_XFER_D2D: reap direct D2D where possible (per-pair transport + topology-aware reduce) [xfer-PR]

2026-11-19. Worktree `/opt/dlami/nvme/dfperf/wt-xfer-d2d`, branch `proto/xfer-d2d` on pr2570 `ae939824`
(+ cherry-pick of the lifecycle microbench). Scripts `$R/runs/xfer_d2d/bin/`, raw output `$R/runs/xfer_d2d/`.
Every build and GPU process ran under `flock $R/machine.lock`. Config untouched; one binary, env switch.
THROWAWAY PROTOTYPE. Everything here belongs to the separate transfer PR ([xfer-PR]), nothing to #2569 / #2570.

STATUS: see section 8 (what ran / what did not). Sections 1-3 are design, 4-7 measurements.

## 1. What the tip does, and what the prototype changes

Tip (`accumulated_bloom_builder::finish_impl`, `sirius_dynamic_bloom_filter.cu`): one transport for the whole
publication. `find_pair_without_pool_access(spaces)` finds any GPU pair without pool access -> the whole finish is
HOST staged: a star through the root, per chunk (2-8 MiB) each source `D2H, sync, H2D, sync, OR`, then root `D2H, sync`,
3 uploads + 3 syncs, one 1-chunk pinned window. On this machine (P2P only inside pairs 0-1 and 2-3) every multi-GPU
Bloom finish takes that path: 10 x B through 7 serial legs = 186 ms/GiB (MB_TRANSFER section 6).

Prototype, switch `SIRIUS_PROTO_XFER_D2D` (unset / `0` / `off` = tip code path, byte for byte):

| Value | Bloom finish | Replica fan-out (hash IN-list, small IN-list, one-shot Bloom) |
|---|---|---|
| `star` | islands; P2P merge + P2P fan-out inside each island; **today's HOST loop** (same `submit` + `wait` code) between the island leaders only | relay on |
| `exchange` (= `1`) | same; with two islands the two leaders exchange both ways at once (both download, 2 waits, both upload + OR, 2 waits) | relay on |
| `pipe` | same exchange, every leg ordered by events, two pinned windows per leader (chunk parity), one host wait at the end | relay on |

### 1.1 Bloom finish: `accumulated_bloom_builder::impl::island_reduce`

- `plan()`: partitions the publication's GPUs into **islands** = cliques of mutual pool access
  (`try_grant_pool_access` both ways, the same per-pair primitive `enqueue_device_copy` already uses). The root leads
  its island; another island is led by a contributing GPU if it has one (saves that GPU's merge leg). Returns nothing
  (-> tip transports, unchanged) for < 3 GPUs, one island (full P2P: tip PEER pipeline) or all-singleton islands (no P2P:
  tip HOST loop / the other agent's host pipeline).
- Per chunk:
  (A) **merge** inside each island: every contributing non-leader GPU is `cudaMemcpyPeerAsync`ed into the leader's
      scratch (leader `ingress` stream, two scratch parities as in the tip's `chunk_pipeline`) and ORed into the
      leader's array on the leader's partial stream (`or_bloom_chunk`, same kernel);
  (B) **cross**: the leaders make their chunks equal to the union through HOST staging (the only legs that leave an
      island). `cross_star` / `cross_exchange` / `cross_pipe`. THE SEAM (section 3);
  (C) **fan-out** inside each island: every non-leader GPU peer-copies the leader's final chunk on its own partial
      stream behind the leader's `settled` event.
- Byte count at G=4, all four contributing, B bytes per array: P2P 2 x B (merge) + 2 x B (fan-out); host 2 x B up +
  2 x B down. Tip: 4 x B up + 6 x B down, all through the host, serial. Only 2 of the 6 exchange legs touch the host
  per direction and the two uplinks (one per pair) work in parallel.
- Fallback: a refused scratch reservation on any leader -> the tip's own transports run instead (no state enqueued
  before `prepare()` succeeds). Zero contributions -> nothing moves (arrays are zero everywhere).
- Filters are published only after the final host waits (every island stream, then every partial stream), as today.

### 1.2 Replica copies: `detail::replica_relay` (`dynamic_filter_replica_transfer.{hpp,cu}`)

`enqueue_replica_copy` -> `enqueue_device_copy` already chooses per (source, destination): peer copy when the source
pool grants the destination, else whole-payload `D2H + sync + H2D + sync`. What it lacked is topology: on this machine
root 0 staged the payload through the host twice (for GPU 2 and for GPU 3). `replica_relay` keeps the per-pair choice
and adds the pair-local fan-out: a HOST-staged replica (complete on return) is offered as a relay; a later destination
without access to the source is peer-copied from the first relay whose pool grants it (GPU 3 from GPU 2). Used by the
three `replicate_to_devices` (hash IN-list, small IN-list, one-shot Bloom). One host bounce instead of two; the second
cross-pair replica becomes an asynchronous P2P copy.

### 1.3 Exact-key gather (`exact_key_stager::gather`)

No change needed for the transport choice: it already calls `enqueue_replica_copy` per written slice, i.e. per-pair
(P2P from the pair partner, HOST from the other pair). A topology-aware gather (pair partner relays the other pair's
slices) makes no sense for a gather: every slice has exactly one source and one destination. The remaining cost is the
per-slice `D2H, sync, H2D, sync` on the two cross-pair sources = the other agent's batched gather (PROTO_XFER_HOST 2.2).

## 2. Files changed (all [xfer-PR])

| File | Change |
|---|---|
| `src/cuda/sirius_dynamic_bloom_filter.cu` | `proto_xfer()` switch, `island_reduce` class (~620 lines), call site in `finish_impl` before the transport choice, route name in the debug log, relay in the one-shot Bloom `replicate_to_devices` / `copy_filter_storage` |
| `src/cuda/dynamic_filter_replica_transfer.cu`, `src/op/dynamic_filter/dynamic_filter_replica_transfer.hpp` | `replica_relay` (pick / offer / test counter) |
| `src/cuda/sirius_dynamic_in_list_filter.cu`, `src/cuda/sirius_dynamic_small_in_list_filter.cu` | use the relay in `replicate_to_devices` |
| `test/cpp/operator/bench_dynamic_filter_lifecycle.cpp` | MB4: contribution masks + mode loop; `[mbid]` Bloom bit-identity gate |
| `test/cpp/operator/test_sirius_dynamic_filter_mgpu.cpp` | `[xfer_relay]` replica identity gate |

Nothing in `src/op/dynamic_filter/dynamic_filter_publisher.cpp`, the session, the planner or PARTITION: the prototype
sits entirely below `finish()` / `replicate_to_devices()`, so it is separable from #2569 / #2570 by construction.

## 3. The seam with the host-bounce prototype (`proto/xfer-host`)

The two prototypes split the finish along one line:

- **This prototype owns the topology and the P2P legs**: which GPUs form an island, who leads, the intra-island merge
  and fan-out (stages A and C), and their event ordering.
- **The cross stage (B) is a black box with a 4-point contract**, implemented three times here (`cross_star`,
  `cross_exchange`, `cross_pipe`) and replaceable by the other agent's `host_pipeline`:
  1. inputs: the island leaders (`island::leader`), each holding its island's partial union of chunk k once its
     `merged[k % 2]` event (islands with sources) / `leader_ready` event (islands without) has fired;
  2. output: every leader's chunk k equals the global union;
  3. it records `island::settled` on a stream of the leader's GPU after the leader's chunk k is final (stage C waits
     on it);
  4. it owns its pinned windows and its device scratch slot(s) on each leader, sized in `prepare()` and reported
     through `host_bytes()` / `root_scratch_bytes()` / `remote_scratch_bytes()`.
- Mapping to `proto/xfer-host`: its `gpu_or` schedule over the 2 leaders = `cross_pipe`; its `host_or` schedule over
  the leaders (CPU OR of 2 inputs, uploads to the 2 leaders) is MB_TRANSFER's `pairhostor_pipe` minus the direct
  uploads to non-leaders. With N > 2 islands, `host_or` over N leaders is the right cross stage (here only the
  serial `cross_star` covers N > 2).
- Selection order in `finish_impl` once both land: one island -> PEER pipeline (tip); several islands, at least one
  with >= 2 GPUs -> `island_reduce` with the host pipeline as stage B; all singletons -> host pipeline on all GPUs;
  preparation failure -> tip loop.
- Replica copies compose by construction: `replica_relay` only changes *which source* a copy reads, the other
  agent's `host_bounce_scope` only changes *what the HOST route does*.

