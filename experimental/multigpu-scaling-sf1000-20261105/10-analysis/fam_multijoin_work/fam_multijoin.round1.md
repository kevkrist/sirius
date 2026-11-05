# Family "multijoin": TPC-H Q5, Q7, Q8, Q9 at 1 GPU vs 4 GPUs (SF1000, host-pinned)

Analyst: fam_multijoin. Commit bb557b1a. Read-only; no GPU work run by this analyst (used the queued x_g4_joinpol /
x_g1_joinpol runs). Work files, scripts and raw outputs: `10-analysis/fam_multijoin_work/`
(`pipe_decomp.py`, `critpath.py`, `categorize.py`, `clone_floor.py`, `h2d_timeline.py`, `alloc_in_ranges.py`, `*.txt`).

Units:
- **wall ms**: query wall time. Absolute numbers come from un-profiled runs: `timings.csv` hot mean (iters 2-4), plus Quent
  iters 2-4 for the per-pipeline split.
- **thread-ms**: summed over the 16 executor threads at g4 (4 at g1).
- nsys (iter 2, about 1.25x profiling slowdown at g4) is used only for proportions: API mix, memcpy bytes, link timeline.

## Bottom line

Family gap to linear at g4: 4 605 ms measured vs 1 879 ms ideal, so **2 726 ms** (24.6% of the 11.08 s suite gap).
Each query has the same three-phase shape:

1. a serial build-side chain of small exchanges;
2. one link-bound lineitem scan;
3. a link-idle post-scan tail of forced PARTITION plus cross-GPU CONCAT clones.

At g4 the scans run at the **host-link ceiling (105-109 GB/s)**. **Everything else runs while the link is idle**, and
that "everything else" is new work or new latency that does not exist at 1 GPU.

| Bucket (critical-path excess over g1/4, wall ms) | Q5 | Q7 | Q8 | Q9 | family |
|---|---|---|---|---|---|
| F1 host-link ceiling (scan phases, hardware) | 206 | 225 | 319 | 340 | **1 089** |
| F2 clone overhead (per-buffer sync staging, 627-way fan-in) | 146 | 65 | 179 | 249 | **639** |
| F3 forced N-way PARTITION (deferred, allocator-bound) | 126 | 13 | 147 | 154 | **441** |
| F4 shuffle bandwidth floor (bytes / cross-switch fabric) | 41 | 42 | 26 | 242 | **351** |
| F5 synchronous HOST-staged Bloom publication | 32 | 6 | 50 | 35 | **123** |
| F6 fixed per-query cost outside the Quent window | 20 | 19 | 24 | 22 | **86** |
| join/agg (BUILD_PROBE confound, negative = g4 faster) | 3 | 11 | 2 | -33 | -18 |
| idle gaps between pipelines | 4 | 3 | 5 | 2 | 14 |
| **sum = measured gap** | **578** | **385** | **751** | **1 012** | **2 726** |

Notes on the table:
- F2 and F4 are the two parts of the CONCAT (cross-GPU clone) critical path: 991 ms in total.
- Allocator contention is not a separate row. It is the main mechanism inside F3: about 267 ms of F3 is driver
  `cudaMallocFromPoolAsync`/`cudaFreeAsync` serialization. It is also part of F2.
- Measured remedy: `x_g4_joinpol` (8 GB broadcast + 8 GB build cap) cuts Q5/Q8/Q9 by **-237/-252/-283 ms** but makes
  Q7 **+153 ms** worse. The blanket threshold broadcasts a 4.5 GB build.

## 1. Time accounting

Data sources for the rows below:
- Timings: `00-baseline/*/timings.csv`.
- Thread-time, occupancy and busy: Quent g1_a / g4_a, iters 2-4 (`pipe_q*.txt`).
- Link and bytes: nsys2 iter 2 (`tl_q*_g*.txt`, `nsys_q*_g*.txt`).
- API counts: `api_latency.txt`.

| | Q5 | Q7 | Q8 | Q9 |
|---|---|---|---|---|
| g1 / g2s / g2x / g4 wall (ms) | 1254 / 1193 / 867 / 891 | 1474 / 1247 / 876 / 754 | 1796 / 1705 / 1215 / 1200 | 2991 / 2563 / 1954 / 1760 |
| speedup g2s / g2x / g4 | 1.05 / 1.45 / **1.41** | 1.18 / 1.68 / **1.96** | 1.05 / 1.48 / **1.50** | 1.17 / 1.53 / **1.70** |
| ideal g4 = g1/4, gap | 314, **578** | 369, **385** | 449, **751** | 748, **1012** |
| Quent wall g1 → g4 | 1233 → 869 | 1451 → 730 | 1767 → 1171 | 2963 → 1733 |
| per-GPU busy at g4 (% of wall) | 95-96 | 96-98 | 92-98 | 89-97 |
| thread-time g1 → g4 (s) | 4.73 → 10.57 (2.24x) | 5.62 → 9.50 (1.69x) | 6.75 → 14.66 (2.17x) | 11.51 → 23.11 (2.01x) |
| · HOST prep (H2D + decode) | 3.30 → 5.75 | 3.68 → 6.17 | 5.03 → 8.64 | 6.34 → 10.68 |
| · GPU prep (cross-GPU clone) | 0.01 → 0.63 | 0.01 → 0.35 | 0.01 → 0.69 | 0.02 → 4.50 |
| · compute | 1.42 → 4.19 | 1.93 → 2.98 | 1.72 → 5.33 | 5.15 → 7.93 |
| · of which PARTITION op | 0.01 → 2.15 | 0.21 → 0.36 | 0.01 → 2.47 | 0.67 → 3.06 |
| occupancy (thread / (wall x threads)) | 0.96 → 0.76 | 0.97 → 0.81 | 0.96 → 0.78 | 0.97 → 0.83 |
| wire H2D bytes (GB, same at g1 and g4) | 52.3 | 59.1 | 79.1 | 94.9 |
| link floor at g4 = bytes / 105 GB/s | 498 | 563 | 753 | 904 |
| nsys time with link ≥ 80% of ceiling, g1 / g4 | 1130 / 480 | 1220 / 520 | 1560 / 720 | 1800 / 840 |
| nsys link-idle (< 25% of ceiling) time at g4 | 440 | 140 | 500 | 600 |
| mean kernel-busy per GPU, g1 / g4 | 40 / 14% | 40 / 20% | 34 / 12% | 51 / 20% |
| `partition::execute` thread-ms: wall / launched kernels at g4 | 2542 / 35 | 408 / 28 | 3151 / 46 | 3943 / 160 |
| malloc+free calls g1 → g4; mean malloc µs | 58k → 117k; 9.6 → 32.1 | 79k → 82k; 8.3 → 17.1 | 79k → 152k; 7.2 → 30.9 | 92k → 173k; 6.7 → 33.4 |
| cross-GPU clone payload at g4 (GB): staged + peer | 3.2 + 1.6 | 3.3 + 1.6 | 2.0 + 1.0 | 18.7 + 9.3 |
| accumulated Bloom (ms after the final contribution) | 12.6 + 19.1 | 6.1 | 11.7 + 37.9 | 34.3 |

Phase walls in ms. Quent iters 2-4. The lineitem scan is the 627-task GPU_SCAN pipeline:

| | pre-scan chain g1 → g4 (ideal) | lineitem scan g1 → g4 (ideal) | post-scan tail g1 → g4 (ideal) |
|---|---|---|---|
| Q5 | 235 → 199 (59) | 931 → 398 (233) | 66 → 272 (17) |
| Q8 | 360 → 300 (90) | 1368 → 592 (342) | 39 → 279 (10) |
| Q9 | 532 → 448 (133) | 1603 → 683 (401) | 828 → 602 (207) |

Q7 is ordered differently: lineitem first (1137 → 482), then an orders scan (134 → 59).

- The scan phases speed up 2.3x, which is the uplink ratio of 108/46 GB/s.
- The pre-scan chain barely speeds up at all.
- The tail gets slower in absolute terms for Q5 and Q8.

Why the queries differ:
- **Q7 scales best (1.96x)** because its lineitem join is already broadcast (the build is 0.2 GB, under the 256 MB
  default). Its 627-task PARTITION(26) stays local: 20 thread-ms, and CONCAT(27) has 2 ms of GPU prep. Its shuffles
  happen on smaller intermediates.
- **Q5 and Q8 scale worst** because their lineitem probe is hash-shuffled. The builds are 1.1 GB and 2.0 GB, above the
  256 MB `max_broadcast_join_size`.
- The 2-switch effect is visible at 2 GPUs:
  - g2s (one uplink) gives 1.05-1.18x.
  - g2x (two uplinks) gives 1.45-1.68x.
  - Q5 at g2s is 1193 ms, close to its 1-uplink floor of 52.3 GB / 52 = 1006 ms.

## 2. Findings (ranked by family wall-ms at g4)

### F1 Host-link ceiling during the scans: 1 089 ms (hardware). Confidence high.

**Evidence.** The nsys aggregate pinned-H2D timeline (`h2d_timeline.py`, 10 ms bins, `tl_q*_g4.txt`) sits at 105-109 GB/s
through every large scan:
- Q5: 220-610 ms.
- Q8: 330-910 ms.
- Q9: 490-1150 ms.

That equals the h2d_concurrency microbench ceiling of 105 GB/s for 4 GPUs (2 Gen5 x16 uplinks). At g1 the same phases
run at 42-46 GB/s against a 52 GB/s ceiling. Wire bytes are identical at g1 and g4:

| | Q5 | Q7 | Q8 | Q9 |
|---|---|---|---|---|
| `sirius::compression::host_to_gpu` (GB) | 51.5 | 58.3 | 72.5 | 77.1 |
| `convert:host_to_gpu`, uncompressed tables (GB) | 0.8 | 0.8 | 6.5 | 17.8 |

**Mechanism.** The aggregate H2D bandwidth only doubles from 1 to 4 GPUs: GPUs 0 and 1 share uplink A, GPUs 2 and 3
share uplink B. Scan-side compute inflation is hidden under the link and is not on the critical path:
- DYNAMIC_FILTER thread-time is 1.4-1.6x.
- GPU_SCAN thread-time is 1.5-1.9x.
- The `cudaStreamSynchronize` inside `simpatico::decompress_table` is 4.3 s of thread time in Q5 at g4.

**Pricing.**
- Phase-based: g4 scan-pipeline critical path minus g1 / 4. This gives 206 / 225 / 319 / 340 ms.
- Strict lower bound: max(0, bytes / 105 − g1/4). This gives 184 / 194 / 304 / 156 = **838 ms**.
- The difference of about 250 ms (mostly Q9) is non-link work that, at g1, overlapped with or followed the link in a
  way that scaled. It is recoverable only by overlapping the post-scan work with the scan (see R3).

### F2 Clone overhead in cross-GPU CONCAT fan-in: 639 ms (mechanical). Confidence high.

**Evidence.**
- The CONCAT pipelines' critical path at g4 exceeds g1/4 by 186 / 108 / 205 / 492 ms. Their bandwidth floor (F4) is
  only 41 / 42 / 26 / 242 ms.
- Q8 CONCAT(40) moves **0.1 GB** and still takes 138 ms on the critical path (1019 → 1157 ms; 427 thread-ms of GPU
  prep).
- Q5 CONCAT(31) moves 3.8 GB in 140 ms. Its floor is 25 ms.
- Inside one Q5 P26 CONCAT task (nsys, 181 ms wall) there are about 3 450 `cuStreamSynchronize` (18 µs each),
  4 400 `cudaFreeAsync`, 2 500 `cudaMemcpyAsync`, 1 900 mallocs and 627 `cudaMemcpyPeerAsync`. The copy engines are
  busy for only about 25 ms of it.
- Across Q5 at g4:
  - `convert:gpu_to_gpu:host_staging`: 1 606 calls, 553 thread-ms, 8.2 `cuStreamSynchronize` per call.
  - `peer_copy`: 803 calls, 89 thread-ms.

**Mechanism.**
- Every source fragment is cloned on its own: one producer PARTITION task per scan task gives 627 fragments for each
  destination, three quarters of them remote. The clone path is `batch_lock_utils.hpp:123-140` →
  `convert_gpu_to_gpu` (`cucascade/src/cudf/representation_converter_builtins.cpp:900-947`, ending in
  `target_stream.sync()`).
- Each column buffer without P2P goes through `peer_copy_into` (`:589-697`). That does a chunked D2H, then
  `target_stream.sync()` (`:665`), then a chunked H2D, then `sync()` (`:694`).
- So the cost scales with the number of fragments × columns, not with bytes. At g1 none of this exists: same memory
  space, fast path at `batch_lock_utils.hpp:118-119`.

**Pricing.** The CONCAT critical-path excess minus the F4 floor, computed per pipeline (`clone_floor.py`).

### F3 Forced N-way PARTITION of the probe output, run after the scan and allocator-bound: 441 ms (design + mechanical). Confidence high.

**Evidence.**
- PARTITION after the 627-task lineitem scan costs 9 thread-ms at g1 (pass-through, P=1) and, at g4:
  - Q5: 1 802 thread-ms.
  - Q8: 2 015 thread-ms.
  - Q9: 2 295 thread-ms.
- About 3 ms per task, independent of bytes. Q8 has only 0.1 GB of input.
- Critical path: 110 / 123 / 141 ms (Q5/Q8/Q9) of post-scan tail.
- Every one of these partition tasks is *created* as its scan task finishes:
  - Q5: created at p10/p50/p90 = 244/402/560 ms.
  - The same tasks are only *started* after the scan drains: p10 = 602 ms vs last scan finish at 597 ms. Q8 and Q9 show
    the same pattern (`fam_multijoin_work`, task timing query).
- Inside `sirius_physical_partition::execute` at g4 (nsys):
  - Q5: launched kernels 35 ms of 2 542 thread-ms. `cudaMallocFromPoolAsync` is 20 756 calls at a mean of 66.7 µs;
    `cudaFreeAsync` is 9 292 calls at 68.4 µs. Together they are **79%** of the range. The OSRT rwlock and mutex time
    sits inside these calls.
  - Q8: 81%.
  - Q9: 73%.
  - At g1, Q9's real partitions take 6.7 µs per malloc and 2.6 µs per free.
- The phase wall matches the driver allocator's process-wide throughput. The alloc_contention microbench gives about
  280-305 k calls/s at 16 threads on 4 GPUs. Q5 makes 30.0 k calls, which takes 100 ms at that rate; the measured phase
  is 110 ms. Q8: 35.0 k → 117 vs 123 ms. Q9: 42.7 k → 142 vs 141 ms.

**Mechanism.**
- (a) `partition_min_num_partitions(N)` (`src/op/partition/partition_sizing.hpp:86,115`) and BUILD_PROBE `min(natural, N)`
  force P=4. The same exchange is a pass-through at N=1.
- (b) Task priority `priority_order::source` (`src/creator/config.hpp:106`) assigns lower values upstream
  (`task_creator.cpp:217-238`). The per-device pop takes the lowest value first (`task_scheduler.cpp:318`). So every
  GPU drains its scan backlog before it runs any PARTITION task, and the allocator-bound partition work runs while the
  link is idle instead of hiding under the link-bound scan.
- (c) Each partition task makes 25-37 stream-ordered driver allocations, against about 9 at g1. All 16 threads hit the
  process-global driver pool lock at once.

**Pricing.**
- Total F3: the PARTITION pipelines' critical-path excess (`categorize.py`). It splits into the post-scan instance
  (385 ms: Q5 110, Q7 11, Q8 123, Q9 141) and the small build-side exchanges (56 ms).
- Allocator share, as a counterfactual at g1 per-call latency: Q5 −80, Q8 −92, Q9 −94, Q7 about −5, so **about
  −270 ms**. This is a mechanism split, not an addition to F3.

### F4 Shuffle bandwidth floor: 351 ms (design + hardware topology). Confidence medium.

**Evidence.**
- Q9 shuffles 15.0 GB (partsupp, CONCAT(2)), 9.1 GB (CONCAT(28)) and 11.4 GB (CONCAT(31)).
- Clone payload at g4 is 28 GB: 18.7 GB host-staged, 9.3 GB peer.
- At g1 none of it moves.

**Mechanism.**
- Hash shuffles move (N−1)/N of the bytes.
- Pin placement is round-robin, not co-partitioned (`task_creator.cpp:793-822`), so no join is local.
- Two thirds of the remote bytes cross switches, where there is no P2P. Staged cross-switch transfers reach 77 GB/s
  aggregate for 4 pairs (p2p microbench).

**Pricing.** Remote bytes × 2/3 / 77 GB/s for each CONCAT pipeline that moved data (GPU prep > 5 ms): 8.7 ms per GB of
remote bytes. Lower bound for any shuffle-based plan on this topology.

### F5 Synchronous, HOST-staged accumulated Bloom publication on the build-side barrier: 123 ms (design). Confidence high.

**Evidence.**
- g4_a iter3 log lines, timed against Quent:
  - Q5 Bloom 2 "started" at +165 ms, published at +184 ms (19.1 ms).
  - Q5 PARTITION(26): three tasks finish at 168.5 ms; the fourth, which owes the publication, finishes at **184.9 ms**.
  - CONCAT(27) is created at 184.9 ms.
  - The lineitem scan is created at 199.6 ms.
- Same pattern elsewhere:
  - Q8: 240.0 vs 272.7 ms (37.9 ms publication).
  - Q9: 406.9 vs 438.1 ms (34.3 ms).
  - Q5 Bloom 1: +10.9 ms.
  - Q7: 6.1 ms.
- g1 has no accumulated Bloom for these queries: the single-partition build publishes directly.
- Under x_g4_joinpol the builds broadcast, and these Bloom lines disappear for Q5/Q8/Q9.

**Mechanism.**
- `publish()` "runs on the task that owes the publication" (`src/op/dynamic_filter/dynamic_filter_publisher.cpp:381-432`),
  so the reduction and fan-out sit inside that PARTITION task.
- The downstream CONCAT and the probe scan wait behind the pipeline barrier.
- Transport is all-or-nothing: any pair without pool access means HOST staging
  (`src/cuda/sirius_dynamic_bloom_filter.cu:1080`), and every 4-GPU set has 8 such pairs.

**Pricing.** Critical-path excess of the Bloom-owning PARTITION pipelines (Q5 P16 + P22, Q7 P7, Q8 P25 + P31,
Q9 P22). About 3 ms per pipeline of that is real partition work.

### F6 Fixed per-query overhead outside the Quent query window: 86 ms (mechanism unknown). Confidence medium.

**Evidence.** `timings.csv` minus Quent wall is 23-31 ms per query, at both g1 and g4: 23/26, 25/26, 31/32 and 30/29 ms.
This is serial work that does not scale, such as planning and result handoff.

**Pricing.** out_g4 − out_g1 / 4. Not attributed further. No nsys covers it, because the capture range is the query.

### Context: join-mode confound (−18 ms net)

- Q9 HASH_JOIN(29) critical path: 215 ms at g1 → 26 ms at g4. HASH_JOIN(32) + GROUP_BY: 323 → 68 ms (BUILD_PROBE at
  g4 vs STANDARD at g1, CRITIC C4).
- This flatters g4. It is listed so the bucket sum closes, not as a bottleneck.

## 3. Measured remedy: join policy (x_g4_joinpol, x_g1_joinpol)

The change is `max_broadcast_join_size` 8 GB, plus `max_build_hash_table_bytes` set to its default (8 GB).

| | g4_a | x_g4_joinpol | Δ | what changed on the critical path (`critjp_q*.txt`) |
|---|---|---|---|---|
| Q5 | 891 | 654 | **−237** | Removed: PARTITION(30) 110, CONCAT(31) 137→11, the orders-level exchange 38 and both Blooms 32. Added: broadcast CONCAT(27) 54 and CONCAT(20) 33. |
| Q7 | 754 | 907 | **+153** | CONCAT(30) 58 → **239**: 4.5 GB broadcast to 3 GPUs (13.5 GB cross-GPU vs 3.4 GB shuffled before). |
| Q8 | 1200 | 948 | **−252** | Removed: PARTITION(39) 123, CONCAT(40) 136→10, Blooms 50. Added: broadcast CONCAT(36) 2 GB → 120. |
| Q9 | 1760 | 1477 | **−283** | Removed: PARTITION(27) 141, CONCAT(28) 155→35, Bloom 35. Added: CONCAT(24) 98, CONCAT(14) 71, CONCAT(2) 136→187. |
| family | 4605 | 3986 | **−619** | With per-join selection (keep Q7 shuffled): **≈ −772** |

x_g1_joinpol (honest denominator): see the addendum at the end. It is filled in when the run lands.

**Reading.**
- Broadcasting medium builds removes F3, F5 and most of F2 for the lineitem probes. The broadcast copy itself is slow:
  2 GB in 120 ms, about 50 GB/s.
- A size-only threshold is wrong at N=4 on this fabric. The policy must compare:
  - broadcast cost: build × (N−1) bytes;
  - shuffle cost: (build + probe) × (N−1)/N bytes, **plus** a per-fragment fan-in cost of about 0.2-0.3 ms per remote
    fragment (F2) and the forced partition (F3).
- The code site is `sirius_physical_hash_join.cpp:867-899`.

## 4. Candidate remedies (family wall-ms at g4; alternatives are not additive)

| # | Remedy | Hits | Est. saving (family) | Basis |
|---|---|---|---|---|
| R1 | Cost-based broadcast vs shuffle per join (bytes + fragment count + N + topology), not a fixed 256 MB / 8 GB threshold | F2, F3, F5 (and F4 for Q5/Q8) | **−772** (measured −619 with a blanket 8 GB; avoiding the Q7 regression adds +153) | x_g4_joinpol |
| R2 | Batched cross-GPU clone: one staged transfer per (src, dst, task), with no per-buffer `sync()`, packed partition outputs, and producer-side coalescing of the 627 fragments | F2 | up to −639; realistic about −400 (60%) | F2 overhead split |
| R3 | Consumer-first scheduling (`priority_order::sink`, or lookahead) so PARTITION and CONCAT overlap the link-bound scan | F3 post-scan, part of F2, and the F1 phase/byte difference | up to −385 (F3 post-scan); depends on spare threads during the scan (x_g4_thr8 tests this) | Quent create-vs-start timing |
| R4 | Fewer and cheaper allocations in partition: contiguous per-partition buffers instead of per-column slice deep copies (`gpu_partition_impl.cpp:213-221`), or a per-thread user-space pool over the driver pool | F3 (and the free storms in F2) | **≈ −270** (F3 only) | allocator counterfactual |
| R5 | Bloom: publish asynchronously, off the barrier task, and hierarchically (peer within {0,1} and {2,3}, then one host leg) | F5 | −85 (hierarchical: 20 → 5.8 ms mean, g2s peer) to −123 (asynchronous) | log latencies |
| R6 | Co-partitioned pinning on join keys (partsupp/lineitem on partkey; orders/lineitem on orderkey) with switch-aware placement | F4, F2 | up to −351 floor, plus the F2 share for Q9 | F4 |
| R7 | Bytes on the link: compress the uncompressed pinned tables (part/partsupp/supplier/customer; Q9 17.8 GB and Q8 6.5 GB through `convert:host_to_gpu`) | F1 | about −100 (Q9) and −37 (Q8) at the lineitem ratio of 2.5x; the g1 gain is 2x larger, so the scaling ratio barely moves | nsys bytes |
| R8 | GPU-resident tables (x_g4_gpupin) | F1 | bound −838 to −1 089 | pending experiment |

## 5. Not pursued / not priced

- **Per-op compute inflation on scan-side ops** (CRITIC N1): DYNAMIC_FILTER 1.41-1.58x and GPU_SCAN 1.5-1.95x
  thread-time at g4. **0 wall ms in this family**, because it runs under the saturated link. It will surface once F1
  is removed (R8).
- Small-pinned pool mutex (sync H1) and the stream-pool sharing in `convert_gpu_to_gpu` (`acquire_stream`, `:926`,
  N4): not separated from F2 and F3.
- `fopen` inside clone ranges: about 22 thread-ms in Q5, 5.8 ms per Q8 CONCAT task. A curiosity, possibly driver probing.
- Q7 "accumulated Bloom refused: per-GPU cap" happens at both g1 and g4, so it adds no scaling cost.
- 2-GPU rows (g2s ≈ 1.05-1.18x, uplink-bound): out of scope (M11).
- Extra nsys3_g4 captures (Q5/Q8): no reports present when analysed.
- Other config flips (hpb256, hpb4g, batch4g, thr8, gpupin): not present in `20-experiments/` at write time.

## Addendum: x_g1_joinpol (landed 17:03)

| | g1_a | x_g1_joinpol | g4 joinpol speedup vs g1 joinpol |
|---|---|---|---|
| Q5 | 1254 | 1260 | 1.93x (was 1.41x) |
| Q7 | 1474 | 1436 | 1.58x (was 1.96x) |
| Q8 | 1796 | 1799 | 1.90x (was 1.50x) |
| Q9 | 2991 | 2991 | 2.03x (was 1.70x) |

The g1 denominator is unchanged for this family (Q7 changes by −38 ms; the rest are within noise), so the join-mode
confound does not bite here.

- Family gap under joinpol: 3986 − 7486/4 = **2 114 ms**, down from 2 726, so joinpol closes **612 ms**.
- With per-join selection that keeps Q7 shuffled, the gap closes by about **765 ms**: Q7 then contributes 754 − 1436/4.
