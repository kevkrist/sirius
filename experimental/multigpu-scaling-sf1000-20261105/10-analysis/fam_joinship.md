# Family "joinship": TPC-H Q3, Q10 (customer/orders/lineitem joins + group-by + top-N)

Analyst: fam_joinship. Commit bb557b1a, host-pinned SF1000, 4x RTX PRO 6000. Read-only; no GPU runs by me.
Artifacts (scripts + raw outputs): `10-analysis/fam_joinship/` (pipe_acct.py, range_api.py, clone_by_pipe.py,
link_phases.py, pipe_api.py, nvtx_names.py, logsec.sh, partial_timings.py; flattened Quent for g2s, g2x,
x_g4_joinpol, x_g1_joinpol under `qf_*`).
Revision 2 (17:55) adds the post-17:08 runs: x_{g1,g4}_batch4g, x_{g1,g4}_best (batch4g + joinpol), x_{g1,g4}_thr8,
x_g4_hpb4g, x_g4_batch8g, x_g4_gpupin, and nsys3_g4 (Q3 single-query capture). New outputs:
`fam_joinship/pipe_acct_{batch4g,best,thr8}.txt`, `nsys3_g4_q3_check.txt`, `qf_{g1,g4}_{batch4g,best,thr8}/`.

**Rev2 headline.**
- Baseline family gap is 1.311 s (Q3 0.548, Q10 0.763).
- The best measured config (`best` = batch4g + joinpol) reaches g4 1.690 s vs g1 2.651 s, i.e. 1.57x. The residual
  gap to 4x is 1.027 s (Q3 0.380, Q10 0.647); §1.5 decomposes it.
- What is left after the config fixes:
  - Host link: about 0.52 s.
  - Exchange clones: about 0.32 s (Q3 broadcast replication, Q10 customer join, Q10 group-by re-shuffle).
  - Q10 Bloom HOST-staged publication: 0.066 s. It persists unchanged under `best`: 21.0 / 44.5 ms in the log.
  - Q10 forced partitions and small tails: about 0.10 s.
- `batch4g` confirms that F3 and F4 scale with fragment count. Q10 P2+P4 shrink by 190 ms at g4. But g1 also gains
  165 ms elsewhere, so the speedup ratio barely moves (1.45x → 1.52x).
- `thr8` makes Q10 g4 96 ms slower. The allocator becomes binding on the lineitem scan (F9), so more threads cannot
  hide latency.

Conventions:
- **wall** = un-profiled hot mean (iters 2-4) from `timings.csv`.
- Phase tables use Quent (un-profiled, iters 2-4 mean). Quent wall is within 2% of `timings.csv`. Priced ms are
  Quent ms x 1.019 (Q3: 1056/1036; Q10: 1196/1174).
- nsys (iter2, profiled 1.06-1.13x slower) is used only for proportions: alloc/sync/kernel share and bytes.
- **thread-ms** = summed over executor threads (4 at g1, 16 at g4). **Wall impact** of a phase = its measured
  span on the critical path. These queries are strictly serial chains of phases (scan, then exchange, then join,
  ...), so the phase spans add up to the query wall (checked: the sums match to within 1 ms).
- **Gap share** of a phase = g4 span − g1 span / 4.

## 1. Time accounting

### 1.1 Wall time and speedup (un-profiled, s)

| q | g1 | g2s {0,1} | g2x {0,2} | g4 | x g2s | x g2x | x g4 | ideal g4 | gap to 4x |
|---|---|---|---|---|---|---|---|---|---|
| Q3 | 2.031 | 1.338 | 1.172 | 1.056 | 1.52 | 1.73 | 1.92 | 0.508 | **0.548** |
| Q10 | 1.731 | 1.595 | 1.244 | 1.196 | 1.09 | 1.39 | 1.45 | 0.433 | **0.763** |
| family | 3.762 | 2.933 | 2.416 | 2.252 | 1.28 | 1.56 | 1.67 | 0.941 | **1.311** |

With equal join algorithms at both GPU counts (`x_g4_joinpol` / `x_g1_joinpol`: `max_broadcast_join_size` 8GB
and `max_build_hash_table_bytes` derived as 8GB):

| q | g1 | g4 | x |
|---|---|---|---|
| Q3 | 1.164 | 0.655 | 1.78 |
| Q10 | 1.735 | 1.203 | 1.44 |

So the baseline Q3 speedup of 1.92x is flattered (see F7).

All config flips (un-profiled hot mean of iters 2-4, s; `20-experiments/*/timings.csv`):

| config | Q3 g1 | Q3 g4 | Q3 x | Q10 g1 | Q10 g4 | Q10 x | family g4 (Δ vs base) |
|---|---|---|---|---|---|---|---|
| baseline | 2.031 | 1.056 | 1.92 | 1.731 | 1.196 | 1.45 | 2.252 |
| joinpol | 1.164 | 0.655 | 1.78 | 1.735 | 1.203 | 1.44 | 1.858 (−394) |
| batch4g | 1.980 | 1.010 | 1.96 | 1.566 | 1.031 | 1.52 | 2.041 (−211) |
| batch8g | — | 1.022 | — | — | 1.049 | — | 2.071 (−181) |
| **best** (batch4g + joinpol) | 1.087 | **0.652** | 1.67 | 1.564 | **1.038** | 1.51 | **1.690 (−562)** |
| thr8 | 2.285* | 1.083 | — | 1.730 | 1.292 | 1.34 | 2.375 (+123) |
| hpb256 | — | 1.056 | — | — | 1.237 | — | 2.293 (+41) |
| hpb4g | — | 1.071 | — | — | 1.242 | — | 2.313 (+61) |
| gpupin | — | 2.618 (4.19/1.76/1.90) | — | — | 1.569 | — | unusable** |

\* g1 thr8 Q3: CONCAT(14) inputs partly spilled to HOST (4.2 GB HOST-origin prepare, `pipe_acct_thr8.txt`), so it
is a memory-pressure artifact.
\** gpupin is confounded by downgrade/convert failures, Q3 above all (see `X3_link.md` L1(d)).

Under equal algorithms and the best config, family scaling is 2.651 / 1.690 = **1.57x**.

### 1.2 Per-GPU and thread-time (Quent, iters 2-4 mean)

| | Q3 g1 | Q3 g4 | Q10 g1 | Q10 g4 |
|---|---|---|---|---|
| per-GPU busy (some task in flight) | 100% | 98-99% on every GPU | 100% | 93-94% on every GPU |
| tasks per GPU | 1819 | 458-459 | 1711 | 431-433 |
| thread-ms total | 7787 | 15000 (1.93x) | 6500 | 13011 (2.00x) |
| HOST-origin prepare (H2D + decode) | 2833 | 4745 | 4276 | 7483 |
| GPU-origin prepare (cross-GPU clones) | 15 | **6061** | 9 | **1668** |
| compute | 4939 | 4194 | 2216 | 3859 |
| thread occupancy (thread-ms / (wall x threads)) | 0.968 | 0.905 | 0.950 | **0.693** |

- The GPUs are not idle and the load is balanced.
- Q3 inflation is almost entirely clone prepare (+6.0 s thread-ms).
- Q10 inflation is spread over:
  - +3.2 s scan prepare (link, plus allocator)
  - +1.7 s clones
  - +1.6 s PARTITION compute
  - 0.69 occupancy, from single-task-per-GPU CONCAT and MERGE phases that leave 3 of 4 threads idle.

### 1.3 Phase table, Q3 (Quent ms; pipelines from `fam_joinship/pipe_acct*.txt`)

| phase (pipelines) | g1 | g2s | g2x | g4 | g1/4 | **gap** | what dominates at g4 |
|---|---|---|---|---|---|---|---|
| P1 customer + orders scans (0-6), incl. CONCAT(3) | 247.6 | 210.1 | 130.4 | 115.2 | 61.9 | **53.3** | H2D at 102-105 GB/s (link ceiling) + 6.7 ms CONCAT(3) clone |
| P2 orders⋈customer build; PARTITION(9), CONCAT(10) | 53.0 | 40.2 | 59.5 | 49.9 | 13.3 | **36.6** | CONCAT(10): 4 tasks, 1.1 GB staged + 0.55 GB P2P, 98 thread-ms clone |
| P3 lineitem scan + DF (9) | 760.5 | 626.3 | 385.0 | 317.9 | 190.1 | **127.8** | 34.0 GB H2D at 105.6 GB/s = link ceiling (g1: 42.6 GB/s) |
| P4 PARTITION(13), CONCAT(14), HJ(15)+GB | 926.2 | 417.6 | 547.6 | 522.4 | 231.6 | **290.8** | probe shuffle: 25.7 GB staged + 12.8 GB P2P; PARTITION tail 97 ms |
| P5 PARTITION(18), MERGE_GB, MERGE_TOP_N | 24.3 | 23.8 | 29.8 | 30.7 | 6.1 | **24.6** | forced 4-way PARTITION(18) (11.5 ms vs 0.8) + MERGE clones |
| total | 2011.6 | 1318.0 | 1152.3 | 1036.1 | 502.9 | **533.2** | |

Under joinpol (equal algorithms), the totals are g1 1142.7 and g4 634.0, so the gap is 348:

| phase | gap under joinpol | what dominates |
|---|---|---|
| P1 | 53.8 | link |
| P2 | **112.3** | build broadcast, CONCAT(10) 380 thread-ms of clones, 1 task per GPU |
| P3 | 136.1 | link |
| P4 | 37.4 | Bloom leak, see F1 |
| P5 | 8.6 | |

### 1.4 Phase table, Q10 (Quent ms)

| phase (pipelines) | g1 | g2s | g2x | g4 | g1/4 | **gap** | what dominates at g4 |
|---|---|---|---|---|---|---|---|
| P1 orders scan (3) | 186.1 | 157.9 | 96.2 | 81.9 | 46.5 | **35.4** | 8.5 GB H2D at 104.8 GB/s (ceiling) |
| P2 orders build exchange: PARTITION(4), CONCAT(5) | 8.6 | 29.8 | 45.8 | 67.8 | 2.2 | **65.6** | forced 4-way partition (alloc 70%) 22 ms + **Bloom publication 20.5 ms** + CONCAT(5) 489 clones 23 ms |
| P3 lineitem scan + DF (6) | 583.5 | 490.7 | 294.1 | 249.3 | 145.9 | **103.4** | 26.5 GB H2D, 106 GB/s un-profiled (ceiling) |
| P4 lineitem probe exchange: PARTITION(8), CONCAT(9), HJ(10) | 34.1 | 114.7 | 163.7 | 203.8 | 8.5 | **195.3** | forced partition of 627 x 3 MB pieces (95 ms tail, alloc 80%) + CONCAT(9) 1881 serial clones (103 ms) |
| P5 join-output exchange: PARTITION(11), CONCAT(12) | 9.0 | 20.6 | 43.5 | 61.7 | 2.3 | **59.5** | **Bloom publication 41 ms** on one PARTITION(11) task + CONCAT(12) clones 16 ms |
| P6 customer scan + DF (13) | 438.1 | 423.9 | 221.7 | 218.6 | 109.5 | **109.1** | 23.3 GB **uncompressed** H2D: g1 54 GB/s, g4 108 GB/s (both at their link ceilings) |
| P7 customer join, nation join, group-by (15-18) | 306.7 | 201.3 | 195.1 | 152.0 | 76.7 | **75.3** | CONCAT(16) 3.46 GB staged + 1.73 GB P2P (496 thread-ms clone), PARTITION(15) forced |
| P8 PARTITION(24), MERGE_GB, MERGE_TOP_N | 143.5 | 134.3 | 161.9 | 138.4 | 35.9 | **102.5** | MERGE_GROUP_BY: 9 tasks, 77 ms clone prepare each (4.04 GB staged + 2.02 GB P2P) |
| total | 1709.7 | 1573.2 | 1222.1 | 1173.6 | 427.4 | **746.2** | |

- P4 gets *worse* as GPUs are added: 34 → 115 → 164 → 204 ms (g1 → g2s → g2x → g4).
- P8 is flat: 144 → 134 → 162 → 138 ms.
- The g2s row shows the 2-GPU same-switch config adds no link bandwidth: the customer scan is 424 vs 438 ms.

### 1.5 Residual gap under the best measured config (`best` = batch4g + joinpol; Quent ms, `pipe_acct_best.txt`)

Q3 (Quent wall: g1 1067.3, g4 632.3; un-profiled factor 1.031):

| phase | g1 | g4 | g1/4 | gap | what dominates at g4 |
|---|---|---|---|---|---|
| P1 customer + orders scans, HJ(8) (0-6) | 265.5 | 137.7 | 66.4 | 71.3 | link: 23 GB host-in |
| P2 build broadcast PARTITION(9), CONCAT(10), until the probe scan starts | 24.6 | 108.4 | 6.2 | **102.2** | CONCAT(10): 1 task per GPU replicates the 2.2 GB build to every GPU. 377.9 thread-ms clone, 8.8 GB. Runs until 341 ms, overlapping the scan by 95 ms |
| P3 lineitem scan + DF | 717.3 | 360.2 | 179.3 | **180.9** | exactly 1.99x vs g1 = 2-uplink ceiling |
| P4 tail PARTITION(13) … TOP_N | 59.9 | 26.0 | 15.0 | 11.0 | 13.0 GB probe leaks past the Bloom (g1: 0.5 GB), but costs at most 11 ms |
| total | 1067.3 | 632.3 | 266.8 | **365.5** | un-profiled: 380 |

Q10 (Quent wall: g1 1542.3, g4 1015.4; factor 1.022):

| phase | g1 | g4 | g1/4 | gap | what dominates at g4 |
|---|---|---|---|---|---|
| P1 orders scan | 177.8 | 86.2 | 44.5 | 41.7 | link |
| P2 PARTITION(4), CONCAT(5) | 5.9 | 36.7 | 1.5 | 35.2 | **Bloom HOST publication 21.0 ms** (log) + forced 4-way partition + 37 thread-ms clones |
| P3 lineitem scan + DF | 574.8 | 245.2 | 143.7 | **101.5** | link (2.34x vs g1) |
| P4 PARTITION(8), CONCAT(9), HJ(10) | 7.1 | 44.3 | 1.8 | 42.5 | forced partition (145 tasks) + CONCAT(9) 122 thread-ms clones |
| P5 PARTITION(11), CONCAT(12) | 9.1 | 60.1 | 2.3 | 57.8 | **Bloom HOST publication 44.5 ms** (log) + CONCAT(12) 52 thread-ms clones |
| P6 customer scan + DF (7 tasks) | 442.4 | 248.9 | 110.6 | **138.3** | link 110.6 (2x floor = 221) + 28 from 7 scan tasks on 4 GPUs (2/2/2/1) |
| P7 customer join exchange + HJ(17) + HJ(21) + GB | 180.7 | 150.1 | 45.2 | **104.9** | CONCAT(16) 388 thread-ms clones (5.2 GB) |
| P8 PARTITION(24), MERGE_GB, TOP_N | 144.4 | 143.8 | 36.1 | **107.7** | MERGE_GROUP_BY 697.5 thread-ms clone prepare. Still no speedup at all |
| total | 1542.3 | 1015.4 | 385.6 | **629.8** | un-profiled: 647 |

Residual family gap by mechanism (un-profiled ms):

| mechanism | Q3 | Q10 | family |
|---|---|---|---|
| link ceiling (F1) | 260 | 259 | **519** |
| exchange clones: broadcast replication (Q3), customer-join exchange (F6), group-by re-shuffle (F5) | 105 | 217 | **322** |
| forced partitions + fan-in residual (F3/F4) | 0 | 72 | 72 |
| Bloom HOST-staged publication (F8) | 0 | 67 | 67 |
| scan-task imbalance from 4 GB batches | 0 | 29 | 29 |
| tails | 11 | 0 | 11 |
| total | 376 | 644 | **1020** (measured 1027) |

### 1.6 What the post-17:08 flips moved (Quent ms; `pipe_acct_batch4g.txt`, `pipe_acct_thr8.txt`)

- **batch4g, Q10 g4: 1173.6 → 1009.8 (−164).**
  - P4: 203.8 → 44.2 (−160). PARTITION(8) has 145 tasks instead of 627, and its compute falls from 1525 to 185
    thread-ms. CONCAT(9) clone prepare falls from 328 to 122 thread-ms.
  - P2: 67.7 → 37.0 (−31).
  - P6 customer scan: 218.6 → 249.0 (+30). There are 7 tasks for 4 GPUs.
  - Everything else is within ±4 ms.
  - This directly confirms that F3 and F4 scale with fragment count. They were priced at 263 ms gap share for Q10; 190
    of that was recovered.
- **batch4g, Q10 g1: 1709.7 → 1544.4 (−165).** Almost all of it is P7 (306.8 → 182.5). CONCAT(16) at g1 serially
  concatenated 31 customer pieces into 8 partitions (165.6 ms compute); with 7 pieces it passes through. So g1 gains
  the same amount from a *different* place, and Q10's speedup only moves from 1.45x to 1.52x.
- **batch4g, Q3: g4 −46, g1 −51.**
  - PARTITION(13) compute falls from 1538 to 926 thread-ms.
  - CONCAT(14) clone prepare is unchanged (5879 vs 5796 thread-ms), because the 38.5 GB probe shuffle is bytes-bound,
    not fragment-bound. That is consistent with F2.
- **thr8, Q10 g4: 1173.6 → 1270.3 (+97).**
  - The lineitem scan span grows from 249.3 to 321.4 ms, i.e. 106 → 82 GB/s for the same 26.5 GB. Its HOST-prepare
    thread-ms grows 2.24x (3651 → 8177).
  - PARTITION(8) compute grows from 1525 to 3770 thread-ms (2.47x) for identical work, and its span from 104 to 139.
  - At g1, thr8 changes nothing for Q10 (1709.7 vs 1708.9).
  - So at g4 the scan sits right at the process-serialized allocator limit (F9). Doubling the threads tips it from
    link-bound to allocator-bound.
- **nsys3_g4 Q3** (single-query capture, 1089.7 ms profiled vs 1056 un-profiled) reproduces nsys2 within 4%
  (`nsys3_g4_q3_check.txt`):
  - CONCAT(14): 1254 staged clones of 25.68 GB, with 4867 ms of sync vs 1520 ms of copy, plus 627 P2P clones of 12.84 GB.
  - PARTITION(13) allocator share: 70.4%. PARTITION(18): 77.4%.

## 2. Findings (ranked by family gap share, un-profiled g4 ms)

Primary attribution is non-overlapping and sums to the gap. Overlapping levers are noted per finding.

| # | finding | kind | Q3 | Q10 | family | confidence |
|---|---|---|---|---|---|---|
| F1 | Host-link ceiling on scans (2 uplinks, about 105 GB/s) | hardware | 178 | 253 | **431** | high |
| F2 | Q3 HJ(15) probe shuffle of an unfiltered 51 GB probe (BUILD_PROBE P=4; broadcast blocked by the 256MB cap; Bloom refused by its 256MB cap) | design+config | 260 | 0 | **260** (measured fix: −401 ms at g4) | high |
| F3 | Fan-in clones of per-task fragments: serial, about 2 syncs per buffer, 1 task per GPU | design/mechanical | 43 | 141 | **184** | high |
| F4 | N-forced partitioning (P≥N), allocator-bound | design (+mech) | 37 | 122 | **159** | high |
| F5 | Group-by re-shuffle after a join already partitioned on a group key, plus MERGE clone | design | 25 | 104 | **129** | medium-high |
| F6 | Bytes-bound exchange of the customer-side join (Q10 P7) | design | 0 | 77 | **77** | medium |
| F8 | Accumulated-Bloom HOST-staged publication blocks the final build-side task | design | 0 | 63 | **63** | high |
| — | residual (outside engine) | | 5 | 9 | 14 | |
| | **total = gap** | | 548 | 769 | **1317** | |
| F9 | Driver stream-ordered allocator contention (overlaps F3, F4: share of their wall) | mechanical | ~78 abs | ~117 abs | **~195 abs** | high |
| F7 | Join-mode / Bloom confound flatters Q3 g4 (measurement) | config/measurement | −184 of gap hidden | 0 | | high |

**Rev2, the same findings under `best`** (residual gap 1027 ms; §1.5):
- F1 link: 519.
- Exchange clones: 322 in total, split as F5 about 110, F6 about 107, and Q3 broadcast replication 105 (the new cost
  joinpol introduces).
- F3/F4: 72.
- F8: 67.
- Scan-task imbalance: 29 (introduced by batch4g).
- Tails: 11.
- F2 is fixed: 0.
- F9 sits inside the 72 and, per thr8, co-binds the scan.

### F1. Host-link ceiling during scans: 431 ms (Q3 178, Q10 253). Hardware.

**Evidence (nsys, H2D bytes inside each scan pipeline window, `link_phases.txt`):**

| scan | GB | g1 GB/s | g4 GB/s |
|---|---|---|---|
| Q3 customer | 2.55 | 53 | 102 |
| Q3 orders | 8.6 | 42.6 | 101.9 |
| Q3 lineitem | 34.0 | 42.6 | 105.6 |
| Q10 orders | 8.49 | 44.2 | 104.8 |
| Q10 lineitem | 26.5 | 43.7 | 88 profiled; 26.5/0.249 s = **106** un-profiled |
| Q10 customer | 23.3 | 54.0 | 107.7 |

- Per-GPU H2D is balanced: 8.5 GB on each GPU for the Q3 lineitem scan.
- The microbench ceiling for 4 GPUs is 105 GB/s (`microbench/h2d_concurrency`).
- At g1 the scans run at 42-54 GB/s, which is at or near the single-uplink ceiling of 52 GB/s.
- So each g4 scan phase is at most 2.0-2.5x faster than g1, never 4x.
- Q10's customer scan is the clearest case: exactly 2.00x (438 → 219 ms), and g2s (same switch) is 424 ms, which is
  no faster than g1.

**Pricing:** the sum of scan-phase gap shares.
- Q3: P1 scans 46.6 + P3 127.8.
- Q10: P1 35.4 + P3 103.4 + P6 109.1.

**Lever (M9):** the customer table is pinned **uncompressed** (23.3 GB H2D for Q10).
- If customer compressed about 2.5x like orders, Q10's g4 customer scan would drop from 219 to about 90 ms (about
  −125 ms at g4, about −250 ms at g1).
- This is an estimate: bytes / 105 GB/s plus decode, assuming decode is not limiting, as it is not for orders.

### F2. Q3: the HJ(15) probe shuffle moves 38.5 GB of the lineitem probe. Gap share 260 ms; measured saving 401 ms at g4.

**Mechanism:**
- The orders⋈customer build has 147M rows: 2.2 GB, with an estimated hash table of 2.35 GB.
- At N=4 it is BUILD_PROBE with P = min(natural, N) = 4 (`src/op/sirius_physical_hash_join.cpp:873,887`).
- Broadcast is blocked because the build is above `max_broadcast_join_size` = 256 MiB
  (`sirius_config.hpp:67`, `sirius_physical_hash_join.cpp:867-869`).
- The lineitem dynamic filter that should shrink the probe is refused at both g1 and g4: `accumulated Bloom refused:
  per-GPU cap, rows=147060214, required_bytes=294120448, cap=268435456` (g1_a and g4_a logs, every iteration;
  `dynamic_filter_publisher.cpp:641-654`; default cap at `src/op/dynamic_filter/config.hpp:27`).
- So the probe is 99.4 GB host → **51.4 GB** of GPU bytes, hash-partitioned 4 ways. 3/4 of it is cloned to the
  partition owner: 2/3 of that host-staged (no cross-switch P2P), 1/3 P2P.

**Evidence:**
- nsys g4 (`clone_by_pipe_g4.txt`), CONCAT(14):
  - 1254 host-staged clones, 25.68 GB, 5458 thread-ms. 5005 ms of that is in `cuStreamSynchronize`, and only 1517 ms is
    copy-engine time.
  - 627 P2P clones, 12.84 GB, 553 thread-ms.
  - Clones are 95% of CONCAT(14) task time (6011 of 6360 thread-ms).
- Effective rate per clone thread: staged 4.7 GB/s vs P2P 23 GB/s. The aggregate during the phase is 57 GB/s H2D plus
  57 GB/s D2H.
- Quent: CONCAT(14) GPU-origin prepare is 5879 thread-ms (g1: 3.7). The P4 span is 522 ms vs 232 ideal.
- 2-GPU check (Quent CONCAT(14) GPU-prepare, 25.7 GB moved in both 2-GPU configs): g2s (all P2P) 1537 thread-ms,
  g2x (all staged) 2868, g4 5879 for 38.5 GB moved.

**Measured counterfactual (`x_g4_joinpol`):**
- Q3 falls from 1.056 to **0.655 s (−401 ms)**.
- HJ(15) becomes broadcast and its dynamic filter is published directly (147M rows, no accumulation).
- Probe bytes drop from 51.4 to 6.9 GB, and P4 drops from 522 to **51 ms**.
- P2 grows from 50 to 128 ms: broadcasting the 2.2 GB build costs 380 thread-ms of clones, CONCAT(10) 135-294 ms.

**Residual leak under joinpol:**
- The probe scan starts at 243 ms, before the broadcast build finishes (294 ms).
- 57-99 of 627 lineitem DF ops ran before the filter was published (283-308 ms after query start, aligned from the
  log).
- So about 6.4 GB passes unfiltered. g1 joinpol gets 0.5 GB because there the scan starts after the build.
- Cost: about 37 ms (P4 gap under joinpol).

**Pricing:** the gap share is P4 gap 290.8 minus the PARTITION allocator part 36 (F4) = 255 Quent → 260 ms. The
absolute g4 saving is the measured 401 ms. This is one finding with two causes, the broadcast cap and the Bloom cap.
Either one alone removes most of the probe bytes. I estimate each alone saves 300-370 ms at g4; neither was measured
separately.

### F3. Fan-in clones of per-task fragments: 184 ms (Q3 43, Q10 141). Design/mechanical.

**Mechanism:**
- Each producer task emits P fragments.
- The consumer CONCAT or MERGE (one task per partition, so one task per GPU at P=4) clones every remote fragment
  serially in its prepare phase. The steps:
  - `convert_gpu_to_gpu` runs `stream.sync` at entry and at the end
    (`cucascade/src/cudf/representation_converter_builtins.cpp:902,947`).
  - The stream comes from the shared `acquire_stream()` pool (`:926`).
  - A staged buffer does D2H, then `target_stream.sync()`, then H2D, then sync (`:589-697`, syncs at `:665,:694`).
- The cost scales with the fragment count, not the bytes. 3 of 4 executor threads on each GPU sit idle, which is why
  Q10's thread occupancy is 0.69.

**Evidence, Q10 CONCAT(9) (lineitem probe):**
- 4 tasks. Each clones about 470 of its 627 pieces: 1254 staged plus 627 P2P, 1.4 GB in total.
- Staged clones average 0.235 ms each; P2P clones 0.085 ms.
- Prepare is 82 ms per task (Quent 496.8 → 578.6 on every GPU). The span is 103 ms vs 8 at g1.
- CONCAT(5) (163 orders pieces): 489 clones, 23 ms span.
- CONCAT(12): 16 ms.
- Q3 CONCAT(10) build: 28 ms. Q3 CONCAT(3): 6.7 ms.
- `x_g4_hpb256` (4x more partitions) makes Q10 slower: 1.196 → 1.237 s (+41 ms). Q3 is flat at 1.056. That is
  consistent with a cost that grows with the fragment count.

**Pricing:** gap shares.
- Q10: P2 CONCAT(5) 21.9 + P4 CONCAT(9)/HJ(10) 102 + P5 CONCAT(12) 14 = 137.9 Quent.
- Q3: P1 6.7 + P2 36.6 = 43.3.
- Overlap with F9: 22% of CONCAT(9) task time is allocator.

**Rev2, measured (batch4g, about 4x fewer fragments):**
- Q10 P2 + P4 fall by 190 Quent ms at g4. This covers F3 and F4 together on those phases.
- CONCAT(9) clone prepare falls from 328 to 122 thread-ms; CONCAT(5) from 74.5 to 37.4.
- CONCAT(12) and CONCAT(16) are unchanged. Their producers (4 HJ(10) partitions, and 31 → 7 customer tasks feeding a
  4-way partition) are bytes-bound rather than fragment-bound.
- Residual under `best`: Q10 P4 42.5 + P2/P5 non-Bloom parts 27.5 = 70 Quent → **72 ms** (§1.5).

### F4. N-forced partitioning (P≥N floor), allocator-bound: 159 ms (Q3 37, Q10 122). Design + mechanical.

**Mechanism:**
- `natural_num_partitions` floors P to N once the input is ≥ N x 16 MB (`src/op/partition/partition_sizing.hpp:86-120`).
- BUILD_PROBE then runs at min(natural, N) (`sirius_physical_hash_join.cpp:873`).
- At g1 the same exchanges are pass-through:

  | partition | g1 (thread-ms) | g4 (thread-ms) |
  |---|---|---|
  | Q10 PARTITION(8) | 8.7 | 1524 |
  | Q10 PARTITION(4) | 2.1 | 372 |
  | Q3 PARTITION(18) | 0.8 | 166 |

- Each task hash-partitions and then deep-copies P slices (`src/op/partition/gpu_partition_impl.cpp:212-221`): 23
  `cudaMallocFromPoolAsync` plus 11 `cudaFreeAsync` per task.

**Evidence (nsys `range_api.py`, `pipe_api.txt`):**

| | Q3 PARTITION(13) per task | Q10 PARTITION(8) per task |
|---|---|---|
| wall, g1 → g4 | 1.52 → 3.23 ms (g1 is the 54-way STANDARD partition) | 3.02 ms at g4 (0.009 ms pass-through at g1) |
| malloc | 6.6 → 68 µs | 69.5 µs at g4 |
| free | 2.5 → 77 µs | 74 µs at g4 |
| allocator share of task time | 10.7% → **74.8%** | **79.5%** at g4 |
| GPU kernels | 230 → 171 ms total | 17 ms total across 627 tasks |

- Q3 PARTITION(18): 82% allocator.
- The PARTITION work runs as a tail after the scan: Q10 at 390-494 ms, after the scan ends at 399. With
  `priority=source` (`src/creator/config.hpp:106`), scan tasks drain first.

**Pricing:** gap shares.
- Q10: P2 PARTITION(4) work 22.9 (excluding the Bloom 20.5) + P4 PARTITION(8) tail 93 + P5 PARTITION(11) work 4
  (excluding the Bloom 41) = 119.9.
- Q3: P4 PARTITION(13) tail 97 − 61 (g1/4) = 36.

### F5. Group-by re-shuffle after a co-partitioned join, plus MERGE fan-in: 129 ms (Q3 25, Q10 104). Design.

**Mechanism:** `wrap_hash_group_by` always inserts PARTITION(FULL) → MERGE_GROUP_BY
(`src/planner/sirius_physical_plan_generator.cpp:1425-1475`) and never checks whether the input is already
hash-partitioned on a subset of the group keys.
- Q10: HJ(17) is partitioned on c_custkey, which is a group key, yet PARTITION(24) re-hashes 8.1 GB of partials into
  9 partitions and moves 3/4 of them.
- Q3: HJ(15) is partitioned on l_orderkey, which is a group key.

**Evidence (Q10 MERGE_GROUP_BY at g4):**
- 9 tasks, GPU-origin prepare 690 thread-ms (77 ms per task) vs 0.5 at g1.
- nsys: 36 staged clones (4.04 GB, 18.6 ms each, about 6 GB/s per clone) and 18 P2P clones (2.02 GB).
- P8 span: 138 ms at g4 vs 143.5 at g1. No speedup at all.

**Pricing:** P8 gap 102.5 (Q10) and P5 gap 24.6 (Q3), x1.019. If the per-partition aggregate were final, P8 would
shrink to roughly a top-N plus merge (about 10-25 ms).

### F6. Q10 customer-side join exchange (P7): 77 ms. Design.

- CONCAT(16) clones the customer probe: 62 staged clones (3.46 GB, 6.9 ms each) plus 31 P2P clones (1.73 GB). That
  is 496 thread-ms of GPU-origin prepare vs 0.2 at g1.
- PARTITION(15) is a forced 4-way partition: 148 thread-ms vs 0.5.
- HJ(17) is not a broadcast candidate even under joinpol. The estimated probe/build ratio is 150M / 60M = 2.5, below
  1.25N = 5 (`sirius_physical_hash_join.cpp:867-869`).
- Priced as the P7 gap: 75.3 Quent → 77.

### F8. Accumulated Bloom HOST-staged publication holds the last build-side task: 63 ms (Q10 only). Design.

**Mechanism:**
- At g4 the build is multi-partition, so Bloom partials accumulate.
- The task that makes the final contribution runs the reduction and the replication synchronously, through HOST
  staging ("Runs on the task that owes the publication", `src/op/dynamic_filter/dynamic_filter_publisher.cpp:381-425`).
- The downstream CONCAT is created only after that task finalizes.
- At g1 the build is a single partition and publishes directly (0 ms).

**Evidence:**
- The log says `published through HOST staging ... 21.6 ms` (orders Bloom, 163 batches, 115 MB per GPU) and `44.8 ms`
  (customer-side Bloom, 229 MB per GPU), in every iteration.
- Quent, deterministic across iters 2-4:
  - PARTITION(4): the last task finalizes 20.5-20.7 ms after the second-to-last (163 tasks, median compute 2.1 ms, max
    21.2 ms).
  - PARTITION(11): the last task finalizes 41.0-41.1 ms after the others (3.8 ms median vs 44.8 ms max).
  - CONCAT(12) is created at 650.4 ms, the same instant PARTITION(11) on GPU 2 finalizes.
- The *probe-side* version of this hypothesis (unfiltered early probes) is **falsified** for Q10:
  - The probe scans start after publication (lineitem at 150 ms vs published at about 124; customer at 665 vs about
    650).
  - Bytes into PARTITION(8) and PARTITION(15) are identical at g1 and g4.
- Q3 has no publication: the Bloom is refused (F2).

**Pricing:** 20.5 + 41.0 = 61.5 Quent → 63 ms.

**Rev2:** this cost is untouched by every config flip. Under `best` the log shows 21.0-21.2 ms and 44.5 ms in each
iteration (`x_g4_best/log`), and the phase spans hold: Q10 P2 is 36.7 and P5 is 60.1 Quent ms. It now accounts for
67 ms of the 647 ms residual Q10 gap, about 10%.

### F9 (overlapping F3/F4). Driver stream-ordered allocator contention: about 195 ms absolute at g4. Mechanical.

**Evidence (nsys `api_latency_q3_q10.txt`):**

| | Q10 g1 | Q10 g4 | Q3 g1 | Q3 g4 |
|---|---|---|---|---|
| `cudaMallocFromPoolAsync` calls | 37.8k | 66.4k | 46.1k | 56.4k |
| mean malloc latency | 4.7 µs | 44.6 µs (p90 115) | 17.4 µs | 31.5 µs (p90 86) |
| `cudaFreeAsync` mean | 1.8 µs | 36.5 µs | 2.6 µs | 23.5 µs |
| malloc + free thread-ms | 246 | **5382** | 922 | **3102** |

- OSRT inside those calls: `pthread_rwlock_wrlock` plus `pthread_mutex_lock`, for example 730 + 611 ms inside Q10
  PARTITION(8).
- Microbench (`microbench/alloc_contention/results.txt`):
  - Process-wide throughput is about 280-300k calls/s regardless of the number of GPUs.
  - Mean latency is 10.9 µs at 4 threads x 1 GPU and 50-56 µs at 4 threads x 4 GPUs.
  - grant=0 and grant=1 are the same, so pool peer-access grants (PR #2302) are not the cause.
  - So the driver serializes these calls process-wide.
- Q10 lineitem scan: the allocator is 66% of task time at g4 (3062 of 4624 thread-ms) vs 5.8% at g1. But the
  un-profiled scan runs at 106 GB/s, the link ceiling, so I price this part at 0 wall.

**Pricing (wall, at g4) = phase span x allocator share of that phase's task time:**
- Q3: PARTITION(13) tail 97 x 0.75 = 73; PARTITION(18) 11.5 x 0.82 = 9; MERGE 4.
- Q10: PARTITION(4) work 22 x 0.70 = 15; PARTITION(8) 95 x 0.795 = 76; CONCAT(9) 103 x 0.22 = 22; CONCAT(5) 4.
- Total about 203 ms. About 90% (about 180 ms) is recoverable at g1-like per-call latency.

This is the dominant cost *inside* F4, and part of F3. It is not additive to them.

**Rev2, the thr8 test.** If the scan were purely link-bound, 8 threads per GPU would leave it unchanged. That is what
happens at g1: Q10 1709.7 → 1708.9 Quent ms.
- At g4 Q10 slows by 97 ms. The lineitem scan drops from 106 to 82 GB/s (span 249 → 321 ms).
- PARTITION(8) compute grows 2.47x for identical work.
- This matches a process-wide serialized allocator with a fixed calls/s budget: more callers mean longer queues, not
  more throughput.
- So at g4 the allocator is **co-binding with the link** on the Q10 lineitem scan. A faster link alone would hit the
  allocator next, with a scan floor of roughly 26.5 GB / (allocator-limited rate).
- I keep the scan part priced at 0 (it is not separable from F1 at baseline), but note the risk: F1's link share is
  only recoverable together with R3.

### F7. Measurement: the join-mode and Bloom confound flatters Q3 at g4. Config/measurement.

- At g1, HJ(15) needs a 147M x 16 B = 2.35 GB table. That is above `max_build_hash_table_bytes` = 2GB, so the join
  runs STANDARD with a 54-way partition.
- So at g1:
  - HJ(15) costs 2144 thread-ms vs 315 at g4.
  - PARTITION(13) is 945 thread-ms of real work.
  - Because the build is multi-partition, the accumulated Bloom is refused, and the probe stays at 51.4 GB.
- At g2/g4, P = 2 or 4 makes BUILD_PROBE eligible (C4 in CRITIC).
- `x_g1_joinpol` shows Q3 at g1 dropping from **2.031 to 1.164 s (−867 ms)**: the filter applies directly and the
  probe falls from 51.4 to 0.5 GB.
- So the honest Q3 speedup is 1.78x, not 1.92x, and the family's is 1.56x, not 1.67x.
- Under equal algorithms the Q3 gap is 364 ms (un-profiled):
  - link 196 (P1 + P3)
  - **broadcast-build replication 116** (CONCAT(10): 6.6 GB moved by 1 task per GPU, about 41 GB/s aggregate)
  - Bloom leak 39
  - final merge 9

## 3. Candidate remedies (priced at g4 wall on this family)

| # | remedy | targets | est. g4 saving | basis |
|---|---|---|---|---|
| R1 | `max_broadcast_join_size` 8GB + derived `max_build_hash_table_bytes` (joinpol) | F2 | **−401 ms Q3**, 0 on Q10 (also −867 ms at g1) | measured (x_g4/x_g1_joinpol) |
| R1b | `scan_task_batch_size` 4GB (batch4g) | F3/F4 fragment count | **−165 ms Q10, −46 ms Q3** at g4. Also −165/−51 at g1, so the ratio barely improves. Costs +30 ms of scan imbalance on Q10 customer (7 tasks on 4 GPUs) | measured |
| R1c | R1 + R1b (best) | F2, F3, F4 | **−562 ms family** (2.252 → 1.690 s). Remaining gap 1.027 s, decomposed in §1.5 | measured |
| R2 | Broadcast decision from *measured* build rows / probe bytes, not planner estimates. Q10 HJ(10) build est 300M rows vs 57.4M actual; ratio test fails | F3/F4 on Q10 P4 | about −140 ms Q10 (remove PARTITION(8) 93 + CONCAT(9) 102, add about 50 ms to replicate 0.7 GB x 3, scaled from Q3's 160 ms for 6.6 GB) | estimate |
| R3 | Pool device memory per GPU/thread in front of the driver allocator (RMM pool/arena), so hot-path alloc/free never enter `cudaMallocFromPoolAsync` | F9 (inside F3/F4) | about −180 ms family (Q3 about 75, Q10 about 105) | nsys alloc share x span |
| R4 | Pre-combine fragments per source GPU before the exchange, or clone all remote fragments of a task in one batched transfer with one sync | F3, part of F5 | about −130 ms (70% of F3) | estimate from clone counts/latency |
| R5 | Pipelined staging: overlap D2H/H2D chunks across buffers, no per-buffer syncs, parallel clones per consumer | F2 residual, F5, F6, joinpol broadcast | Q3 baseline CONCAT(14) 425 → about 250 floor (−175). Under joinpol: broadcast 160 → about 45 (−115). Q10 MERGE/CONCAT(16) about −60 | uplink-floor arithmetic: 12.8 GB per uplink / 52 GB/s |
| R6 | Partitioning-property propagation: skip PARTITION+MERGE when the input is already partitioned on a group-key subset | F5 | about −100 ms Q10, −20 ms Q3 | P8/P5 spans minus top-N |
| R7 | Async Bloom publication (off the build task), or peer transport intra-switch | F8 | −63 ms Q10 | measured stall |
| R8 | Compress customer (Simpatico plan), plus o_comment/l_shipmode per M9 | F1 | about −125 ms Q10 at g4 (−250 at g1) | bytes / 105 GB/s |
| R9 | Raise `max_dynamic_filter_bloom_bytes_per_gpu` to 512MB (with no broadcast) | F2 | likely −300 ms or more on Q3 at g4 and about −850 at g1. Superseded by R1 | estimate |
| R10 | Start the probe scan only after the broadcast build's filter is published, or publish the filter before replication | F2 residual under joinpol | about −37 ms Q3 (under joinpol) | measured leak |

**Rev2: remaining headroom on top of `best`** (g4 1.690 s, gap 1.027 s; un-profiled ms, from §1.5):

| remedy | on top of `best` | basis |
|---|---|---|
| R5 pipelined staging, plus switch-aware or batched clone of broadcast/exchange | Q3 broadcast phase 105 → about 45; Q10 P7+P8 217 → about 120. Total about **−160** | about 41 GB/s achieved vs 104 GB/s for two uplinks on the replication |
| R6 partitioning-property propagation | Q10 P8 108 → about 25. Q3 P4 tail is already small. Total about **−85** | P8 span minus top-N |
| R7 async Bloom publication | **−67** Q10 | measured log stall, unchanged under `best` |
| R8 compress customer | **−125** Q10 | bytes / 105 GB/s |
| R2 runtime broadcast for Q10 HJ(10) | about 0. P4 is already only 44 ms under batch4g, and replication costs about 50 | superseded by batch4g |
| R3 allocator pool | about −35 on the exchange tails (PARTITION(8) compute is down to 178 thread-ms). The larger value is unblocking the scan, see F9 Rev2 | nsys share x residual span |
| R10 Bloom-before-probe | ≤ 11 Q3 (P4 tail) | measured |
| scan RR-binding of 7 large batches | −29 Q10. Work-steal or size batches as a multiple of N | §1.5 P6 |

The non-link residual after these is about 0.1 s. The family's floor at g4 then becomes the link: 519 ms of gap, or
about 400 ms after R8. That puts the achievable family scaling at about 2.651 / (0.663 + 0.52 + 0.1) ≈ 2.1x on this
host topology, unless bytes shrink further (M9) or the host attaches over more uplinks.

`priority=source` makes forced partitioning a post-scan tail. Interleaving it with the scan, which waits on the link,
could hide part of F4. That is not priced: allocator contention might then slow the scan.

## 4. Not pursued / not priced

- **N4, shared `acquire_stream()` pool:**
  - Staged clone waits are 3.3x their copy-engine time (Q3: 5005 ms of sync vs 1517 ms of copy). Part of that is
    serialized legs and engine queueing; part could be stream sharing.
  - I did not separate them. They are folded into F2/F3.
- **P mod N imbalance:** Q10 MERGE has 9 partitions on 4 GPUs, 3 on GPU0. With 4 threads per GPU, ≤3 tasks run in
  parallel, so the extra partition costs no wall time.
  - Per-GPU busy is balanced (93-99%) and task counts are equal ±2.
  - Not a factor here.
- **Experiments (Rev2: all landed; see §1.1 and §1.6):**
  - `x_g4_hpb256`: Q3 flat at 1.056, Q10 +41 ms (1.237 s). It hurts by adding fragments, consistent with F3.
  - `x_g4_hpb4g`: Q3 +15, Q10 +46. A coarser hash_partition_bytes does not help this family. The P≥N floor keeps
    P=4 on the forced exchanges, so only the natural-P merges change. Not decomposed further.
  - `x_g4_batch8g`: Q3 1.022, Q10 1.049. No better than batch4g (1.010 / 1.031). Fragment savings saturate, and
    scan-task imbalance grows.
  - `x_g4_gpupin`: unusable for this family. Q3 iterations were 4.19/1.76/1.90 s and Q10 was 1.57 s. That comes from
    downgrade/convert failures and memory pressure (`X3_link.md`), not from the link.
  - `x_g1_thr8` Q3 +254 ms: a spill artifact (CONCAT(14) HOST-origin inputs), not a scaling signal.
- **Allocator cost inside the scan:** 2.6 s thread-ms in Q10 at g4 (`sirius::compression::host_to_gpu`). It is hidden
  under the link ceiling in un-profiled runs, so priced at 0.
- **Q10 HJ(21) + group-by compute:** 459 vs 769 thread-ms. This is a GPU-side gain at g4, so it is not an impediment.
