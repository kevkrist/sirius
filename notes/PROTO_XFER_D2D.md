# PROTO_XFER_D2D: reap direct D2D where possible (per-pair transport + topology-aware reduce) [xfer-PR]

2026-11-19. Worktree `/opt/dlami/nvme/dfperf/wt-xfer-d2d`, branch `proto/xfer-d2d` on pr2570 `ae939824`
(+ cherry-pick of the lifecycle microbench). Scripts `$R/runs/xfer_d2d/bin/`, raw output `$R/runs/xfer_d2d/`.
Every build and GPU process ran under `flock $R/machine.lock`. Config untouched; one binary, env switch.
THROWAWAY PROTOTYPE. Everything here belongs to the separate transfer PR ([xfer-PR]), nothing to #2569 / #2570.

STATUS: complete; section 12 lists what ran and what is unverified. Sections 1-3 design, 4-8 measurements and gates,
9-11 memory / production / other machines, 13 recommendation.

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

## 5. (b) TPC-H SF1000 ABBA timing, tip binary (`$R/repo`) vs prototype with `SIRIUS_PROTO_XFER_D2D=pipe`

`bin/abba_q.sh` (`abba_guard.sh`: only after the gates passed): per query 4 fresh processes tip, proto, proto, tip, each
1 warm + 5 hot iterations, first hot dropped -> 4 kept per process, n = 8 per arm; pooled medians; one ABBA block per
query (no second block). Raw: `runs/xfer_d2d/p1_{tipA,protoA,protoB,tipB}/`. Results = md5 of `result.txt`.

| q | tip med ms (n) [min-max] | proto med ms (n) [min-max] | delta ms | delta % | per-block deltas ms | results |
|---|---|---|---|---|---|---|
| 1 | 470.8 (8) [459-477] | 468.8 (8) [461-475] | -2.0 | -0.43 | -2.0 | identical |
| 2 | 149.2 (8) [148-151] | 147.6 (8) [146-150] | -1.6 | -1.05 | -1.6 | identical |
| 3 | 658.7 (8) [653-668] | 621.9 (8) [615-626] | -36.8 | -5.59 | -36.8 | identical |
| 4 | 324.3 (8) [321-327] | 310.6 (8) [308-312] | -13.7 | -4.23 | -13.7 | identical |
| 5 | 693.9 (8) [680-714] | 679.6 (8) [666-702] | -14.4 | -2.07 | -14.4 | identical |
| 6 | 380.5 (8) [379-388] | 380.2 (8) [379-388] | -0.3 | -0.07 | -0.3 | identical |
| 7 | 746.9 (8) [741-752] | 710.9 (8) [707-713] | -35.9 | -4.81 | -35.9 | identical |
| 8 | 936.1 (8) [927-942] | 912.4 (8) [904-913] | -23.7 | -2.54 | -23.7 | identical |
| 9 | 1420.5 (8) [1404-1438] | 1394.1 (8) [1365-1409] | -26.4 | -1.86 | -26.4 | identical |
| 10 | 849.7 (8) [840-869] | 820.2 (8) [813-833] | -29.5 | -3.47 | -29.5 | identical |
| 11 | 172.3 (8) [167-183] | 172.1 (8) [168-183] | -0.2 | -0.11 | -0.2 | row order of ties only (see below) |
| 12 | 705.1 (8) [702-708] | 693.5 (8) [692-700] | -11.6 | -1.65 | -11.6 | identical |
| 13 | 1311.8 (8) [1279-1496] | 1306.1 (8) [1298-1495] | -5.7 | -0.44 | -5.7 | identical |
| 14 | 685.8 (8) [676-709] | 687.8 (8) [676-707] | +2.0 | +0.29 | +2.0 | identical |
| 15 | 527.6 (8) [526-530] | 527.9 (8) [526-531] | +0.3 | +0.06 | +0.3 | identical |
| 16 | 311.4 (8) [302-318] | 301.2 (8) [300-312] | -10.2 | -3.29 | -10.2 | identical |
| 17 | 497.1 (8) [487-508] | 497.9 (8) [487-507] | +0.8 | +0.16 | +0.8 | identical |
| 18 | 1098.4 (8) [1082-1114] | 1086.1 (8) [1079-1092] | -12.2 | -1.11 | -12.2 | identical |
| 19 | 879.8 (8) [876-892] | 880.5 (8) [877-892] | +0.7 | +0.08 | +0.7 | identical |
| 20 | 714.8 (8) [698-734] | 715.2 (8) [696-735] | +0.4 | +0.06 | +0.4 | identical |
| 21 | 1442.0 (8) [1429-1454] | 1244.1 (8) [1227-1258] | -197.9 | -13.72 | -197.9 | identical |
| 22 | 338.6 (8) [338-340] | 333.5 (8) [333-334] | -5.1 | -1.50 | -5.1 | identical |
| sum | 15315 | 14892 | -423.1 | -2.76 | | |

Per-process medians (the A-B columns are the same binary run twice = the noise of this protocol):

| q | tipA | protoA | protoB | tipB | tipA-tipB | protoA-protoB | proto-tip (mean of process medians) |
|---|---|---|---|---|---|---|---|
| 1 | 475.2 | 470.8 | 466.2 | 464.3 | +10.9 | +4.7 | -1.2 |
| 2 | 149.2 | 147.6 | 147.9 | 149.3 | -0.1 | -0.3 | -1.5 |
| 3 | 658.1 | 621.4 | 622.9 | 660.6 | -2.6 | -1.5 | -37.2 |
| 4 | 323.9 | 310.5 | 311.2 | 324.3 | -0.4 | -0.7 | -13.3 |
| 5 | 694.3 | 679.0 | 680.2 | 691.2 | +3.1 | -1.2 | -13.1 |
| 6 | 380.3 | 379.6 | 381.0 | 380.7 | -0.4 | -1.4 | -0.2 |
| 7 | 748.3 | 709.8 | 711.9 | 745.1 | +3.2 | -2.1 | -35.8 |
| 8 | 936.8 | 912.6 | 911.8 | 934.3 | +2.5 | +0.8 | -23.3 |
| 9 | 1412.4 | 1403.4 | 1382.0 | 1427.0 | -14.6 | +21.4 | -27.0 |
| 10 | 843.3 | 819.7 | 820.2 | 865.1 | -21.8 | -0.5 | -34.2 |
| 11 | 175.4 | 176.1 | 172.1 | 172.1 | +3.4 | +4.0 | +0.4 |
| 12 | 703.9 | 694.0 | 692.9 | 705.9 | -2.0 | +1.1 | -11.4 |
| 13 | 1309.5 | 1314.3 | 1306.1 | 1314.3 | -4.9 | +8.2 | -1.7 |
| 14 | 682.5 | 687.8 | 686.8 | 690.4 | -8.0 | +0.9 | +0.8 |
| 15 | 527.7 | 527.7 | 527.9 | 527.5 | +0.1 | -0.3 | +0.2 |
| 16 | 311.4 | 305.1 | 301.2 | 309.1 | +2.3 | +3.9 | -7.1 |
| 17 | 497.1 | 497.9 | 497.7 | 496.7 | +0.3 | +0.3 | +0.9 |
| 18 | 1099.2 | 1079.7 | 1090.3 | 1092.4 | +6.8 | -10.6 | -10.8 |
| 19 | 880.2 | 882.3 | 880.5 | 879.8 | +0.4 | +1.8 | +1.4 |
| 20 | 715.1 | 715.2 | 715.2 | 714.8 | +0.3 | -0.0 | +0.3 |
| 21 | 1443.9 | 1244.1 | 1246.9 | 1441.8 | +2.1 | -2.8 | -197.3 |
| 22 | 338.4 | 333.6 | 333.5 | 338.7 | -0.3 | +0.1 | -5.0 |
| sum | 15306 | 14912 | 14886 | 15325 | -20 | +26 | -416 |

- **Suite: 15315 -> 14892 ms, -423 ms, -2.76%** (mean of process medians: -416 ms). Two runs of the same arm differ by
  20-26 ms on the suite sum, so the gain is -420 +- 25 ms. Tip total 15315 vs the reference 15372 (-0.4%).
  Tip + this = ~14.95 s against the base reference 15.225 s (base measured earlier, without any transfer change),
  without touching #2569 / #2570.
- The 8 named queries: q21 -198 (-13.7%), q3 -37 (-5.6%), q7 -36 (-4.8%), q10 -30 (-3.5%; same-arm spread 22 ms),
  q9 -26 (-1.9%; same-arm spread 15-21 ms), q8 -24 (-2.5%), q4 -14 (-4.2%), q5 -14 (-2.1%): -379 ms together.
  Further: q12 -12 (-1.7%), q16 -10 (-3.3%; same-arm spread 2-4), q22 -5 (-1.5%).
- Controls: q6 -0.3 ms, q2 -1.6 ms (-1.1%; q2 only has exact-kind filters: relay), q15 / q17 / q19 / q20 within
  +-1.4 ms. q18 -12 and q13 -6 are inside their own same-arm spread (7-11 ms): not counted as gains.
- Against the model (MB_TRANSFER section 9: finish chain 600 -> ~170 ms, -430): measured -423. Per query the wall gain
  is 61-81% (median 66%) of the tip's HOST-staged finish time (PROFILE section 4: q21 296, q3 56, q7 56, q8 39,
  q10 37, q9 35, q4 21, q5 20 ms); a 3.5x faster finish (MB4) would give 71%.
- Results: identical md5 for 21 queries. q11: all four processes differ from each other (tip vs tip too) only in
  the order of rows with equal `value`; the sorted outputs are byte-identical (md5 `730f7529...` for all four).

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

## 7. (c) nsys on q21 and q3: finish wall, bytes by transport, sync count

`bin/nsys_q.sh <q>` (3 iterations per arm, iteration 0 warm; captures under `runs/xfer_d2d/ns_tip`, `ns_pipe`),
analysis `bin/nsys_xport.py` (copies issued by the finish thread inside `dynfilter::accum::finish`, followed by
correlation id; a `cudaMemcpyBatchAsync` = one HOST-staged leg, direction from the memory kinds; CUPTI's `copyKind`
is not usable for batch copies) and `bin/nsys_tab.py`. Two hot iterations each; the two iterations agree to < 1 ms and
to the byte.

| q | arm | `accum::finish` wall ms per iteration (each finish) | host waits (`cudaStreamSynchronize`) | D2H MiB | H2D MiB | P2P MiB | query wall under nsys ms |
|---|---|---|---|---|---|---|---|
| 21 | tip | 307.7 (281 + 27) | 1932 | 6107 | 9161 | 0 | 1450 |
| 21 | proto `pipe` | 84.4 (76 + 9) | 38 | 3054 | 3054 | 6107 | 1258 |
| 3 | tip | 62.2 (58 + 4.2) | 404 | 1179 | 1855 | 0 | 667 |
| 3 | proto `pipe` | 23.4 (20.4 + 3.0) | 34 | 618 | 618 | 1236 | 634 |

- q21: finish 308 -> 84 ms (3.6x), host-staged bytes 15.3 GiB -> 6.1 GiB (10 x B -> 4 x B), P2P 0 -> 6.1 GiB (4 x B),
  host waits 1932 -> 38 (19 per finish: island streams + partial streams at the end, allocation joins).
  q3: 62 -> 23 ms, host-staged 3.0 GiB -> 1.2 GiB, waits 404 -> 34.
- Engine vs microbench: q21's 1394 MiB array takes 76 ms in the engine under nsys (55 ms/GiB; MB4 52), q3's 281 MiB
  array 20.4 ms (MB4 14.3 ms; the tip's 58 ms is also 7 ms above MB4's 51: the finish first waits for the insert
  kernels still running on the partial streams).
- Query wall under nsys: q21 -192 ms, q3 -33 ms: consistent with the ABBA deltas (-198, -37 ms). About 30 ms of q21's
  223 ms finish saving does not reach the wall (the second, 133 MiB filter is not fully on the critical path).
- Debug-log pass (`bin/logpass.sh 3 21`, 2 iterations): all 8 Bloom finishes logged `through island peer copies +
  HOST pipelined exchange`, roots GPU 0, 1, 2 and 3 all occurred; 0 fallback lines. Scratch on the other leader:
  32 MiB (2 x 2 slots of 8 MiB) for S=3, 15 MiB for the 57 MiB S=0 filters (allocated but unused: see section 10).

## 8. Correctness gates

| Gate | What | Result |
|---|---|---|
| `[mbid]` G=4 (`mb/gate_g4.log`) | 4 sizes (1, 3, 20, 57 MiB; 2 keys of different size -> 1 to 8+ chunks, both chunk parities) x all 16 contribution masks x roots {0, 3} x modes {off, star, exchange, pipe}: every replica on every GPU compared byte for byte with the OR of the partials read before `finish`, and with the `off` replica | 512 cases, **0 bad** |
| `[mbid]` G=3 on GPUs 0,1,2 and on GPUs 0,2,3 | islands {pair, single} with the root in the pair / as the single | 128 + 128 cases, 0 bad |
| `[xfer_relay]` (`test_sirius_dynamic_filter_mgpu.cpp`) | hash IN-list (200k keys), small IN-list (7 keys), one-shot Bloom (200k keys), roots {0, 3}: mask of a 700k-row probe on every GPU, switch on vs off, identical (exact kinds also checked against the exact key set; Bloom includes its false positives) | 24 masks identical, 6 copies relayed (1 per filter, as designed) |
| TPC-H results | `result.txt` md5 of every ABBA process, tip vs prototype | identical for all 22 queries (section 5) |
| Existing unit tests, switch `pipe` | `sirius_unittest "[dynamic_filter][multi_gpu]~[.]"` (32 cases, incl. the relay gate) | all 32 pass, run twice (4917 / 4920 assertions)  |
| Existing unit tests, switch off (same binary) | same filter | all 32 pass, run twice (4920 / 4917 assertions: the count varies by 3 between runs in either mode) |
| Other multi-GPU tests, switch `pipe` | `[multi_gpu]~[dynamic_filter]~[.]` | 115 cases pass (5,655,402 assertions) |
| Fault injection, switch off / `pipe` | separate binary `sirius_cuda_fault_test "[cuda_status_fault][multi_gpu]~[.]"` (`bin/fault.sh`) | off: 12 cases pass (962 assertions); `pipe`: 12 cases pass (956 assertions; these tests step an injected fault through the CUDA calls of a publication, and the island path issues a different call sequence, which is the likely reason for the 6 fewer assertions - not investigated) |

Not run: the single-GPU DF tests (`plan()` declines below 3 GPUs and the relay never engages without a HOST-staged
copy, so they cannot reach new code), the integration shards, `make test`.

## 9. Memory held during a finish (G=4, two pair islands, 8 MiB chunks = any Bloom >= 64 MiB)

| | pinned HOST | device scratch, root GPU | device scratch, other leader | other GPUs |
|---|---|---|---|---|
| tip, HOST staged (this machine) | 8 MiB (1 window) | 8 MiB (1 slot; 0 when S=0) | 0 | 0 |
| tip, PEER (full P2P) | 0 | 48 MiB (2 parities x 3 sources) | 0 | 0 |
| `star` | 8 MiB | 24 MiB (2 merge + 1 cross) | 16 MiB (2 merge) | 0 |
| `exchange` | 16 MiB (1 window per leader) | 24 MiB | 24 MiB | 0 |
| `pipe` | 32 MiB (2 windows per leader) | 32 MiB (2 merge + 2 cross) | 32 MiB | 0 |

- Slots scale with the chunk (clamp(B/8, 2, 8 MiB)), not with B. The root's scratch stays inside the bound the task
  already reserves (`publication_scratch_bytes` = 2 x (G-1) slots = 48 MiB at G=4).
- New: the other leader's scratch is leased at finish time through `scoped_replica_reservation` on a GPU the
  Publishing Task does not run on (a refusal falls back to the tip transports). It is held only for the finish.
- All of it is released before `finish` returns. The array memory itself (B per GPU) is unchanged.
- CUDA Best Practices 10.1.1 ("pinned memory should not be overused"): 32 MiB transient from the engine's existing
  fixed-block pinned pool, no new pinning.

## 10. Production issues (what a real [xfer-PR] must fix; the prototype is not mergeable)

1. **Scratch on the other leader is not in the plan's upfront lease.** Either add it to the session's reservation at
   `begin` (it is known then: islands depend only on the replica spaces) or accept the finish-time lease + fallback.
   The prototype also leases the cross slots when the other island contributes nothing (15-16 MiB unused).
2. **Per-finish object churn**: 6 streams, 22 events, 4 windows, 1 lease per finish = the ~0.06 ms fixed cost (6.1).
   Use `runtime_stream_pool` leases and keep the tip loop for tiny single-contributor filters.
3. **Topology is recomputed per finish** from `try_grant_pool_access` (N x (N-1) calls; `cudaMemPoolGetAccess`
   shows 20 calls per 2 finishes in nsys). Compute the islands once per context and hand them to the builder.
4. **Islands = greedy cliques of mutual access.** Exact for pair / full / none topologies; for a non-transitive access
   graph a greedy clique cover is legal but not necessarily the best.
5. **More than two islands** only has `cross_star` (serial HOST loop over the leaders). The right cross stage for N
   leaders is the other agent's `host_or` over N inputs (section 3).
6. **Failure paths**: `island_reduce`'s destructor drains its own streams before its scratch and windows go away, and
   `finish_impl`'s `join_on_failure` still joins the partial streams; the 12 multi-GPU fault-injection cases pass
   with the switch on, but they were written for the tip's call sequence: island-specific faults (failed peer copy on
   the other leader, failed event wait) have no test. `leak_storage` is not set for a failed island-stream join.
7. **Observability contract**: `publication.transport` still reports `HOST_STAGED` for an island publication and
   `workspace_observation` reports only the root's scratch and the summed windows. A third route value
   (`ISLAND`) and per-GPU scratch are needed before tests can assert on it.
8. **The cross stage is the prototype's weakest code**: three hand-written variants. In the real PR it should be one
   leg-graph primitive shared with the host pipeline (`proto/xfer-host`), with P2P legs and HOST legs as two leg kinds.
9. **Replica relay** relies on "HOST-staged copies are complete on return". If the host route becomes asynchronous
   (the other agent's `host_bounce_scope`), a relayed copy must wait on the relay's upload event: the relay then has
   to carry an event, not only a pointer.
10. **Concurrency with other traffic** (build CONCAT move, eager probe scans of S6): `pipe` uses both pair uplinks
    and all four ports at once for 50-80 ms on q21. MB_TRANSFER section 7 measured the contention of such schedules
    with a synthetic background; nothing was measured here with eager scans on.
11. Exact-key gather still pays `D2H, sync, H2D, sync` per slice from the two cross-pair GPUs (5.3 ms at 512 spread
    slices): owned by the host-bounce prototype (batched gather), not changed here.

## 11. Expected behaviour on other machine classes

| Machine | Bloom finish | Replicas / gather | Evidence |
|---|---|---|---|
| Full P2P (every pair has pool access) | one island -> `plan()` declines -> tip PEER pipeline, byte for byte. **No change, no gain, no cost** (one extra N^2 grant query) | relay never engages (every copy is already a peer copy) | G=2 pair: off = on within 0.2% (6.3); G=4 full P2P: by code only, no such machine here |
| Partial P2P (this box: 2 pairs) | islands; 185 -> 52-56 ms/GiB at S=3, 61 -> 40 at S=0 | one bounce instead of two per fan-out | sections 4-7 |
| No P2P | all-singleton islands -> `plan()` declines -> tip HOST loop. **No change**: the whole gain there has to come from the host pipeline (`proto/xfer-host`: 53-76 ms/GiB modelled in MB_TRANSFER) | relay never engages (no relay grants anyone) | by code only; a 3-GPU no-P2P set cannot be formed on this box |
| Full P2P, G >= 4, as a follow-up | the island code with a P2P cross leg would give 2 x B per port instead of the star's 3 x B (MB_TRANSFER: ~44 vs 62-67 ms/GiB, model) | - | not built: needs "islands" to mean a reduce tree, not access cliques |

CUDA Best Practices Guide, memory chapter, as applied: 10.1 "minimize data transfer between the host and the device"
(10 x B -> 4 x B through the host, the OR stays on the GPUs); 10.1.1 pinned memory, not overused (existing pool, 32 MiB);
10.1.2 asynchronous transfers overlapped with computation on non-default streams, staged per chunk (`pipe`: copies and
OR kernels of consecutive chunks overlap, one host wait at the end instead of 10 per chunk).

## 12. What ran, repetitions, variance, what is unverified

Ran (all under the machine lock; one binary = `proto/xfer-d2d` at `b7d0eb81`, never rebuilt after the gates):
- Build: full build + one incremental rebuild; the recovered design compiled unchanged on the first attempt.
- Gates: `[mbid]` 512 + 128 + 128 cases, `[xfer_relay]` 24 masks: once each, all clean.
- MB4 G=4: 4 sizes x 5 masks x 4 modes, median of 5 (+1 warm-up), one process; small sizes median of 9; G=2 median
  of 5; MB4x / MB5 median of 7, off run twice. Spread: <= 1.9% for >= 57 MiB, 4-30% below 1 ms.
- ABBA, mode `pipe`: all 22 queries, **one block per query** (4 processes, n = 8 hot per arm). Same-arm
  process-to-process differences: <= 4 ms for 16 queries; q1 11, q9 15-21, q10 22, q13 5-8, q14 8, q18 7-11 ms.
- ABBA q21 with modes `exchange` and `star`: section 5.1 (one block each).
- nsys: q21 and q3, tip and `pipe`, 2 hot iterations each, one capture each.
- Debug-log pass q3, q21 (2 iterations): route confirmation.
- Unit tests: section 8.

Unverified / not done:
- No second ABBA block on another day or after a reboot; no ABBA for `exchange` / `star` beyond q21.
- Full-P2P at G >= 3 and no-P2P at G >= 3: by code inspection only (cannot be formed on this machine).
- > 2 islands (`cross_star` over 3+ leaders): only the 3-GPU `[mbid]` topologies (2 islands) ran; a 3-island set
  does not exist here. The N-island path is identity-tested only through `star` with 2 islands.
- Interaction with eager probe scheduling (S6) and with a concurrent build CONCAT move: not measured.
- Composition with `proto/xfer-host`: designed (section 3), not built, not measured.
- OOM / refused-lease fallback of the island path (`prepare()` returning false): not exercised by any test.
- Quent telemetry of the ABBA runs was moved to `$R/telemetry/xfer_d2d_*` and not analysed.
- The single-contributor variant with every link at 1 x B (section 4) is a model.

## 13. Recommendation

1. **Land the per-island reduce for the Bloom finish in the transfer PR** (mode `pipe` semantics). Measured on this
   machine: finish 185 -> 52-56 ms/GiB (3.5x), suite -423 ms (-2.8%), q21 -13.7%, q3 -5.6%, q7 -4.8%, q4 -4.2%,
   q10 -3.5%; results and filter bits identical. It is the single largest measured item of the whole campaign and it
   passes both gates (>= 1% of a query on 11 queries, >= 0.5% of the suite 5 times over). With eager probe scans
   (S6) the finish is squarely on the critical path, so this gain is the one that survives S6.
2. **Order of work**: (i) islands + P2P merge / fan-out with the existing HOST loop between leaders (`star`: no
   new host code, already 1.8x on the finish); (ii) the symmetric exchange; (iii) event ordering. (ii) and (iii)
   should not be written as a third hand-rolled variant: implement the cross stage once on the host-pipeline
   leg primitive of `proto/xfer-host` (section 3 contract) and let it serve both the no-P2P machine and the island
   leaders here.
3. **Replica relay**: land it with the same PR only if the host route stays synchronous, otherwise give relays an
   event (section 10.9). Gain is small (-18..-20% of a replica fan-out, <= 0.4 ms per filter).
4. **Exact-key gather**: no transport change needed (already per pair); batching it is the host prototype's item.
5. **Fix before merging**: reserve the other leader's scratch at `begin`, pool the streams, compute the islands once
   per context, give the route its own `publication_transport` value and tests, add island fault-injection tests
   (section 10, items 1-3, 6, 7).
6. Not recommended now: a reduce tree for full-P2P machines at G >= 4 (model only, no machine to measure) and the
   1 x B single-contributor fan-out (<= 1 ms per filter).
