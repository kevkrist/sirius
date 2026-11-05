# Family "semijoin" (TPC-H Q4, Q12, Q21): why 4 GPUs is not 4x — v2 (all config flips integrated)

Regime: SF1000, host-pinned, bb557b1a. Absolute times are un-profiled (timings.csv, Quent of `00-baseline/{g1_a,g2s_a,g2x_a,g4_a}`
and `20-experiments/x_*`, hot iters 2-4). nsys (`01-nsys/nsys2_{g1,g4}` iter2, plus `01-nsys/nsys3_g4` rep.3 = Q21) is used
only for proportions (API shares, per-call latency, kernel vs wall).
Work files: `10-analysis/fam_semijoin/` — `pw_side_flips.txt` (per-pipeline exclusive wall, g1/g1-batch4g/g4/g4-batch4g/thr8/gpupin/joinpol),
`pipe_acct_{g1_g4,g4_vs_b4,b4_g1_g4,g4_vs_thr8}.txt` (per-pipeline thread-time, tasks, bytes), `wall_decomp*.txt`,
`nsys_timeline.txt`, `task_api.txt`, `api_latency.txt`, `nsys_bloom.txt`; scripts `pipe_wall.py`, `pw_side.py`, `pipe_acct.py`,
`wall_decomp.py`, `task_api.py`, `nsys_timeline.py`; flattened Quent of the flips in `fam_semijoin/qf/`.

**Bottom line**
- Family gap to linear at g4 = **2239 ms** (Q4 418, Q12 640, Q21 1182) = 20% of the 11.08 s suite gap.
- Shape: big lineitem/orders scans (link-bound at g4) alternate with key exchanges (PARTITION -> CONCAT -> join) that are
  pass-through or near-free at g1.
- **Largest cause is per-task fixed cost that inflates at g4** (allocator serialization in every PARTITION/scan task +
  per-fragment fan-in clones + N-forced hash-partition calls): priced 1059 ms (47%). Measured: `scan_task_batch_size=4GB`
  (4.3x fewer tasks) removes **931 ms** of family Quent wall at g4 (timings: -943), with the link-bound scans unchanged.
- Host link ceiling: 728 ms (33%), confirmed by tier=gpu pin (link-bound scans drop 2.0-2.8x).
- Bulk shuffle bytes via serialized host staging: 292 ms (13%, mostly Q21). Accumulated-Bloom host-staged finish: 92 ms (4%),
  batch-invariant. Host-serial outside query span: 54 ms.

## Time accounting

Wall ms, un-profiled. Ideal = g1/4.

| | Q4 | Q12 | Q21 |
|---|---|---|---|
| g1 / g2s / g2x / g4 (timings.csv) | 822 / 733 / 636 / 623 | 1702 / 1529 / 1134 / 1065 | 3240 / 2704 / 2149 / 1992 |
| speedup g2s / g2x / g4 | 1.12 / 1.29 / 1.32 | 1.11 / 1.50 / 1.60 | 1.20 / 1.51 / 1.63 |
| ideal g4 / **gap** | 206 / **418** | 426 / **640** | 810 / **1182** |
| Quent wall g1 -> g4 | 806 -> 606 | 1685 -> 1049 | 3203 -> 1954 |
| per-GPU busy at g4 (Quent, some task in flight) | 95-96% | 97-98% | 93-95% |
| thread-time g1 -> g4 (4 -> 16 threads) | 2967 -> 7471 (2.52x) | 6551 -> 14292 (2.18x) | 11953 -> 22333 (1.87x) |
| - HOST-origin prepare (H2D+decode) | 1814 -> 3282 | 3823 -> 6536 | 6807 -> 11928 |
| - GPU-origin prepare (cross-GPU clones) | 13 -> 377 | 7 -> 488 | 27 -> 1904 |
| - compute | 1140 -> 3812 | 2721 -> 7268 | 5119 -> 8501 |
| SCAN-phase wall g1 -> g4 (ideal) | 655 -> 294 (164) | 1636 -> 711 (409) | 2322 -> 997 (581) |
| PARTITION-phase wall g1 -> g4 | 48 -> 154 | 9 -> 170 | 196 -> 324 |
| CONCAT-phase wall g1 -> g4 (clone-prepare-only part) | 19 -> 96 (73) | 17 -> 154 (116) | 71 -> 430 (364) |
| MERGE / join+groupby compute g1 -> g4 | 3 -> 19 / 79 -> 41 | 1 -> 3 / 19 -> 7 | 38 -> 30 / 571 -> 166 |
| host time outside Quent span g1 / g4 | 16 / 17 | 17 / 17 | 37 / 38 |
| **flips at g4**: batch4g / joinpol / best / thr8 / gpupin / hpb256 / hpb4g | 381 / 622 / 417 / 712 / 574 / 631 / 623 | 780 / 872 / 745 / 1137 / 763 / 1062 / 1060 | 1576 / 1973 / 1558 / 2092 / 2033* / 2315 / 1966 |
| flips at g1: batch4g / joinpol / best / thr8 | 694 / 778 / 674 / 793 | 1551 / 1703 / 1549 / 1679 | 3000 / 3180 / 2935 / 3231 |
| scaling g1->g4 under batch4g / joinpol / best | 1.82 / 1.25 / 1.62 | 1.99 / 1.95 / 2.08 | 1.90 / 1.61 / 1.88 |
| thread-time g1 -> g4 under batch4g | 2505 -> 4478 (1.79x) | 5988 -> 10925 (1.82x) | 11116 -> 17375 (1.56x) |

\* gpupin confounded by GPU memory pressure (downgrades; Q21 IDLE 6.6 -> 65 ms, HASH_GROUP_BY(42) 5 -> 77 ms).

Kernel vs wall inside NVTX ranges at g4 (nsys): exchange phases are host-bound, not GPU-bound.
- Q12 PARTITION(2) phase: kernels 6 ms/device in a 182 ms span; `hash_partition` 27 ms kernel in 1788 ms thread-time, of which
  cudaMallocFromPoolAsync 712 + cudaFreeAsync 612 ms.
- Q21 (independent capture nsys3_g4 rep.3): `sirius_physical_partition::execute` n=2095, thread-wall 4903 ms, launched kernels
  134.5 ms (2.7%), malloc 2379 + free 1336 ms (76%), osrt pthread_rwlock_wrlock 1809 + pthread_mutex_lock 1463 ms (inside those calls).
- Q4 `hash_partition` 25 ms kernel in 1286 ms thread-time; at g1 PARTITION(10) is pass-through, 7.3 ms total.
- Scan phases run at the host-link ceiling: Q12 P0 66.5 GB in 666 ms (99.8 GB/s), Q21 P12/P24/P39 105 GB/s, Q4 P6 89 GB/s;
  g1 runs the same scans at 41-44 GB/s.

## Findings (ranked by family wall ms at g4)

Pricing method (all findings): per pipeline, excess = g4 exclusive wall (Quent, un-profiled; `pipe_wall.py` gives each 0.1 ms bin to
the in-flight pipeline, scans first) minus g1 wall/4. Each pipeline's excess is assigned to mechanisms; sums close to the measured
gap within 5 ms. Splits inside a pipeline use (a) nsys API fractions of task thread-time applied to the un-profiled phase wall
(valid because these phases have 10-14 of 16 threads busy), and (b) the batch4g flip as a dose-response on task/fragment count
(627 -> 145 lineitem tasks, 163 -> 38 orders tasks; bytes identical).

| # | Mechanism (kind) | Q4 | Q12 | Q21 | **Family** | Conf. |
|---|---|---|---|---|---|---|
| F1 | Host-link ceiling on lineitem/orders scans (hardware) | 81 | 262 | 385 | **728** | high |
| F3 | Driver stream-ordered allocator = process-global serialization point, hit per task (mechanical/design) | 144 | 134 | 198 | **476** | high |
| F2a | Fan-in: serial per-fragment cross-GPU clones, cost proportional to fragment count (design+mechanical) | 91 | 145 | 198 | **434** | high |
| F2b | Bulk shuffle bytes through serialized 2-leg host staging, single flow per consumer (design/hardware) | 23 | 8 | 261 | **292** | med |
| F4 | N-forced hash partitioning (pass-through at g1), non-allocator part (design/config) | 38 | 62 | 49 | **149** | med |
| F5 | Accumulated Bloom finish, HOST-staged, fully serial, on critical path (design) | 22 | 12 | 58 | **92** | high |
| F6 | Host-serial work outside Quent span (DuckDB plan/result) (design) | 13 | 12 | 29 | **54** | med |
| F7 | Other: P mod N tail vs join-mode gains (net) | 5 | 4 | 0 | **10** | low |
| | **Sum (measured gap)** | 417 (418) | 639 (640) | 1178 (1182) | **2235 (2239)** | |

Grouping: per-task fixed cost F2a+F3+F4 = **1059 ms** (47%), validated by batch4g removing 931 ms (88%) of family Quent wall;
everything that exists only because of the g4 shuffle (F2a+F2b+F3-in-PARTITION+F4+F5) = 434+292+345+149+92 = **1312 ms** (59%).

### F1 Host-link ceiling (hardware): 728 ms
- Floor = nsys H2D GB / 105 GB/s (two Gen5 switch uplinks). g4 exclusive wall vs floor: Q12 P0 637 vs 633; Q21 P12 375 vs 381,
  P39 374 vs 381, P24 203 vs 209; Q4 P6 207 vs 209. g1/4 of the same bytes would need ~170 GB/s.
- Price: g4 wall - g1/4 per link-bound pipeline: Q4 P6 +81; Q12 P0 +262; Q21 P12 +152, P24 +83, P39 +150.
- Confirmation: `x_g4_gpupin` (no host link) drops these scans 2.0-2.8x: Q12 P0 637 -> 313, Q4 P6 207 -> 92,
  Q21 P13 375 -> 189, P47 374 -> 133, P28 203 -> 83 (`pw_side_flips.txt`); Q12 whole query 1065 -> 763 (-302).
  `x_g4_batch4g` leaves them unchanged (637 -> 627, 375 -> 368, 374 -> 368, 203 -> 202): link, not task overhead.
- g2s (one switch, 52 GB/s) Q12 scan 1502 -> 1239 ms only (1.2x), consistent.

### F3 Driver allocator serialization (mechanical/design): 476 ms
- Each memory_space allocates via `rmm::mr::cuda_async_memory_resource` (`cucascade/src/memory/memory_space.cpp:127`), so every
  allocation/free is a `cudaMallocFromPoolAsync`/`cudaFreeAsync` driver call. PARTITION allocates per column per partition:
  `cudf::hash_partition` (`src/op/partition/gpu_partition_impl.cpp:187`) plus per-slice deep copy (`:212-216`).
- Microbench (`microbench/alloc_contention/results.txt`, no profiler): process-wide cap ~280-385k calls/s; mean latency 10.9 us
  (1 GPU x 4 thr) vs 55.9 us (4x4) vs 103.8 us (4x8); pool peer grants make no difference (50.4 vs 55.9 us).
- Engine nsys (`api_latency.txt`), malloc mean g1 -> g4: Q4 9.1 -> 57.7 us, Q12 8.5 -> 32.8, Q21 7.2 -> 31.0; malloc+free
  thread-time Q4 425 -> 5966 ms, Q12 628 -> 5152, Q21 892 -> 6809. Q12 PARTITION(2): 28.2k calls in 182 ms = 155k/s with 9.8
  threads inside the allocator on average (g1 analogue 0.9).
- Same work, more cost: Q4 PARTITION(17) hash-partitions at both counts (163 tasks, 1.17 GB): 0.77 ms/task at g1 vs 3.25 at g4
  (4.2x); Q21 PARTITION(16) 0.54 -> 2.08 ms/task (3.8x) (`pipe_acct_g1_g4.txt`).
- Dose-response on threads (`pipe_acct_g4_vs_thr8.txt`): `num_threads 8` at g4 raises PARTITION thread-time 2.5x
  (Q12 PARTITION(2) 2005 -> 5135 ms, 3.2 -> 8.2 ms/task; Q21 PARTITION(16/30/50) 1303/1194/1172 -> 3409/3102/2999) while
  wall rises only 18-27% (139 -> 174 ms): a throughput-capped shared resource. Whole queries +89/+72/+100 ms at g4, but
  -29/-23/-9 ms at g1.
- Price = allocator excess fraction (calls x (g4 - g1 latency)) of task thread-time x un-profiled phase wall:
  scans/group-by Q4 60, Q12 41, Q21 31; PARTITION Q4 84, Q12 93, Q21 168. Cross-check: 28.2k calls of Q12 PARTITION(2) need
  >= 94 of its 139 ms at the microbench cap.

### F2a Per-fragment fan-in clones (design + mechanical): 434 ms
- Every upstream task's PARTITION emits P=4 fragments; each CONCAT/MERGE consumer clones its ~470 remote fragments **one at a
  time** in Preparing (`src/op/sirius_physical_operator.cpp:140-190` -> `batch_lock_utils.hpp:137` -> cuCascade `convert_gpu_to_gpu`,
  `representation_converter_builtins.cpp:892-960`), with a sync at entry/exit and staged legs D2H -> sync -> H2D -> sync (`:598-697`).
  One consumer task per GPU, so 4 of 16 threads work in these phases.
- nsys Q12 CONCAT(3) per task: 470 converts (314 host_staging 77.5 ms = 0.25 ms each; 156 peer_copy 18.0 ms = 0.115 ms each),
  3768 cuStreamSynchronize, kernels 1.9 ms, payload ~0.14 GB (~4 ms at link speed).
- Dose-response: wall = a + b x fragments, fitted from g4 vs g4-batch4g (fragments 4.3x fewer, bytes equal):
  Q12 CONCAT(3) 109.1 -> 28.5 (b 0.167 ms/frag, a 4.3); CONCAT(7) 46.2 -> 15.0; Q21 CONCAT(17) 95.3 -> 32.2, CONCAT(31) 98.7 -> 53.1,
  CONCAT(51) 86.2 -> 42.5; Q4 CONCAT(11) 52.3 -> 19.1, CONCAT(18) 48.2 -> 20.1, MERGE_GROUP_BY(4) 16.7 -> 7.9. GPU-origin prepare
  thread-time Q12 488 -> 141, Q4 377 -> 164.
- Price = b x fragments at baseline: Q4 91, Q12 145, Q21 198.
- Topology: Q12 CONCAT(3) g2s (all P2P) 36.8, g2x (all staged) 65.9, g4 109.1 ms: staging ~doubles per-clone cost.
  joinpol does not help (CONCAT(3) 109 -> 117 as broadcast).

### F2b Bulk shuffle bytes via serialized staging (design/hardware): 292 ms
- Remainder of CONCAT/MERGE excess not removed by fewer fragments: Q21 few-fragment CONCATs fed by joins/orders, unchanged under
  batch4g: CONCAT(12) 59.4 (5.85 GB), CONCAT(35) 43.8 (3.51 GB), CONCAT(38) 37.4 (2.49 GB), CONCAT(55) 26.7 (2.21 GB), CONCAT(27)
  11.6, CONCAT(46) 10.3, MERGE_GROUP_BY(25) 16.7, plus intercepts of CONCAT(31) 39 and (51) 29 ms.
- Example CONCAT(35): 4 consumers x 37 ms clone-prepare for ~0.66 GB each = ~18 GB/s per consumer vs 43-54 GB/s single-flow
  cross/intra copy (microbench p2p): legs are serialized (D2H, sync, H2D, sync), one clone at a time.
- Price = g4 - g1/4 of these pipelines minus F2a: Q4 23, Q12 8, Q21 261. Bandwidth floor of these bytes at 77-105 GB/s agg is
  roughly 40% of it (estimate); the rest is serialization.

### F4 N-forced hash partitioning (design/config): 149 ms beyond F3
- `partition_min_num_partitions(N)=N` (`src/op/partition/partition_sizing.hpp:86,115`); P=1 means pass-through
  (`src/op/partition/sirius_physical_partition.cpp:157`). Same input bytes at g1 and g4 (Q4 PARTITION(10) 1.20 GB both):
  Q12 PARTITION(2) 8.0 -> 139 ms, PARTITION(6) 1.3 -> 30.5; Q4 PARTITION(3) 2.1 -> 39.9, PARTITION(10) 6.9 -> 56.9;
  Q21 PARTITION(30) 7.1 -> 75.2, PARTITION(50) 6.7 -> 71.6.
- Per-call cost is fixed, not bytes: Q12 PARTITION(2) 3.2 ms/task at 627 tasks vs 3.45 ms/task at 145 tasks with 4.3x the
  bytes each. batch4g PARTITION walls: Q4 (3) 39.9 -> 9.4, (10) 56.9 -> 3.1, (17) 34.4 -> 8.6; Q12 (2) 139 -> 43.1, (6) 30.5 -> 3.9;
  Q21 (16) 79.2 -> 4.6, (30) 75.2 -> 3.3, (50) 71.6 -> 3.7 (family PARTITION -456 ms).
- PARTITION excess total (F3 part + this): Q4 121 (84+38), Q12 155 (93+62), Q21 217 (168+49).
- Exposed: PARTITION tasks are created during the scan but prepared after it (Q4 PARTITION(10) created 207 ms, preparing
  393 ms) because of `priority=source` (`src/creator/config.hpp:62-65`, `task_scheduler.cpp:318`).
- Structural confirmation: `x_g4_joinpol` broadcasts Q12's build: Q12 1065 -> 872 (-193); PARTITION(2) 139 -> 5, PARTITION(6)
  30.5 -> 0, CONCAT(7) 46 -> 5, no Bloom accumulation. Q4 (-1) and Q21 (-19) keep partitioned joins.

### F5 Accumulated Bloom finish (design): 92 ms
- Build partitioned across 4 GPUs -> filter accumulated; cross-switch pairs lack pool access -> whole publication HOST-staged
  (`src/op/dynamic_filter/sirius_dynamic_bloom_filter.cu:1080`); staged loop fully serial (`:1171-1207`): per 8 MB chunk and per
  source D2H, wait, H2D, wait, OR, then root -> targets. Runs in the last build-side PARTITION task before a barrier.
- nsys `dynfilter::accum::finish`: Q4 21.1 ms (PARTITION(5) span 22.9), Q12 11.5, Q21 26.9 + 25.9 (rep.3 re-capture: 2 ranges,
  52.3 ms total, cudaStreamSynchronize 48.6 ms = 93%, kernels 0.6 ms, 2.9 GB copied). Logs: 22.1 / 12.0 / 29.1+28.1 ms; g1 direct
  publish 3.7 / 0.6 / 7.0+6.5 ms.
- Batch-invariant: Q4 PARTITION(5) 22.5 -> 22.4, Q21 PARTITION(26) 29.5 -> 29.4, PARTITION(45) 28.3 -> 28.1.
- Price = excess of those pipelines: Q4 22.5, Q12 12.0, Q21 29.5+28.3. Probe scans start after publication, so no extra bytes
  from late filtering here.

### F6 Host-serial outside Quent span: 54 ms
- timings - Quent wall is constant: Q4 16 -> 17, Q12 17 -> 17, Q21 37 -> 38 ms. Price 0.75 x g1 value: 13 + 12 + 29.

### F7 Other (net 10 ms)
- Q21 HASH_JOIN(18): 6 partitions on 4 GPUs, kernel busy 53.7/53.8/26.9/26.9 ms per device, tail ~13 ms; offset by join-mode
  gains (HASH_JOIN(32) 167 -> 17, HASH_JOIN(52) 110 -> 14 ms, BUILD_PROBE only at g4 = C4 confound). x_g1_joinpol (equal
  algorithms) moves g1 Q4 822 -> 778, Q21 3240 -> 3180, Q12 unchanged: the confound is small for this family.

## Candidate remedies (family wall ms at g4; overlapping, do not add)

| Remedy | Targets | Upper bound | Realistic / measured |
|---|---|---|---|
| Coarser scan tasks (`scan_task_batch_size` 4 GB), config | F3, F2a, F4 (1059) | 1059 | **measured -943** (Q4 -242, Q12 -285, Q21 -416); g1 also -519, so equal-config gap 2239 -> 1426 |
| Per-GPU caching sub-allocator over cuda_async (rmm pool/arena per memory_space or thread-local bins); no driver call on hot path | F3 | 476 | ~350 at default batch; ~110 left after batch4g |
| Coalesce fan-in: pre-concat fragments per (source GPU, partition) or one packed transfer + one sync per source; parallel clones | F2a | 434 | ~330 (batch4g already gets ~334 of it) |
| Pipelined staging (double-buffered D2H/H2D chunks), several clones in flight per consumer, switch-aware partition placement | F2b | 292 | ~150 |
| Avoid shuffles g1 does not do: broadcast medium builds (joinpol: Q12 -193 measured), drop the P>=N floor when the build fits one GPU, co-partition lineitem/orders pins by orderkey | F4 + F3(PARTITION) + F2 + F5 | 1312 | Q12 -193 measured; Q4/Q21 need co-partitioning |
| Bloom: hierarchical (P2P within {0,1},{2,3} then one host hop), double-buffered window, async finish off the executor thread | F5 | 92 | 50-90 |
| Overlap probe PARTITION with link-bound scan (`priority=sink`/lookahead) | exposure of F3/F4 | <=422 | small until F3 fixed (allocator near cap during scans, 145k/s) |
| Fewer link bytes (more lineitem/orders compression) or GPU residency | F1 | 728 | gpupin bound: Q12 -302 whole query |
| Do NOT raise `num_threads` | F3 | - | thr8: +261 ms family at g4 |

## Not pursued / not priced
- Bloom array oversizing (Q4 115 MB/GPU vs 57M actual build rows, estimate 272M); sizing rule not checked; could shrink F5 ~4x.
- `fopen` x2 per clone inside convert ranges (942 per Q12 CONCAT task, 3.6 ms) and cuCascade's shared 16-stream pool for clone
  target streams (CRITIC N4): small, folded into F2a.
- GPU clocks/power: no gpu-metrics; kernel time of identical work flat (Q4 DYNAMIC_FILTER kernels 139 -> 137 ms), so excluded.
- Why `x_g4_best` (batch4g+joinpol) is worse than batch4g alone on Q4 (417 vs 381) was not analyzed.
- hpb flips: hpb4g no effect (623/1060/1966); hpb256 hurts Q21 only (+323), consistent with more fragments (F2a/F3).
