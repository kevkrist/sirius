# Family `scanagg`: TPC-H Q1, Q6, Q14, Q19 at 4 GPUs (host-pinned, SF1000)

Revision 2 (after config flips landed).
Every revision-1 pricing was re-checked against `x_g4_batch4g`, `x_g1_batch4g`, `x_g4_gpupin`, `x_g4_thr8`, `x_g1_thr8`,
`x_g4_joinpol`, `x_g4_hpb{256,4g}` and `x_g4_best`. One finding was reclassified (Q14 CONCAT(2), now F4).

Scope: scan-aggregate queries, plus a scan with a small join (Q14 lineitem⋈part, Q19 lineitem⋈part).

Data:
- Un-profiled `00-baseline/{g1_a,g2s_a,g2x_a,g4_a}` (timings + Quent).
- nsys warm iter2 in `01-nsys/nsys2_{g1,g4}`.
- Microbenchmarks under `10-analysis/microbench/`.
- `20-experiments/*`.

Scripts and outputs are in `10-analysis/fam_scanagg/`:
- `pipes.py`: Quent per pipeline and per GPU: span, prepare and compute sums, op thread-ms.
- `apisum.py`: nsys CUDA API totals in the query window.
- `taskalloc.py` → `taskalloc_out.txt`: allocator share of task thread-time.
- `linkline.py`, `devstats.py`: host-link GB/s timeline, per-device totals.
- `opqueue.py`: copy and kernel queue delay.
- `rb.sh` → `rb_{g1,g4}_q*.txt`: `nsys_range_breakdown.py` restricted to the query window.
- Flattened Quent for the flips: `q_x_g{1,4}_batch4g/`, `q_x_g4_gpupin/`, `q_x_g{1,4}_thr8/`, `q_x_g4_joinpol/`, `q_g2s/`, `q_g2x/`.

Conventions:
- **Ideal** = g1 hot mean / 4. **Gap** = g4 hot mean − ideal.
- Thread-time = sum over executor threads (4 per GPU, 16 at g4). In a thread-bound phase, wall ≈ thread-time / (16 × occupancy). Occupancy measured 0.97-0.99 in these scan pipelines.
- Link floor = host bytes / 105 GB/s, the 4-GPU H2D microbench aggregate. Q14 and Q19 plateau at 107-111 GB/s.
- Per-phase accounting uses Quent iter3; it matches hot means within 4%. nsys is profiled: g4 windows are 1.11-1.24x un-profiled, so nsys is used for proportions only.
- **nsys quirk:** at g4, CUPTI labels most pinned-host→device copies `Peer-to-Peer` (srcKind=Pinned). Host→device bytes are identical at g1 and g4 (Q1 45.36 GB, Q6 39.95, Q19 94.8).

## Time accounting

### Speedups (un-profiled hot mean, s)

| q | g1 | g2s {0,1} | g2x {0,2} | g4 | x g2s | x g2x | x g4 | ideal g4 | gap g4 (ms) |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 2.458 | 1.392 | 1.399 | 1.086 | 1.77 | 1.76 | 2.26 | 0.614 | **472** |
| 6 | 1.041 | 0.807 | 0.607 | 0.466 | 1.29 | 1.71 | 2.23 | 0.260 | **206** |
| 14 | 1.487 | 1.254 | 0.907 | 0.848 | 1.19 | 1.64 | 1.75 | 0.372 | **476** |
| 19 | 2.095 | 1.791 | 1.113 | 0.962 | 1.17 | 1.88 | 2.18 | 0.524 | **438** |
| fam | 7.081 | 5.244 | 4.026 | 3.362 | 1.35 | 1.76 | 2.11 | 1.770 | **1592** (14.4% of the 11.08 s suite gap) |

The g2s vs g2x split sorts the family:
- **Link-bound:** Q6, Q14 and Q19 get 1.17-1.29x from a second GPU on the same switch, and 1.64-1.88x across switches.
- **Not link-bound:** Q1 is 1.77x on either pair, then gains only 1.28x from 2 to 4 GPUs. This is the allocator signature (F2): 8 → 16 threads.

### Config flips on this family (un-profiled hot mean, s)

| q | g4 base | batch4g g4 | batch4g g1 | x (batch4g) | gpupin g4 | thr8 g4 | thr8 g1 | joinpol g4 | hpb256 / hpb4g g4 | best g4 |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 1.086 | **0.728** | 2.108 | 2.90 | 0.913 | **1.251** | 2.248 | 1.121 | 1.129 / 1.121 | 0.724 |
| 6 | 0.466 | 0.400 | 0.887 | 2.22 | **0.300** | 0.505 | 1.026 | 0.478 | 0.472 / 0.465 | 0.399 |
| 14 | 0.848 | 0.704 | 1.340 | 1.90 | **0.625** | 0.876 | 1.490 | **0.919** | 0.849 / 0.838 | 0.929 |
| 19 | 0.962 | 0.937 | 1.992 | 2.13 | **0.469** | 0.985 | 2.151 | 0.972 | 0.954 / 0.952 | 0.943 |
| fam | 3.362 | **2.769** (−593) | 6.327 | 2.29 | **2.307** (−1055) | 3.617 (+255) | 6.915 | 3.490 | 3.404 / 3.376 | 2.995 |

- **batch4g** cuts per-task fixed costs. They matter at g1 too (−754 ms), so the family gap to *its own* linear drops 1592 → 1187 ms.
  batch8g g4 is no better: 2.781.
- **gpupin** removes the host link. Q19 falls *below* the base ideal (0.469 < 0.524), and Q6 lands near it (0.300 vs 0.260).
  - Caveat: gpupin is confounded by GPU memory pressure. Q1's group-by partials were downgraded to host (PARTITION origin `HOST`).
  - The 4 family queries were stable across iterations (Q19 .472/.469/.466).
- **thr8** worsens all four at g4, but helps Q1 at g1 (−210 ms).
- **hpb** does not move this family (the exchanges are small).

### Physical regime per query (nsys iter2, query window)

| q | host→dev GB | g1: link GB/s, kernel-busy/GPU | g4: aggregate link GB/s in scan, kernel-busy/GPU | link floor g4 (ms) | regime at g4 |
|---|---|---|---|---|---|
| 1 | 45.4 | 17.5, **0.92** | **42**, 0.44 | 432 | thread-bound: link 40% used, SM 44%, allocator-bound threads |
| 6 | 40.0 | 37, 0.37 | 85 (un-profiled 93), 0.20 | 380 | mostly link (89% of ceiling) |
| 14 | 60.2 (51.2 lineitem + 5.7 part + 3.3 staged shuffle legs) | 45, 0.28 | **109** during lineitem scan; **0** for 290 ms after | 542 | link-bound scan, then a link-idle exchange tail |
| 19 | 94.8 (89.1 lineitem + 5.7 part) | 44, 0.34 | **100-108** for the whole query except the part phase (~60) | 903 | link-bound |

### Per-GPU busy (Quent iter3, ms; busy = some task in flight)

| q | g1 GPU0 | g4 GPU0 / 1 / 2 / 3 | note |
|---|---|---|---|
| 1 | 2450 | 927 / 931 / 944 / **1061** | last task ends 947 / 947 / 947 / **1097**: GPU3 hosts the single MERGE_GROUP_BY |
| 6 | 1030 | 425 / 424 / **451** / 425 | GPU2 hosts MERGE_AGGREGATE (+27 ms) |
| 14 | 1465 | 809 / 814 / 813 / 805 | balanced (327-329 tasks per GPU) |
| 19 | 2072 | 934 / 927 / 916 / 916 | balanced |

No GPU idles. The losses are per-task inflation, plus serial fan-in tails that land on one GPU.

### Per-phase wall (Quent iter3, ms; ideal = g1 phase / 4)

| q | phase | g1 | g4 | ideal | excess | batch4g g4 | owner |
|---|---|---|---|---|---|---|---|
| 1 | scan→HASH_GROUP_BY (627 tasks) | 2427 | 941 | 607 | +334 | 692 (145 tasks) | thread-bound: thread-ms 9666 → 14705 (prep 4160 → 5142, compute 5506 → 9563) |
| 1 | PARTITION(5), pass-through | 9 | 5 | 2 | +3 | 1 | |
| 1 | MERGE_GROUP_BY, 1 task | 15 | **148** (prep 136) | 4 | **+144** | 42 (prep 33) | serial fan-in of 627 partials, 470 remote |
| 6 | scan→UNGROUPED_AGGREGATE | 1028 | 428 | 257 | +171 | 380 | link floor 380; thread-ms 4080 → 6747 |
| 6 | MERGE_AGGREGATE, 1 task | 3 | **23** (prep 21) | 1 | +22 | 6 | serial fan-in of 627 partials |
| 14 | part scan (23 tasks, raw 5.7 GB) | 105 | 56 | 26 | +30 | 55 | link (102 GB/s) |
| 14 | PARTITION(1) | 45 | 12 | 11 | +1 | 16 | |
| 14 | CONCAT(2) build gather | 12 | **73** (prep 60-69/GPU) | 3 | **+70** | **82** (prep 62-80) | build shuffle, byte/staging-bound (F4) |
| 14 | lineitem scan (627 tasks) | 1090 | 470 | 272 | +198 | 461 | link (109-111 GB/s) |
| 14 | PARTITION(4) after scan end | 109 | **94** | 27 | **+67** | **~7** | allocator (F2) |
| 14 | CONCAT(5) probe gather | 86 | **98** (prep 75/GPU) | 21 | **+77** | **27** (prep 22) | fragment fan-in, ~488 fragments per GPU (F3) |
| 14 | HASH_JOIN + merge + result | 21 | 25 | 5 | +18 | 24 | 1 task per GPU |
| 19 | part scan→FILTER + CONCAT(3) | 126 | 79 | 32 | +47 | 81 | link floor 54: +22 link, +25 non-link |
| 19 | lineitem scan→DYNAMIC_FILTER→FILTER | 1932 | 845 | 483 | +362 | 829 | link (floor 848) |
| 19 | PARTITION(7) + CONCAT(8) + HJ + merge tail | 17 | 15 | 4 | +11 | 9 | fixed per-phase latency |

### Operator thread-time, g1 → g4 (Quent iter3, un-profiled; sum ms / median ms per task)

| op | g1 | g4 | ratio | other configs (median per task) |
|---|---|---|---|---|
| Q1 HASH_GROUP_BY(4) | 3046 / 4.84 | 6344 / 10.15 | 2.08x | g2s 5.42, g2x 5.57. **gpupin g4 10.57 (no host link at all).** thr8: g1 8.26, **g4 24.11**. batch4g: g1 13.99 → g4 15.24 (**1.09x**) |
| Q1 GPU_SCAN(0) | 1419 / 2.26 | 2035 / 3.27 | 1.43x | batch4g 1475 → 1482 (1.00x) |
| Q1 prepare (H2D + decode) | 4160 / 6.69 | 5142 / 8.2-8.5 | 1.24x | batch4g 3789 → 5993 (1.58x); gpupin 1778 |
| Q6 UNGROUPED_AGGREGATE(2) | 1206 / 1.73 | 2355 / 3.50 | 1.95x | g2s 2.57, g2x 2.20. gpupin 2.73. batch4g 4.76 → 8.40 (1.76x) |
| Q6 GPU_SCAN(0) | 1016 / 1.38 | 1993 / 2.76 | 1.96x | batch4g 911 → 1670 (1.83x) |
| Q14 PARTITION(4) | 416 / 0.66 | 1479 / 2.42 | 3.6x | batch4g 121 → 137 (1.13x) |
| Q19 GPU_SCAN(4) (link-bound) | 2365 / 2.92 | 4342 / 4.78 | 1.84x | batch4g 2735 → 4185 (1.53x) |

### nsys op ranges (iter2, query window; wall vs launched kernels)

| range | g1 wall / kernels | g4 wall / kernels | allocator ms g1 → g4 | sync ms g1 → g4 |
|---|---|---|---|---|
| Q1 `HASH_GROUP_BY` | 3238 / 903 | 6963 / 857 | 541 → 3834 (**88% of the wall increase**) | 1393 → 1417 |
| Q1 pipeline 0 tasks | 10084 thread-ms | 17368 thread-ms | 1309 (13%) → 10071 (**58%**) | 6328 → 3776 |
| Q6 `UNGROUPED_AGGREGATE` | 1345 / 7.3 | 2413 / 7.3 | +655 (61%) | +251 |
| Q14 `PARTITION` (pipeline 4) | 551 / 16.5 | 1838 / 16.4 | 170 → 1495 (81%) | flat |

The query-level driver allocator (`apisum.py`) runs 80k → 161k calls/s on Q1, and the profiled window sums 10,096 thread-ms of allocator
time over a 1346 ms window. So on average **7.5 of the 16 executor threads are inside `cudaMallocFromPoolAsync`/`cudaFreeAsync`**.
Mean latency per call goes from 9.4/3.4 µs to 45.6/47.8 µs (malloc/free).

## Findings (ranked by family wall ms at g4)

| # | finding | kind | Q1 | Q6 | Q14 | Q19 | family | confidence |
|---|---|---|---|---|---|---|---|---|
| F1 | Host-link ceiling (2 shared Gen5 uplinks), including per-task H2D latency where Q1 is thread-bound | hardware | 61 | 122 | 228 | 384 | **795** | high |
| F2 | Process-global driver allocator (`cudaMallocFromPoolAsync`/`cudaFreeAsync`) saturates at 16 threads, inflating per-task host time | mechanical/design | 254 | 48 | 67 | 0 | **369** | high |
| F3 | Serial cross-GPU fan-in of hundreds of small fragments into one consumer task | design | 144 | 23 | 77 | 12 | **256** | high |
| F4 | Q14 build-side shuffle through host-staged, two-leg, serialized clones | design (+hw) | 0 | 0 | 70 | 0 | **70** | medium-high |
| F5 | Barrier imbalance and a 1-task-per-GPU join tail | design | 19 | 0 | 18 | 0 | **37** | medium |
| F6 | Small-table (part) scan granularity: 23 tasks on 16 threads | design/config | 0 | 0 | 0 | 25 | **25** | medium |
| — | residual | | −6 | 13 | 16 | 17 | 40 | |
| | **sum = gap** | | 472 | 206 | 476 | 438 | **1592** | |

**Experimental cross-check of F2 + F3.** batch4g cuts the per-task allocation count and the fragment count about 4x. The
predicted g4 saving is "F2 + the fragment-dependent part of F3". It matches the measured saving per query:

| q | predicted | measured (g4 base − g4 batch4g) |
|---|---|---|
| Q1 | 254 + 106 = 360 | 358 |
| Q6 | 48 + 17 = 65 | 66 |
| Q14 | 67 + 71 = 138 | 144 |
| Q19 | ~10 | 25 |

### F1. Host-link ceiling: 795 ms (Q19 384, Q14 228, Q6 122, Q1 61)

**Mechanism.**
- All pinned data sits in one host NUMA node. GPUs {0,1} share switch-A's single Gen5 x16 uplink and {2,3} share switch-B's.
- The 4-GPU H2D aggregate is 105 GB/s (26/GPU) vs 52 GB/s for one GPU, so bytes move at most 2.0x faster
  (`10-analysis/microbench/h2d_concurrency/results.txt`).
- Code path:
  - Compressed lineitem: `compressed_representation.cpp:62-86` (`05-codemap/host_scan.md` §1).
  - Raw part (no compression plan): `representation_converter_builtins.cpp:1186-1240`, with flush_submit/reconstruct copies in `cudaMemcpyBatchAsync`.

**Evidence.**
- nsys g4:
  - Q19 runs at 100-108 GB/s in every 25 ms bin from 125 to 925 ms.
  - Q14's lineitem scan runs at 107.5-109.6 GB/s.
  - Q6 averages 85 GB/s profiled, about 93 GB/s un-profiled.
- batch4g g4 (Quent): lineitem phases run at 105-111 GB/s and **do not shrink** (Q19 845 → 829, Q14 470 → 461, Q6 428 → 380 = the floor).
- g2s gives 1.17x (Q19), 1.19x (Q14) and 1.29x (Q6), vs 1.88/1.64/1.71x for g2x.
- **gpupin** (no link): Q19 −493, Q14 −223, Q6 −166, Q1 −173 ms.
  - Q19 and Q14 drop by at least the F1 pricing.
  - Q19 even beats the ideal: g1 itself was link-limited at 44 GB/s.

**Pricing.** Per phase: max(link floor, ideal) − ideal.
- Q19: 848 − 483 = 365, plus part 54 − 32 = 22. Total **384**.
- Q14: lineitem 470 − 272 = 198, plus part 56 − 26 = 30. Total **228**. gpupin −223 agrees.
- Q6: 380 − 258 = **122**.
- Q1: its aggregate floor (432) is below the ideal (614), but Q1 is thread-bound. Each thread uploads, then computes, so H2D latency
  is thread-time.
  - Prep thread-time grows 4160 → 5142 ms (+982). Each GPU's 4 threads share a 26 GB/s link share in bursts (dmon p90 about 30 GB/s per GPU).
  - +982 / 16 threads = **61 ms**.
  - gpupin cuts Q1 prep to 1778 thread-ms. The scan pipeline drops 941 → 730 ms, which bounds this.

**Thread vs wall.** Allocator and other inflation inside link-bound phases (Q19, the Q14 scan, Q6 at batch4g) costs thread-time but
**zero wall**. Example: Q19 GPU_SCAN 1.84x thread-time is fully hidden.

### F2. Driver stream-ordered allocator is a process-wide serialized resource: 369 ms (Q1 254, Q14 67, Q6 48, Q19 0)

**Mechanism.**
- Every device allocation goes through rmm `cuda_async_memory_resource` (`cucascade/src/memory/memory_space.cpp:127`), which calls
  `cudaMallocFromPoolAsync`/`cudaFreeAsync` in the driver.
- The driver serializes these calls process-wide. nsys OSRT shows the time inside them as `pthread_rwlock_wrlock` and `pthread_mutex_lock`
  (Q14 PARTITION range: 715 + 610 ms).
- Microbench (`alloc_contention/results.txt`): aggregate throughput is flat at 279-385k calls/s whatever the GPU count. Mean latency:

  | threads × GPUs | mean latency |
  |---|---|
  | 4 × 1 | 10.9 µs |
  | 4 × 2 | 25 µs |
  | 4 × 4 | **56 µs** |
  | 8 × 4 | **104 µs** |

  Pool peer grants make no difference.
- Allocation count is a property of the task (Q1 ≈ 172 mallocs per 1 GB task, Q6 ≈ 65, Q14 ≈ 74), not of the GPU count. Q1 issues
  216k calls whether on 1 or 4 GPUs. At g4 they must be issued in a quarter of the time (~200k/s during the scan), close to saturation.
  Queueing then grows steeply.
- This is not covered in `05-codemap/sync_contention.md` "Checked and ruled out", which only cleared pool release churn.

**Evidence (four independent discriminators).**
1. **nsys (iter2).** Q1 HASH_GROUP_BY wall 3238 → 6963 ms. The allocator explains 88% of the increase. Kernels are flat (903 → 857); syncs are flat.
2. **gpupin.** Q1 HASH_GROUP_BY median stays at **10.57 ms with no host link at all** (g1 4.84), so the link is ruled out.
3. **thr8.** Q1 HASH_GROUP_BY median **24.11 ms** at g4 (32 threads) vs 8.26 at g1 (8 threads). This tracks the microbench 56 → 104 µs.
   Q1 g4 wall is 1.086 → 1.251 s, while thr8 *helps* g1 (2.458 → 2.248).
4. **batch4g.** With 4x fewer tasks (about 4x fewer calls), Q1 HASH_GROUP_BY g1→g4 inflation falls from 2.08x to **1.09x** (2058 → 2217 thread-ms),
   and GPU_SCAN to 1.00x. Q14 PARTITION(4) falls from 3.6x to 1.13x.

**Pricing (un-profiled, wall).**
- **Q1:** the scan pipeline is thread-bound (14705 thread-ms ≈ 16 threads × 941 ms × 0.98). Compute inflation is +4057 thread-ms,
  which is 254 ms of wall. **254** = 224 allocator (88%) + 30 other driver-API latency (`cudaMemcpyAsync` 4.9 → 9.4 µs,
  `cudaMemcpyBatchAsync`, `cudaStreamWaitEvent` 1.6 → 4.8 µs). The latter is most likely the same driver lock family.
  batch4g measured −249 ms on this phase.
- **Q14:** PARTITION(4) sits on the critical path with the link idle. 94 → 27 ms gives **67**. batch4g measured 94 → 7.
- **Q6:** inflation-free would be 255 ms, but the link floor is 380, so the saving is capped at 428 − 380 = **48**. batch4g measured exactly 380.
- **Q19:** hidden under the link: **0**.

**Ratio caveat.** The allocator also costs g1 11-13% of task time. batch4g speeds up g1 by 754 ms on the family.

**Not explained by F2.** Q6 UNGROUPED_AGGREGATE still inflates 1.76x at batch4g (low allocator rate) and 1.58x under gpupin. A
second per-call coupling exists: candidates are sync H1 (shared small-pinned pool, cross-GPU `cudaStreamWaitEvent` under one mutex,
`small_pinned_host_memory_resource.cpp:166-225`) or SM sharing with concurrent decode. It is hidden under Q6's link floor in every
measured config, so it is **not priced** (0 wall here).

### F3. Serial cross-GPU fan-in of small fragments: 256 ms (Q1 144, Q14 77, Q6 23, Q19 12)

**Mechanism** (`05-codemap/xgpu_movement.md` H1):
- One consumer task drains every upstream fragment:
  - MERGE_GROUP_BY / MERGE_AGGREGATE: one task for the query.
  - CONCAT feeding the BUILD_PROBE join: one task per GPU.
- It clones remote fragments **one at a time** in Preparing: `sirius_physical_operator.cpp:140-190` → `batch_lock_utils.hpp:137` →
  `convert_gpu_to_gpu` (`representation_converter_builtins.cpp:892-960`).
- Each host-staged buffer costs 2-5 blocking syncs (`peer_copy_into`, `:589-697`).
- At g1 the fragments are local and free. Nothing pre-combines per GPU.
- The link and the SMs are idle meanwhile: the Q1 nsys g4 timeline shows 0 GB/s and kernel-busy 0.00 from 1100 to 1346 ms.
- This is also the "Q1 straggler GPU": GPU3 hosts the merge and ends 150 ms after the others in iters 2/3/4 (1056/1097/1067 vs 904/947/918).
  Per-GPU scan ends are within 32 ms of each other.

**Evidence.**
- Q1 MERGE_GROUP_BY prep 134-136 ms in every hot iteration vs 3 ms at g1. nsys shows 470 `convert:gpu_to_gpu` calls (314 host_staging
  at 0.49 ms, 156 peer_copy at 0.19 ms), carrying KBs. This is pure latency.
- Q6 MERGE_AGGREGATE prep 21-22 ms.
- Q14 CONCAT(5) prep 74-76 ms per GPU over ~488 fragments. Staged traffic runs at **4.6 GB/s** aggregate.
- **batch4g scales the cost with fragment count:**
  - Q1 merge 148 → 42 ms (627 → 145 partials).
  - Q6 23 → 6.
  - Q14 CONCAT(5) 98 → 27.
- gpupin Q1 merge is still 104 ms (prep 89).

**Pricing.** Excess over g1/4: Q1 148 − 4 = **144**; Q6 **23**; Q14 CONCAT(5) **77**; Q19 CONCAT(3) about 7 + CONCAT(8) about 5 = **12**.
Achievable about 85% (irreducible bytes ≤ 12-20 ms per phase): **~215**.

### F4. Q14 build-side shuffle through host staging: 70 ms (reclassified from F3 in revision 1)

**Mechanism.**
- At g4 the lineitem⋈part join is BUILD_PROBE with 4 partitions = N, one per GPU. 3/4 of the partitioned part build (4.12 GB total,
  nsys `Pipeline 2: CONCAT` D2D) must move.
- Cross-switch pairs (2/3 of remote) are host-staged: a D2H leg then an H2D leg, serialized by `peer_copy_into`'s syncs (`:589-697`).
  Intra-switch pairs use P2P at about 50 GB/s.
- One CONCAT task per GPU clones serially.

**Evidence.**
- CONCAT(2) prep is 60-69 ms per GPU at base and **62-80 ms at batch4g** (24 instead of 92 fragments). So it is byte/staging-bound, not fragment-bound.
- About 1.05 GB arrives per GPU, so the effective clone rate is 13-17 GB/s per GPU.
- `x_g4_joinpol` (broadcast) makes it worse: CONCAT(2) prep 235-289 ms per GPU (3 GB per GPU at 11-13 GB/s); Q14 0.848 → 0.919 s.
  `x_g4_best` keeps that penalty (0.929).

**Pricing.**
- Excess over g1/4: 73 − 3 = **70**.
- Irreducible floor: cross bytes 2.06 GB each way over 2 uplinks ≈ 20 ms, plus the intra P2P 0.26 GB per GPU ≈ 5 ms, so about 25-30 ms with pipelined legs.
- Achievable about **40 ms**.

### F5. Barrier imbalance and the 1-task-per-GPU join tail: 37 ms

- **Q1:** scan ends range 909.6-941.0 ms across GPUs, and the FULL barrier waits for the last: 941 − mean 925 = **19**.
  At batch4g: 646-692 ms (gpu2 got 37 tasks, the others 36).
- **Q14:** HASH_JOIN runs 1 task per GPU (22 ms on 1 of 4 threads) after CONCAT(5): **18** over ideal. Unchanged at batch4g (22 ms).

### F6. Part-scan phase under-uses the link: 25 ms (Q19)

- 23 part tasks are bound 6/6/6/5 per GPU on 4 threads, which gives 2 waves of 30 ms prep plus 15-16 ms compute.
- GPU_SCAN and FILTER medians go 3.7 → 7.6 ms and 3.9 → 7.7 ms (allocator and mutex time).
- The phase takes 79 ms against a 54 ms link floor for 5.7 GB.
- batch4g (6 tasks, 2/1/1/2 per GPU) gives 73-81 ms, which is no better: imbalance replaces waves.
- Pricing: 79 − 54 = **25**. Q14's part phase has no FILTER and is link-bound: 0.

### Residual (40 ms)

- Q6: 13 ms of start-up and result tail.
- Q14: 16 ms (PARTITION(1) about 2, plus phase-transition latencies).
- Q19: 17 ms (link-phase edges).
- Q1: −6 (iter3 vs hot-mean rounding).

## Candidate remedies (wall ms at g4 on this family)

| remedy | targets | est. saving at g4 | evidence it would work | notes |
|---|---|---|---|---|
| **R0 (config, available now).** `scan_task_batch_size` 4GB | F2 + F3 | **593 measured** (Q1 358, Q14 144, Q6 66, Q19 25) | x_g4_batch4g | Also −754 ms at g1, so family scaling goes 2.11x → 2.29x. Q1 at g4 becomes 0.728 s, prep-thread-bound (prep 1.58x). 8GB is no better (family 2.781). |
| R1. Keep hot-path device allocations off the driver: a per-GPU, per-thread caching layer above `cuda_async_memory_resource` (rmm `arena_memory_resource` per-thread arenas, or a binning/pool layer), or fewer allocations per task (Q1 ≈ 172 per task) | F2 | **~370** at 1 GB batches; about 0-50 beyond R0 | four discriminators in F2 | Lets thread count rise. thr8 currently loses 255 ms on the family and would then add latency hiding (thr8 g1 already helps Q1 −210). |
| R2. Pre-combine before fan-in: a local merge/concat per GPU so the merge sees N inputs, or one `cudf::pack`ed buffer per remote fragment, cloned in parallel or pushed by the producer | F3 | **~215** at 1 GB; ~60 beyond R0 (Q1 merge still 42 ms) | batch4g fragment scaling | Also fixes the broadcast-gather penalty that made `joinpol` lose on Q14. |
| R3. Pipeline the two staging legs, run clones of one CONCAT in parallel (chunked D2H→H2D double buffer), and place partitions switch-aware | F4 | ~40 (Q14) | CONCAT(2) runs at 13-17 GB/s per GPU vs about 50 achievable | Also needed before broadcast joins can win here. |
| R4. Cut host bytes: dictionary-code l_shipmode/l_shipinstruct (Q19), compress part (raw 5.7 GB) | F1 | ≈ 9.5 ms per GB at g4; Q19 l_shipmode about 250 (host_scan H5) | gpupin bounds F1 (Q19 −493) | Speeds up g1 about 2x more, so the ratio moves less. It is the only lever for 734 of F1's 795. |
| R5. Overlap prep with compute inside a thread (prefetch the next batch's H2D while computing), or more threads once R1 lands | F1 (Q1 61) | ~60 (Q1) | gpupin: prep removal = −211 on Q1's scan | Not useful while F2 holds (thr8 regresses). |
| R6. Finer or unbound placement for small host tables (`task_creator.cpp:804-811`) | F6 | ~25 (Q19) | | |

R0 + R2 + R3 is about 593 + 60 + 40 ≈ 690 ms of the 1592 family gap. Most of the rest is F1, which is hardware and bytes.

## Not pursued / not priced

- Q6 UNGROUPED_AGGREGATE and GPU_SCAN still inflate 1.76x / 1.83x at batch4g, and 1.58x under gpupin. This is a second, non-allocator
  per-call coupling (see F2). It is always hidden under the Q6 link floor, so 0 wall here. It may matter for non-link-bound queries:
  hand off to X1/sync H1.
- Q19 lineitem D2D 75.4 GB inside `sirius::compression::host_to_gpu` (decode output, 685-720 GB/s): identical at g1 and g4.
- `fopen` (9-15 ms osrt) inside `convert:gpu_to_gpu` ranges at g4: small, included in F3/F4.
- gpupin per-query numbers for Q1 are confounded: group-by partials were downgraded to host (PARTITION origin HOST, MERGE origin `GPU+HOST`).
- Bloom publication: none of these 4 queries has accumulated-Bloom log lines.
- `hash_partition_bytes` 256MB / 4GB: family ±40 ms, no effect.
- No new GPU runs were made by this analyst. A batch4g + thr8 combination (tests R5 once F2 is relieved by fewer calls) is a cheap follow-up.
