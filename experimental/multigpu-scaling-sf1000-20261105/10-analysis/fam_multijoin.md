# Family "multijoin": TPC-H Q5, Q7, Q8, Q9 at 1 GPU vs 4 GPUs (SF1000, host-pinned)

Analyst: fam_multijoin (round 2; round-1 report kept at `fam_multijoin_work/fam_multijoin.round1.md`). Commit bb557b1a.
Read-only; no GPU work run by this analyst. All scripts and raw outputs: `10-analysis/fam_multijoin_work/`.

Units: **wall ms** = query wall (absolute numbers from un-profiled `timings.csv`, hot mean iters 2-4; Quent iters 2-4 for
per-pipeline splits). **thread-ms** = summed over executor threads (16 at g4, 4 at g1). nsys (iter 2, ~1.25x slowdown at g4)
is used only for proportions/API mix. Wall pricing method: a 0.1 ms sweep assigns each instant of the query to the most
upstream pipeline with a task in flight (`critpath.py`); per pipeline, *excess* = g4 critical-path ms − g1 critical-path
ms / 4; excesses are bucketed by mechanism (`categorize2.py`) and sum exactly to the measured gap (timings minus g1/4).

## Bottom line

Family gap to linear at g4: 4 605 ms measured vs 1 879 ideal = **2 726 ms (24.6 % of the 11.08 s suite gap)**.
Every query has the same shape: (1) a serial build-side chain of small exchanges, (2) one link-bound lineitem scan at the
**host-link ceiling (105-109 GB/s)**, (3) a link-idle post-scan tail of **forced 4-way PARTITION + cross-GPU CONCAT
clones**. Phase (3) is ~0 at 1 GPU and costs 252 / 147 / 266 / 393 ms of critical path at 4 GPUs.

| Bucket (wall ms at g4, critical-path excess over g1/4) | kind | Q5 | Q7 | Q8 | Q9 | family |
|---|---|---|---|---|---|---|
| F1 host-link ceiling during scans (2 uplinks for 4 GPUs) | hardware | 206 | 225 | 319 | 340 | **1 089** |
| F2 clone overhead (per-fragment, per-buffer synchronous staging) | mechanical | 146 | 65 | 179 | 249 | **639** |
| F3 forced N-way PARTITION, deferred + allocator-bound | design + mechanical | 126 | 13 | 147 | 154 | **441** |
| F4 shuffle bandwidth floor (bytes over cross-switch fabric) | design + hardware | 41 | 42 | 26 | 242 | **351** |
| F5 synchronous HOST-staged accumulated Bloom publication | design | 32 | 6 | 50 | 35 | **123** |
| F6 fixed per-query cost outside the Quent window | unknown | 20 | 19 | 24 | 22 | **86** |
| join/agg (BUILD_PROBE confound; negative = g4 faster) | measurement | 3 | 11 | 2 | -33 | -18 |
| idle gaps between pipelines | — | 4 | 3 | 5 | 2 | 14 |
| **sum = measured gap** | | **578** | **385** | **751** | **1 012** | **2 726** |

**Root cause shared by F2 + F3 (new, measured):** both scale with the *fragment count* (627 scan tasks at the 1 GB
`scan_task_batch_size` → 627 partition tasks → 627 fragments per destination). `x_g4_batch4g` (145 scan tasks) leaves the
link-bound scan phase unchanged (Q5 398→394 ms) but cuts F3 441→**28** and F2 639→**276**: family g4 wall **−729 ms**
(4 605→3 876), and −646 ms of gap against an equally-configured g1 (2 726→2 080). It is the largest single measured lever
for this family, bigger than the join policy (−619).

## 1. Time accounting

Sources: timings `00-baseline/*/timings.csv`; Quent g1_a/g4_a iters 2-4 (`pipe_q*.txt`, `crit_q*.txt`); nsys2 iter 2
(`tl_q*_g*.txt`, `nsys_q*_g*.txt`, `nsys_summary.txt`); API counts `api_latency.txt`, `alloc_partition*.txt`.

| | Q5 | Q7 | Q8 | Q9 |
|---|---|---|---|---|
| g1 / g2s / g2x / g4 wall (ms) | 1254 / 1193 / 867 / 891 | 1474 / 1247 / 876 / 754 | 1796 / 1705 / 1215 / 1200 | 2991 / 2563 / 1954 / 1760 |
| speedup g2s / g2x / g4 | 1.05 / 1.45 / **1.41** | 1.18 / 1.68 / **1.96** | 1.05 / 1.48 / **1.50** | 1.17 / 1.53 / **1.70** |
| ideal g4 = g1/4; gap | 314; **578** | 369; **385** | 449; **751** | 748; **1012** |
| Quent wall g1 → g4 | 1233 → 869 | 1451 → 730 | 1767 → 1171 | 2963 → 1733 |
| per-GPU busy at g4 (% of wall) | 95-96 | 96-98 | 92-98 | 89-97 |
| thread-time g1 → g4 (s) | 4.73 → 10.57 (2.24x) | 5.62 → 9.50 (1.69x) | 6.75 → 14.66 (2.17x) | 11.51 → 23.11 (2.01x) |
| · HOST prep (H2D + decode) | 3.30 → 5.75 | 3.68 → 6.17 | 5.03 → 8.64 | 6.34 → 10.68 |
| · GPU prep (cross-GPU clone) | 0.01 → 0.63 | 0.01 → 0.35 | 0.01 → 0.69 | 0.02 → 4.50 |
| · compute | 1.42 → 4.19 | 1.93 → 2.98 | 1.72 → 5.33 | 5.15 → 7.93 |
| · of which PARTITION op | 0.01 → 2.15 | 0.21 → 0.36 | 0.01 → 2.47 | 0.67 → 3.06 |
| occupancy thread/(wall x threads) | 0.96 → 0.76 | 0.97 → 0.81 | 0.96 → 0.78 | 0.97 → 0.83 |
| wire H2D bytes (GB, equal g1/g4) | 52.3 | 59.1 | 79.1 | 94.9 |
| link floor g4 = bytes / 105 GB/s (ms) | 498 | 563 | 753 | 904 |
| nsys time with H2D ≥ 80 % of ceiling, g1 / g4 (ms) | 1130 / 480 | 1220 / 520 | 1560 / 720 | 1800 / 840 |
| nsys link-idle time (< 25 % of ceiling) at g4 (ms) | 440 | 140 | 500 | 600 |
| mean kernel-busy per GPU g1 / g4 | 40 / 14 % | 40 / 20 % | 34 / 12 % | 51 / 20 % |

Phase split of the critical path (ms; g1 → g4, excess over g1/4; `phase_split.py` → `phase_split.txt`):

| | pre-scan build chain | lineitem scan | post-scan tail (PARTITION + CONCAT + final join) |
|---|---|---|---|
| Q5 | 232 → 194 (+136: PART 49, CONCAT 49, small scans 41) | 931 → 398 (+165) | 65 → 268 (+252: PART 110, CONCAT 137) |
| Q7 | 18 → 27 (+22) | 1136 → 478 (+194) | 291 → 219 (+147: CONCAT 98, orders scan 26) |
| Q8 | 356 → 295 (+207: PART 72, CONCAT 67, scans 69) | 1368 → 592 (+250) | 36 → 275 (+266: PART 124, CONCAT 138) |
| Q9 | 528 → 444 (+312: CONCAT 202 incl. 15 GB partsupp shuffle) | 1603 → 683 (+282) | 827 → 600 (+393: PART 140, CONCAT 290, HASH_JOIN −41) |

NVTX op ranges, thread-ms inside the query window (nsys2 iter 2, `nsys_summary.txt`), wall vs launched-kernel ms:

| range (g1 → g4) | Q5 | Q7 | Q8 | Q9 |
|---|---|---|---|---|
| `sirius::compression::host_to_gpu` wall (kernels flat ~250-400) | 3432 → 5691 | 3821 → 6104 | 4771 → 7905 | 5504 → 8948 |
| `sirius_physical_partition::execute` wall / kernels at g4 | 1 → **2542 / 35** | 223 → 408 / 28 | 1 → **3151 / 46** | 627 → **3943 / 160** |
| `sirius_physical_dynamic_filter::execute` wall (kernels flat) | 624 → 953 | 43 → 57 | 1076 → 1644 | 995 → 1506 |
| `convert:gpu_to_gpu` (clones) n / wall at g4 | 2409 / 671 | 540 / 338 | 2493 / 754 | 3954 / 4632 |
| · host_staging n / wall / GB | 1606 / 553 / 6.4 | 360 / 287 / 6.6 | 1662 / 616 / 3.9 | 2637 / 3946 / 37.4 |
| · peer_copy n / wall / GB | 803 / 89 / 1.6 | 180 / 44 / 1.6 | 831 / 107 / 1.0 | 1317 / 610 / 9.3 |
| CONCAT task ranges n / wall | 16/64 → 37/943 | 21/127 → 38/473 | 15/44 → 51/1074 | 53/609 → 74/5936 |
| `hash_join::execute` wall (BUILD_PROBE at g4) | 132 → 55 | 175 → 72 | 53 → 35 | 1293 → 951 |
| malloc+free calls (query) g1 → g4; mean malloc µs | 58k → 117k; 9.6 → 32.1 | 79k → 82k; 8.3 → 17.1 | 79k → 152k; 7.2 → 30.9 | 92k → 173k; 6.7 → 33.4 |
| accumulated Bloom latency at g4 (ms after final contribution) | 12.6 + 19.1 | 6.1 | 11.7 + 37.9 | 34.3 |

Why the four differ: Q7's lineitem join is already broadcast (0.2 GB build < 256 MB `max_broadcast_join_size`), so its
627-task PARTITION(26) stays local (20 thread-ms) and its post-scan tail moves small intermediates → best scaling (1.96x).
Q5/Q8 hash-shuffle the lineitem probe (builds 1.1 / 2.0 GB) → forced partition + 627-way clone fan-in after the scan →
worst (1.41/1.50x). Q9 additionally shuffles 35 GB (partsupp 15.0, CONCAT(28) 9.1, CONCAT(31) 11.4 GB). g2s (one uplink)
gives 1.05-1.18x; Q5 at g2s = 1193 ms vs its 1-uplink floor 52.3 GB / 52 GB/s = 1006 ms.

## 2. Config-flip results for the family (un-profiled, hot mean, ms)

| run | Q5 | Q7 | Q8 | Q9 | family | Δ g4 vs g4_a | gap vs same-config g1/4 |
|---|---|---|---|---|---|---|---|
| g1_a | 1254 | 1474 | 1796 | 2991 | 7515 | | |
| g4_a (baseline) | 891 | 754 | 1200 | 1760 | 4605 | 0 | 2726 |
| x_g4_batch4g (scan_task_batch_size 4 GB) | 678 | 712 | 941 | 1545 | 3876 | **−729** | **2080** (g1_b4g 7182) |
| x_g4_batch8g | 662 | 723 | 929 | 1624 | 3938 | −667 | — |
| x_g4_joinpol (8 GB broadcast + 8 GB build cap) | 654 | **907** | 948 | 1477 | 3986 | −619 | 2114 (g1_jp 7486) |
| x_g4_best (batch4g + joinpol) | 649 | **873** | 951 | 1493 | 3966 | −639 | 2011 (g1_best 7165) |
| per-query best-of (b4g for Q7, best for Q5/Q9, b4g for Q8) | 649 | 712 | 941 | 1493 | 3795 | **−810** | — |
| x_g4_thr8 (8 threads/GPU) | 933 | 763 | 1251 | 1833 | 4780 | **+175** | (g1_thr8 7601) |
| x_g4_hpb256 / x_g4_hpb4g | 896 / 887 | 751 / 746 | 1199 / 1204 | 1888 / 1869 | 4734 / 4706 | +129 / +101 (all Q9) | — |
| x_g4_gpupin (no host link; memory-pressure confound) | 766 | 600 | 766 | 1619 | 3751 | −854 | — |

Iteration spread: batch4g/best/thr8 per-query sd ≤ 20 ms; gpupin Q5 729-831, Q9 1576-1689 (unstable, 42 790 downgrade log lines).

Where each flip lands (critical-path ms per pipeline, `cmp_crit_g4_vs_b4g.txt`, `crit_gpin_q*.txt`, `part_timing.py`):

| | lineitem scan | post-scan PARTITION | post-scan CONCAT | per-task partition compute | concat GPU-prep thread-ms |
|---|---|---|---|---|---|
| Q5 g4_a → batch4g | 398 → 394 | 111 → 7 | 141 → 62 | 2.98 → 0.99 ms (627 → 145 tasks) | 448 → 216 |
| Q8 g4_a → batch4g | 592 → 586 | 125 → 3 | 138 → 33 | 3.14 → 1.38 ms | 427 → 102 |
| Q9 g4_a → batch4g | 683 → 681 | 143 → 8 | 169 → 88 | 3.55 → 1.83 ms | 1208 → 668 |
| Q5/Q8/Q9 g4_a → thr8 | ≈ (+0-25) | phase 111→137 / 123→157 / 147→180 | ≈ | **2.98→7.12 / 3.14→8.01 / 3.55→8.70 ms** | ≈ |
| Q5/Q7/Q8/Q9 g4_a → gpupin | 398→113 / 478→160 / 592→171 / 683→166 | 111→171 / — / 125→190 / 143→265 | ≈ | | |

Bucketing the batch4g pair (g1_b4g vs g4_b4g, same method): LINK 1206, CLONE 628 (floor 352 + overhead 276), FPART 28,
BLOOM 122, OUTSIDE 85, rest 12. So batch4g removes F3 and 57 % of F2 and touches nothing else; joinpol removes F3+F5+most
of F2 for Q5/Q8/Q9 but adds broadcast copies; once batch4g is in, joinpol adds only −29 (Q5) / +10 (Q8) / −52 (Q9) and
still regresses Q7 (+161). The two levers attack the same costs and are **not additive**.

## 3. Findings (ranked by family wall ms at g4, baseline config)

### F1 Host-link ceiling during the scans — 1 089 ms (hardware). Confidence high.

**Evidence.** nsys aggregate pinned-H2D (10 ms bins, `h2d_timeline.py`, `tl_q*_g4.txt`) sits at 105-109 GB/s through every
large scan (Q5 220-610 ms, Q8 330-910, Q9 490-1150) = the h2d_concurrency ceiling for 4 GPUs (2 Gen5 x16 uplinks; 105 GB/s).
At g1 the same phases run at 42-46 GB/s (ceiling 52). Wire bytes identical at g1/g4 (`sirius::compression::host_to_gpu`
51.5 / 58.3 / 72.5 / 77.1 GB, plus uncompressed `convert:host_to_gpu` 0.8 / 0.8 / 6.5 / 17.8 GB). Lineitem scan phase speeds
up 2.3x (931→398, 1368→592, 1603→683) = 108/46 GB/s. batch4g leaves it unchanged (398→394, 592→586, 683→681): the phase is
not software-bound. gpupin (no link) shrinks it to 113 / 160 / 171 / 166 ms (Q5/Q7/Q8/Q9), i.e. **−285 / −318 / −421 / −517 ms**.
Scan-side compute inflation exists but is hidden: `dynamic_filter::execute` 1.4-1.5x thread-ms with flat kernels (Q5: 737 of
953 ms is `cudaStreamSynchronize`), `decompress_table` `cudaStreamSynchronize` 4.3 s thread-ms in Q5.

**Mechanism.** GPUs 0+1 share uplink A, 2+3 share uplink B; aggregate H2D only doubles 1→4 GPUs.

**Pricing.** Scan-pipeline critical-path excess (g4 − g1/4): 206 / 225 / 319 / 340. Strict lower bound max(0, bytes/105 −
g1/4) = 184 / 194 / 304 / 156 = 838 ms. Upper bound from gpupin scan-phase drop: 1 541 ms, but gpupin also grows the tail
(see F3), so query-level gpupin savings are only −125 / −154 / −434 / −141.

### F2 Clone overhead in cross-GPU CONCAT fan-in — 639 ms (mechanical; root: fragment count). Confidence high.

**Evidence.** CONCAT critical-path excess 186 / 108 / 205 / 492; bandwidth floor (F4) only 41 / 42 / 26 / 242. Q8 CONCAT(40)
moves **0.1 GB** yet holds the critical path 138 ms (427 thread-ms GPU prep). Q5 `host_staging`: 1 606 calls, 553 thread-ms,
`cuStreamSynchronize` 318 ms of it, copy engines busy ~35 ms; `peer_copy` 803 calls, 89 ms. One Q5 CONCAT task (181 ms)
issues ~3 450 `cuStreamSynchronize`, 4 400 `cudaFreeAsync`, 627 `cudaMemcpyPeerAsync`. **batch4g (4.3x fewer fragments)
cuts the overhead 639 → 276 ms** (Q5 CONCAT 141→62, Q8 138→33, Q9 169→88; GPU-prep thread-ms 448→216, 427→102, 1208→668)
while bytes are unchanged.

**Mechanism.** Every source fragment is cloned separately: `batch_lock_utils.hpp:123-140` → `convert_gpu_to_gpu`
(`cucascade/src/cudf/representation_converter_builtins.cpp:900-947`, ends in `target_stream.sync()`). Each column buffer
without P2P goes through `peer_copy_into` (`:589-697`): chunked D2H, `sync()` (`:665`), chunked H2D, `sync()` (`:694`). Cost
∝ fragments × columns, not bytes. At g1 the same-space fast path (`batch_lock_utils.hpp:118-119`) skips all of it.

**Pricing.** Per-pipeline CONCAT excess minus F4 floor (`clone_floor.py`): 146 / 65 / 179 / 249. Measured recoverable by
fragment coarsening alone: 363 ms (batch4g).

### F3 Forced N-way PARTITION of the probe output, deferred behind the scan and allocator-bound — 441 ms (design + mechanical). Confidence high.

**Evidence.** Post-scan PARTITION: 9 thread-ms at g1 (P=1 pass-through) vs 1 802 / 2 015 / 2 295 thread-ms at g4 (Q5/Q8/Q9),
~3 ms per task independent of bytes (Q8 input 0.1 GB). Tasks are *created* during the scan (Q5 p10/p50/p90 = 244/402/560 ms)
but *started* after it drains (p10 602 ms vs last scan finish 597 ms; Q8 887 vs 892; Q9 1128 vs 1136). nsys
`partition::execute` at g4: kernels 35 of 2 542 thread-ms (Q5); `cudaMallocFromPoolAsync` 20 756 calls × 66.7 µs +
`cudaFreeAsync` 9 292 × 68.4 µs = **79 %** of the range (Q8 81 %, Q9 73 %); reproduced in nsys3_g4 (same counts, 78-79 %,
58-61 µs means; `alloc_partition_nsys3.txt`). g1 per-call latency 6.7 µs malloc / 2.6 µs free. alloc_contention microbench:
4 GPUs × 4 threads → 56 µs mean, 279-305 k calls/s process-wide; 1 GPU × 4 → 10.9 µs. Q5 makes 30.0 k calls in the phase →
100 ms at 300 k/s vs 110 measured (Q8 117 vs 123, Q9 142 vs 141). **thr8 confirms a serialized resource:** doubling threads
raises per-task partition compute 2.98→7.12 / 3.14→8.01 / 3.55→8.70 ms and the phase wall 111→137 / 123→157 / 147→180 ms
(family +175 ms). **batch4g removes it:** 145 tasks at 1.0-1.8 ms, phase 7 / 3 / 8 ms, most tasks start during the scan tail.
gpupin makes it worse (171 / 190 / 265 ms): without the link, nothing hides it.

**Mechanism.** (a) `partition_min_num_partitions(N)` (`src/op/partition/partition_sizing.hpp:86,115`) and BUILD_PROBE
`min(natural, N)` (`sirius_physical_hash_join.cpp:873,895`) force P=4 where N=1 is a pass-through (`sirius_physical_partition.cpp:157`).
(b) `priority_order::source` (`src/creator/config.hpp:106`; values `task_creator.cpp:217-238`; lowest-first pop
`task_scheduler.cpp:318`) drains the scan backlog before any PARTITION task runs, so the allocator-bound work lands in the
link-idle window. (c) 25-37 stream-ordered driver allocations per partition task (per-column slice deep copies,
`gpu_partition_impl.cpp:213-221`), all 16 threads on the process-wide driver pool.

**Pricing.** PARTITION-pipeline critical-path excess: 126 / 13 / 147 / 154 (post-scan 110 / 11 / 123 / 141 + small build-side
exchanges). Measured recoverable: 413 ms (batch4g leaves 28). Allocator share (counterfactual at g1 per-call latency):
≈ −80 / −5 / −92 / −94 = **−270 ms** — a mechanism split inside F3, not additive.

### F4 Shuffle bandwidth floor — 351 ms (design + topology). Confidence medium.

**Evidence.** Q9 shuffles 15.0 GB (partsupp, CONCAT(2), critical 136 ms vs g1 23), 9.1 GB (CONCAT(28)), 11.4 GB (CONCAT(31));
clone payload 37.4 GB staged + 9.3 GB peer (nsys). Unchanged by batch4g (floor 352 both runs; Q9 CONCAT(2) 136→145,
CONCAT(31) 174→179). At g1 nothing moves.

**Mechanism.** Hash shuffles move (N−1)/N of the bytes; pin placement is round-robin, not co-partitioned
(`task_creator.cpp:793-822`), so no join is local; 2/3 of remote bytes cross switches with no P2P (4 cross pairs: 77 GB/s agg).

**Pricing.** remote GB × 2/3 / 77 GB/s per moving CONCAT = 8.7 ms per remote GB: 41 / 42 / 26 / 242. Lower bound for any
shuffle plan on this topology; joinpol removes the Q5/Q8 share by broadcasting.

### F5 Synchronous HOST-staged accumulated Bloom publication on the build barrier — 123 ms (design). Confidence high.

**Evidence.** g4_a iter3 log vs Quent: Q5 Bloom 2 published +19.1 ms after the final contribution; three PARTITION(26) tasks end
at 168.5 ms, the owing one at **184.9 ms**; CONCAT(27) created 184.9; lineitem scan created 199.6. Q8 37.9 ms, Q9 34.3, Q5
Bloom 1 10.9, Q7 6.1. g1: no accumulated Bloom (single-partition build publishes directly). Unchanged by batch4g (bucket 122;
60 publication lines in both runs); joinpol removes them for Q5/Q8/Q9 (60 → 28 lines suite-wide).

**Mechanism.** `publish()` runs on the task that owes the publication (`src/op/dynamic_filter/dynamic_filter_publisher.cpp:381-432`)
inside the build-side PARTITION, so the reduction + host-staged fan-out holds the pipeline barrier. Transport is all-or-nothing:
any pair without pool access → HOST staging (`src/cuda/sirius_dynamic_bloom_filter.cu:1080`); every 4-GPU set has 8 such pairs.

**Pricing.** Critical-path excess of the Bloom-owning PARTITION pipelines (Q5 P16+P22, Q7 P7, Q8 P25+P31, Q9 P22), ~3 ms of
which is real partition work: 32 / 6 / 50 / 35.

### F6 Fixed per-query overhead outside the Quent window — 86 ms (unknown). Confidence medium.

timings.csv − Quent wall = 23-31 ms per query at both g1 and g4 (23/26, 25/26, 31/32, 30/29) → serial, non-scaling
(planning, result handoff). Priced as out_g4 − out_g1/4. Not captured by nsys (capture range = query).

### Context: join-mode confound (−18 ms net) and honest denominators

Q9 HASH_JOIN(29) 215 → 26 ms, HASH_JOIN(32)+GROUP_BY 323 → 68 ms (BUILD_PROBE at g4 vs STANDARD at g1, CRITIC C4) flatters g4.
For this family the g1 denominator barely moves under equal policy (x_g1_joinpol 1260 / 1436 / 1799 / 2991), so the confound
does not distort the family gap. batch4g also speeds g1 (−59 / −95 / −67 / −112 ms), which is why its gap reduction (646) is
smaller than its g4 gain (729).

## 4. Candidate remedies (family wall ms at g4; alternatives overlap, not additive)

| # | Remedy | Hits | Est. family saving at g4 | Basis |
|---|---|---|---|---|
| R1 | Coarser scan fragments: `scan_task_batch_size` 4 GB (config), or producer-side coalescing of partition outputs before the exchange | F3, F2 | **−729 measured** (gap −646) | x_g4_batch4g |
| R2 | Cost-based broadcast vs shuffle per join (bytes × (N−1) vs (build+probe)·(N−1)/N + per-fragment fan-in cost + forced partition + topology), at `sirius_physical_hash_join.cpp:867-899` | F2, F3, F5, F4 (Q5/Q8) | −772 alone (blanket −619 + avoid Q7 +153); **−81 on top of R1** (best-of 3795 vs b4g 3876) | x_g4_joinpol, x_g4_best |
| R3 | Batched cross-GPU clone: one staged transfer per (src, dst, task), no per-buffer `sync()`, packed partition outputs | F2 residual | up to −276 after R1 (−639 without R1) | clone floor split |
| R4 | Fewer driver allocations in partition: contiguous per-partition output instead of per-column slice deep copies, or a per-thread user-space sub-allocator over the pool | F3 (and free storms in F2) | ≈ −270 without R1; ≈ 0 after R1 (F3 already 28) | allocator counterfactual, thr8 |
| R5 | Consumer-first scheduling (`priority_order::sink` / lookahead) so PARTITION/CONCAT overlap the link-bound scan | F3, F2 post-scan | up to −385 without R1; small after R1 | Quent create-vs-start |
| R6 | Bloom: publish asynchronously off the barrier task; hierarchical transport (peer within {0,1},{2,3}, one host leg) | F5 | −85 to −123 (orthogonal to R1) | log latencies |
| R7 | Co-partitioned pinning on join keys (partsupp/lineitem on partkey, orders/lineitem on orderkey), switch-aware placement | F4, part of F2 | up to −351 (mostly Q9 −242) | F4 floor |
| R8 | Fewer bytes on the link: compress part/partsupp/supplier/customer (Q9 17.8 GB, Q8 6.5 GB uncompressed) | F1 | ≈ −100 (Q9) −37 (Q8); g1 gains 2x more, scaling ratio barely moves | nsys bytes |
| R9 | GPU-resident hot tables / larger GPU cache | F1 | scan phase −1 541 measured, query −854 measured (confounded; tail grows unless R1/R4 applied) | x_g4_gpupin |

Best measured config for the family today: per-query best-of (R1 + R2 where it helps) = 3 795 ms, gap ≈ 1 916 ms of which
F1 link ≈ 1 200, shuffle floor 352, Bloom 122, outside 85, residual clone overhead ≈ 150.

## 5. Not pursued / not priced

- **Per-op compute inflation on scan-side ops** (CRITIC N1): DYNAMIC_FILTER 1.4-1.5x and GPU_SCAN 1.5-1.95x thread-time at g4,
  sync-dominated with flat kernels. **0 wall ms in this family** today (under the saturated link); surfaces once F1 is removed (R9).
- Small-pinned pool mutex (sync H1) and `acquire_stream` pool sharing in `convert_gpu_to_gpu` (`:926`, N4): not separated from F2.
- hpb256/hpb4g: Q9 +128 / +109 ms (P count of its big shuffles); Q5/Q7/Q8 within ±10 ms. Mechanism not analysed.
- `fopen` inside clone ranges (≈22 thread-ms Q5): curiosity, not priced.
- Q7 "accumulated Bloom refused: per-GPU cap" occurs at g1 and g4 → no scaling cost.
- 2-GPU rows (g2s 1.05-1.18x, uplink-bound): out of scope (M11).
- gpupin per-pipeline accounting beyond the scan/partition rows: confounded by memory-pressure downgrades.
