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

## 4. (a) Lifecycle microbench MB4: Bloom finish at G=4, tip vs prototype

`runs/xfer_d2d/bin/mb4.sh` -> `mb/mb4_g4.log`. Builder level (`try_create / enqueue_add / finish`), root = GPU 0, 1 key,
default chunk clamp (8 MiB at these sizes), `finish()` wall time, median of 5 after 1 warm-up; `off` = the tip code
path in the same binary. Min-max spread of the 5 repetitions <= 1.9% in every cell (last column = worst mode).

| batches on | Bloom MiB | off ms (ms/GiB) | star ms (ms/GiB) | exchange ms (ms/GiB) | pipe ms (ms/GiB) | pipe vs off | spread max-min, worst mode |
|---|---|---|---|---|---|---|---|
| root only (GPU 0) | 57 | 3.42 (62) | 3.46 (62) | 3.46 (62) | 2.28 (41) | 1.50x | 1.1% |
| root only (GPU 0) | 174 | 10.36 (61) | 10.41 (61) | 10.39 (61) | 6.76 (40) | 1.53x | 0.4% |
| root only (GPU 0) | 281 | 16.73 (61) | 16.77 (61) | 16.75 (61) | 10.84 (40) | 1.54x | 0.6% |
| root only (GPU 0) | 1434 | 85.29 (61) | 85.26 (61) | 85.09 (61) | 54.90 (39) | 1.55x | 0.3% |
| pair 0-1 | 57 | 5.79 (104) | 3.68 (66) | 3.68 (66) | 2.47 (44) | 2.34x | 0.9% |
| pair 0-1 | 174 | 17.52 (103) | 10.78 (63) | 10.75 (63) | 7.07 (42) | 2.48x | 0.3% |
| pair 0-1 | 281 | 28.34 (103) | 17.26 (63) | 17.21 (63) | 11.23 (41) | 2.52x | 0.4% |
| pair 0-1 | 1434 | 144.63 (103) | 86.94 (62) | 86.79 (62) | 56.48 (40) | 2.56x | 0.2% |
| GPUs 0+2 (one per pair) | 57 | 5.79 (104) | 5.84 (105) | 3.67 (66) | 2.50 (45) | 2.31x | 0.9% |
| GPUs 0+2 (one per pair) | 174 | 17.51 (103) | 17.69 (104) | 10.87 (64) | 7.41 (44) | 2.36x | 0.5% |
| GPUs 0+2 (one per pair) | 281 | 28.35 (103) | 28.55 (104) | 17.50 (64) | 11.92 (43) | 2.38x | 0.5% |
| GPUs 0+2 (one per pair) | 1434 | 144.56 (103) | 145.67 (104) | 88.77 (63) | 60.42 (43) | 2.39x | 0.1% |
| all 4 | 57 | 10.46 (188) | 6.13 (110) | 3.93 (71) | 3.09 (56) | 3.38x | 1.9% |
| all 4 | 174 | 31.58 (186) | 18.08 (106) | 11.28 (66) | 8.97 (53) | 3.52x | 0.7% |
| all 4 | 281 | 51.02 (186) | 29.02 (106) | 18.02 (66) | 14.34 (52) | 3.56x | 0.4% |
| all 4 | 1434 | 259.76 (185) | 146.71 (105) | 90.53 (65) | 72.59 (52) | 3.58x | 0.3% |
| GPU 2 only (not the root) | 57 | 5.79 (104) | 3.52 (63) | 3.48 (62) | 2.26 (41) | 2.56x | 1.4% |
| GPU 2 only (not the root) | 174 | 17.51 (103) | 10.58 (62) | 10.48 (62) | 6.75 (40) | 2.59x | 0.3% |
| GPU 2 only (not the root) | 281 | 28.29 (103) | 17.05 (62) | 16.88 (62) | 10.85 (40) | 2.61x | 0.6% |
| GPU 2 only (not the root) | 1434 | 144.62 (103) | 86.62 (62) | 85.87 (61) | 54.92 (39) | 2.63x | 0.2% |

Reading:

- **All four GPUs contributed (the shape of every large TPC-H Bloom: q21 1.4 GiB, q3 281 MiB, q8 174 MiB):
  185-188 -> 52-56 ms/GiB, 3.4-3.6x.** `pipe` lands exactly on MB_TRANSFER's `c2` (52 ms/GiB, 55-59 at 57 MiB): the
  engine prototype reproduces the standalone microbench. q21-size: 260 -> 73 ms; q3-size: 51 -> 14 ms.
- Decomposition of that gain (1434 MiB): per-island P2P merge + fan-out with today's host loop between the leaders
  (`star`) 260 -> 147 ms; both directions at once (`exchange`) -> 91 ms; event ordering instead of host waits
  (`pipe`) -> 73 ms. So ~60% of the gain needs no change to the host staging code at all, ~30% comes from using
  both uplinks at once, ~10% from removing the per-chunk host waits.
- Batches on one pair (mask 3) or one GPU of the other pair (mask 4): 103 -> 40 ms/GiB (2.5-2.6x).
- Root only (S=0; q3/q5/q8 57 MiB filters): 61 -> 39-41 ms/GiB (1.5x) with `pipe`; `star` / `exchange` = tip (one
  bounce either way, P2P fan-out replaces two uploads but the root's port is the limit: it sends B to the host and B
  to its partner). 39 = the same floor MB_TRANSFER section 4 found for every fan-out variant it tried. Untried
  variant with every link at 1 x B (root -> host -> {partner, other leader}, other leader -> its partner by P2P):
  modelled ~20-25 ms/GiB; worth <= 1 ms per 57 MiB filter, not built.
- One contributor per pair (mask 5): `star` = tip (the star through the root is still two serial bounces);
  `exchange` / `pipe` 2.3-2.4x.

## 5. (b) TPC-H SF1000 ABBA timing, tip binary vs prototype with `SIRIUS_PROTO_XFER_D2D=pipe`

INTERIM (replaced by the final table when the queue finishes): gates passed (section 7); first blocks, fresh process
per query, 1 warm + 5 hot, first hot dropped, ABBA, n = 8 per arm:

| q | tip med ms (n) [min-max] | proto med ms (n) [min-max] | delta ms | delta % | per-block deltas ms | results |
|---|---|---|---|---|---|---|
| 3 | 658.7 (8) [653-668] | 621.9 (8) [615-626] | -36.8 | -5.59 | -36.8 | identical |
| 4 | 324.3 (8) [321-327] | 310.6 (8) [308-312] | -13.7 | -4.23 | -13.7 | identical |
| 7 | 746.9 (8) [741-752] | 710.9 (8) [707-713] | -35.9 | -4.81 | -35.9 | identical |
| 8 | 936.1 (8) [927-942] | 912.4 (8) [904-913] | -23.7 | -2.54 | -23.7 | identical |
| 9 | 1420.5 (8) [1404-1438] | 1394.1 (8) [1365-1409] | -26.4 | -1.86 | -26.4 | identical |
| 10 | 849.7 (8) [840-869] | 820.2 (8) [813-833] | -29.5 | -3.47 | -29.5 | identical |
| 21 | 1442.0 (8) [1429-1454] | 1244.1 (8) [1227-1258] | -197.9 | -13.72 | -197.9 | identical |
| sum | 6378 | 6014 | -363.9 | -5.71 | | |

## 6. (a, continued) Small Blooms (fixed cost), exact kinds, full-P2P pair

### 6.1 Fixed cost: small Blooms at G=4 (`mb/mb4_small.log`, median of 9)

| batches on | Bloom MiB | off ms (ms/GiB) | star ms (ms/GiB) | exchange ms (ms/GiB) | pipe ms (ms/GiB) | pipe vs off | spread max-min, worst mode |
|---|---|---|---|---|---|---|---|
| root only (GPU 0) | 1 | 0.08 (85) | 0.13 (128) | 0.12 (127) | 0.15 (159) | 0.53x | 14.8% |
| root only (GPU 0) | 4 | 0.27 (70) | 0.33 (84) | 0.32 (82) | 0.27 (69) | 1.01x | 30.0% |
| root only (GPU 0) | 16 | 1.07 (68) | 1.14 (73) | 1.14 (73) | 0.74 (47) | 1.45x | 13.2% |
| root only (GPU 0) | 32 | 1.98 (64) | 2.04 (65) | 2.04 (65) | 1.35 (43) | 1.47x | 6.6% |
| all 4 | 1 | 0.27 (279) | 0.27 (272) | 0.21 (218) | 0.23 (236) | 1.18x | 16.4% |
| all 4 | 4 | 0.87 (223) | 0.62 (160) | 0.46 (117) | 0.38 (97) | 2.30x | 14.1% |
| all 4 | 16 | 3.38 (216) | 2.09 (134) | 1.42 (91) | 1.03 (66) | 3.28x | 4.1% |
| all 4 | 32 | 6.13 (196) | 3.67 (118) | 2.38 (76) | 1.83 (59) | 3.35x | 4.4% |

- The island reduce has a fixed cost of about 0.05-0.07 ms per finish (3 streams + 6-10 events per island, 2-4 pinned
  windows, one reservation + scratch allocation on the other leader). Root-only 1 MiB: 0.08 -> 0.15 ms (slower);
  break-even at 4 MiB; all-four case wins from 1 MiB up. Production: pool the streams (`runtime_stream_pool`), or keep
  the tip loop below ~4 MiB when at most one GPU contributed.
- Repetition spread at these sizes is 4-30% (sub-millisecond calls); the medians are stable to ~10 us.

### 6.2 Exact kinds (session level, `[mb4x]` exact slices and `[mb5]` small builds, G=4, 1 key, median of 7)

`publish_us` = `publish_if_final` wall (gather + filter construction + replica fan-out); `shot_repl_us` = one-shot
filter `replicate_to_devices` (source GPU 0, 3 targets). Switch off -> `pipe` (relay on); off run twice (run-to-run
difference <= 2%).

| Case (kind chosen by `auto`) | publish us: off -> on | one-shot replicate us: off -> on |
|---|---|---|
| 12 rows, 1 batch (small IN-list) | 165 -> 162 | 73 -> 60 |
| 1000 rows, 1 batch (small / hash IN-list) | 273 -> 254 | 115 -> 107 |
| 100k rows, 1 batch on GPU 0 (hash IN-list) | 654 -> 568 (-13%) | 429 -> 351 (-18%) |
| 100k rows, 16 batches spread over 4 GPUs | 892 -> 758 (-15%) | 429 -> 354 |
| 100k rows, 128 batches spread | 2129 -> 1988 (-7%) | 431 -> 346 |
| 10M rows on GPU 0 (accumulated Bloom, 19 MiB, S=0) | 1286 -> 1037 (-19%) | 1949 -> 1564 (-20%, one-shot Bloom relay) |
| 10M rows, 16 batches spread (Bloom, S=3) | 4067 -> 1344 (3.0x) | 1963 -> 1591 |

- MB4x (exact slices, 512 / 32768 rows in 1 / 64 / 512 batches): publish -10 to -50 us with the switch on
  (`exchange` run: 200 -> 181, 457 -> 431, 498 -> 449 us); the spread 512-slice cases stay at 5.3-5.8 ms
  (5365 -> 5296 us): the gather is unchanged, its cost is the per-slice synced bounce from the two cross-pair GPUs.
- The relay saves one of the two whole-payload host bounces of an exact / one-shot replica fan-out: -18..-20% of
  `replicate_to_devices`, tens of microseconds to 0.4 ms per filter. Not a suite item (as MB_TRANSFER predicted).
- Side finding: mode `exchange` (not `pipe`) made the 19 MiB S=0 session publish 0.2 ms slower (1291 -> 1500 us):
  2.4 MiB chunks, two host waits per chunk plus the fixed cost. `pipe` does not have this.

### 6.3 Full-P2P sanity: G=2 on the P2P pair 0-1 (`mb/mb4_g2.log`)

281 MiB: off 5.57 / 6.42 ms (S=0 / S=1), `pipe` 5.58 / 6.41 ms; 1434 MiB: off 28.37 / 31.85, `pipe` 28.36 / 31.84.
Identical: with one island `plan()` declines and the tip PEER pipeline runs.

