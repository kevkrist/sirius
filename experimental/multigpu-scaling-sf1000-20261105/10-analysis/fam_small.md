# Family "small" (Q2, Q11, Q16, Q22): why 4 GPUs give only 1.5-1.6x

Scope: TPC-H SF1000, host-pinned, 4x RTX PRO 6000, commit bb557b1a. Read-only analysis of existing data. No new GPU runs.
- Runs used: g1_a, g2s_a, g2x_a, g4_a, g4_noquent (un-profiled); nsys2_g1 / nsys2_g4 (iter2); and 20-experiments
  (gpupin, batch4g, batch8g, hpb256, hpb4g, thr8, joinpol, best, plus the g1 variants).
- Scripts and intermediates: `10-analysis/fam_small_work/`.
  - Accounting: `pipe_table.py` (per pipeline), `phase.py` (1 ms bins), `classify.py` (gap by class), `hideable.py` (link idle).
  - nsys: `rb.py` (range breakdown), `syncwait.py` (what a long stream sync waits on), `children.py`, `nvtx_top.py`, `memcpy_dev.py`.
  - Flattened Quent: `qf_g2s`, `qf_g2x`, `qf_joinpol`, `qf_x_g4_{gpupin,batch4g,batch8g,thr8,hpb4g}`.
- Number rules:
  - Absolute times are un-profiled hot means (iters 2-4). Quent numbers are averaged over iters 2-4.
  - nsys is used only for proportions and API detail. nsys query wall is 1.0-1.07x the un-profiled time for this family.
  - Thread-ms is summed over executor threads (4 per GPU). Every price below is **g4 wall ms**.

**Bottom line**
- Family gap to linear at g4 is **714 ms** (ideal 458, measured 1172). That is 6.4% of the 11.08 s suite gap.
- **F1, the host-link hardware ceiling, is 381 ms (53%).**
  - These queries stream 13-30 GB of *uncompressed* host-pinned columns: part, partsupp, customer, supplier.
  - Measured rates are 54 GB/s at g1 and 99-107 GB/s at g4. Two uplinks give exactly 2x, not 4x.
  - The experiment confirms it. With tables pinned on the GPU (`x_g4_gpupin`), Q2 drops 373 → 102 ms and Q11 252 → 89 ms,
    *below* ideal (149 / 98).
- **About 36% is engine work that exists only at N>1 and sits on the critical path while the link is idle:**
  - cross-GPU gathers (F2, 161 ms)
  - scan-task quantization (F8, 43)
  - forced partitioning plus allocator contention (F3, 30)
  - dynamic-filter publication (F5, 20)
  - serial tails (F7, 19)
  - per-task overhead (F9, 16)
- **6.5% is fixed per-query planning** (F6, 46 ms).
- **No tested config flip helps this family.** The flips that help the suite hurt here:
  - batch4g +41 ms, best +73, joinpol +34, hpb4g +46
  - thr8 +6 and hpb256 +2 are neutral
- 2 → 4 GPUs adds no host bandwidth. g2x matches g4 here: 1163 vs 1172 ms family.

## 1. Time accounting

### 1a. Wall time and speedups (un-profiled hot mean, ms)

| Q | g1 | g2s | g2x | g4 | ideal g4 | gap | speedup g2s / g2x / g4 |
|---|---|---|---|---|---|---|---|
| 2 | 595 | 606 | 355 | 373 | 149 | 225 | 0.98 / 1.68 / 1.59 |
| 11 | 390 | 393 | 233 | 252 | 98 | 155 | 0.99 / 1.68 / 1.55 |
| 16 | 458 | 434 | 329 | 304 | 115 | 189 | 1.06 / 1.39 / 1.51 |
| 22 | 388 | 320 | 246 | 243 | 97 | 146 | 1.21 / 1.58 / 1.60 |
| **family** | 1831 | 1753 | 1163 | 1172 | 458 | **714** | 1.04 / 1.57 / 1.56 |

g4_noquent gives 376 / 251 / 302 / 239 ms, so Quent overhead is nil.

### 1b. Where the wall goes (Quent 1 ms bins, iters 2-4)

- "Scan" means a HOST-origin task is uploading.
- "Outside" means CLI time minus the Quent window: planning plus result return.

| Q | host GB | scan ms g1 → g4 | scan GB/s g1 → g4 (g2x) | link floor g4 (GB / 105) | non-scan ms g1 → g4 (g2x) | outside ms g1 / g4 (of which planning) |
|---|---|---|---|---|---|---|
| 2 | 30.22 | 557 → 303 | 54 → 100 (101) | 288 | 17 → 50 (36) | 19.5 / 19.5 (17) |
| 11 | 19.30 | 354 → 191 | 55 → 101 (102) | 184 | 24 → 49 (31) | 11.6 / 12.0 (10-11) |
| 16 | 15.42 | 283 → 156 | 55 → 99 (102) | 147 | 162 → 134 (164) | 13.4 / 13.8 (10-11) |
| 22 | 13.50 | 261 → 126 | 52 → 107 (98) | 129 | 113 → 101 (92) | 14.4 / 15.5 (13-15) |

### 1c. Utilization

**Per-GPU busy** (at least one task in flight) at g4 is 0.84-0.94.

**Thread occupancy** = task-ms / (wall x threads):

| Q | g1 | g4 |
|---|---|---|
| 2 | 0.88 | 0.56 |
| 11 | 0.87 | 0.52 |
| 16 | 0.68 | 0.48 |
| 22 | 0.77 | 0.59 |

**Kernel busy** per GPU (nsys):

| Q | g1 | g4 |
|---|---|---|
| 2 | 0.06 | 0.03-0.04 |
| 11 | 0.13 | 0.05-0.06 |
| 16 | 0.47 | 0.22-0.23 |
| 22 | 0.52 | 0.18-0.22 |

These queries are transfer- and latency-bound. The SMs are nearly idle.

**thr8 at g4** (8 executor threads) leaves wall unchanged: 1178 vs 1172 ms. Q16 scan prepare grows 905 → 1348 thread-ms and
PARTITION(5) compute 126 → 222 thread-ms, at identical pipeline windows. Extra threads only wait longer on the link and the
allocator.

### 1d. Gap split by class (ms at g4: measured minus g1/4)

Source: `classify.py`.

| class | Q2 | Q11 | Q16 | Q22 | family |
|---|---|---|---|---|---|
| scan phase (link) | 163.3 | 102.8 | 85.3 | 60.7 | 412 |
| cross-GPU gather (CONCAT / MERGE_GROUP_BY wall) | 20.6 | 29.8 | 57.1 | 53.4 | 161 |
| outside engine (planning, result) | 14.6 | 9.1 | 10.5 | 11.9 | 46 |
| PARTITION (pass-through at g1) | 8.3 | 3.3 | 23.2 | 6.7 | 41 |
| compute ops (HASH_JOIN etc.) | 10.7 | 7.1 | 3.1 | 2.9 | 24 |
| single-GPU serial tail | 4.7 | 2.8 | 6.2 | 5.4 | 19 |
| scan compute tail (after last upload) | 2.4 | 0.3 | 4.0 | 4.4 | 11 |
| **gap** | 224.7 | 154.8 | 189.2 | 145.6 | **714** |

### 1e. Critical path at g4 (nsys iter2, ms from query start)

Every query is a strict sequence: scan, then exchange, then the next scan. **The link sits idle during every exchange.**

- **Q16:**
  - supplier scan (2 tasks): 0-14
  - part scan: 15-100
  - PARTITION(5): 100-124, which includes `dynfilter::accum::finish` at 113-124
  - CONCAT(6): 124-151
  - partsupp scan: 151-213
  - CONCAT(10): 213-226
  - HJ(11), then HJ(14) + agg: 224-268
  - MERGE_GROUP_BY: 271-297 (3.7 ms at g1)
  - sort and result: 297-306
- **Q22:**
  - customer scans: 0-42 and 47-87
  - CONCAT(14), on 2 GPUs: 79-116
  - NLJ and group-by: 116-125
  - CONCAT(21): 126-137
  - orders scan: 130-180
  - HJ(26): 182-196
  - P/CONCAT(28-29) and P/CONCAT(31-32): 196-224
  - HJ(33): to 231
  - tail: to 234
- **Q11:**
  - partsupp scan #1: 5-82
  - partsupp scan #2: 92-203
  - HJ(35) + agg: to 209
  - MERGE_GROUP_BY: 212-224
  - CONCAT(42): 225-244. It has 2 tasks: 0.81 GB on GPU0 and 0.27 GB on GPU2.
  - NLJ and result: 244-249
- **Q2:**
  - supplier/nation scan (2 tasks): 14-34
  - HJ, P(34), CONCAT(35): 34-50
  - part scan: 50-140
  - CONCAT(38): 140-173, overlapping the start of the next scan
  - partsupp scan #1: 148-240
  - join/agg/concat chain: 240-252
  - partsupp scan #2: 252-344
  - join, top-N, merge tail: 344-372

**Link-idle time before the last scan starts** (`hideable.py`) is the part a scan lookahead could hide:

| | Q2 | Q11 | Q16 | Q22 | total |
|---|---|---|---|---|---|
| g1 (ms) | 8 | 6 | 8 | 21 | 44 |
| g4 (ms) | 34 | 6 | 48 | 50 | 137 |

### 1f. Config-flip experiments (un-profiled hot means, ms, family queries only)

| run | Q2 | Q11 | Q16 | Q22 | family | Δ vs base | suite total |
|---|---|---|---|---|---|---|---|
| g4_a (base) | 373 | 252 | 304 | 243 | 1172 | — | 20.04 |
| x_g4_gpupin (tables in GPU memory, no host link) | **102** | **89** | 336* | 259* | 786 | −386 | 18.26 |
| x_g4_batch4g (scan_task_batch_size 4 GB) | 396 | 247 | 320 | 249 | 1213 | +41 | 16.91 |
| x_g4_batch8g | 488 | 323 | 364 | 258 | 1433 | +261 | 17.34 |
| x_g4_hpb256 (hash_partition_bytes 256 MB) | 379 | 254 | 303 | 238 | 1174 | +2 | 23.66 |
| x_g4_hpb4g | 378 | 261 | 303 | **276** | 1218 | +46 | 19.91 |
| x_g4_thr8 | 375 | 248 | 305 | 250 | 1178 | +6 | 21.23 |
| x_g4_joinpol | 387 | 252 | 324 | 243 | 1206 | +34 | 19.07 |
| x_g4_best (batch4g + joinpol) | 411 | 248 | 334 | 252 | 1245 | +73 | 16.92 |
| g1: base / batch4g / thr8 / joinpol / best | — | — | — | — | 1831 / 1838 / 1826 / 1808 / 1807 | | |

\* gpupin Q16 and Q22 are confounded by GPU memory pressure: about 75 GB of 92 GB allocated per GPU, and 292 monitor
downgrade requests during Q16 iters 2-4. Intermediates are downgraded to host and re-uploaded.
- Q16 PARTITION(12) + CONCAT(13) gain 145 thread-ms of HOST-origin prepare, and their wall goes from 221-223 ms to
  112-244 ms.
- Q22 HJ(26) goes from 4 to 7 tasks, with wall 106-183 ms vs 177-191 ms.
- **Before the confound starts**, the link-free run saves exactly the scan phases: Q16 reaches the end of HJ(11) at
  122 ms vs 222 ms (−100 ms), and Q22 reaches the end of PARTITION(24) at 110 ms vs 175 ms (−65 ms).

**gpupin as a counterfactual.** It is clean for Q2 and Q11, and estimated from the clean prefix for Q16 and Q22.
- Link-free g4 wall (Quent window) is about 83 / 78 / ~180 / ~160 ms. Ideal Quent g1/4 is 143 / 95 / 111 / 94.
- So Q2 and Q11 are 100% link-limited: they go superlinear without the link, even with their gathers.
- Q16 and Q22 keep about 70 ms each of non-link gap. That matches F2 + F3 + F5 + F7 + F9 priced from the host-pinned run
  (Q16 90 ms, Q22 68 ms).

## 2. Findings (priced in g4 wall ms against linear g1/4)

The prices are additive across rows and sum to the 714 ms family gap. The exceptions are F4 (a re-attribution) and F10
(measurement). Suite share = family ms / 11 080.

| # | finding | kind | Q2 | Q11 | Q16 | Q22 | family | conf. |
|---|---|---|---|---|---|---|---|---|
| F1 | Host-link ceiling on link-bound scan phases | hardware | 148.5 | 95.3 | 76.2 | 60.7 | **381** | high |
| F2 | Cross-GPU gathers: host-staged, serial per-buffer sync, P<N fan-in | design + mech. | 20.6 | 29.8 | 57.1 | 53.4 | **161** | high |
| F6 | Per-query planning and result return (serial, fixed) | design | 14.6 | 9.1 | 10.5 | 11.9 | **46** | high |
| F8 | Scan-task quantization and ramp on small tables, scan compute tail | config + design | 17.3 | 7.8 | 13.1 | 4.4 | **43** | high (was medium) |
| F3 | N-forced hash partitioning of small exchanges, plus allocator/lock contention | design + mech. | 7.6 | 3.3 | 12.1 | 6.7 | **30** | high |
| F5 | Dynamic-filter publication at N>1 (accumulated Bloom finish on the critical path; host-staged replicate stuck behind scan H2D; unfiltered probes) | design + mech. | 0.7 | 0 | 11.1 | 7.8 | **20** | medium-high |
| F7 | Single-GPU serial tails (merge top-N, sort, merge-agg, NLJ) | design | 4.7 | 2.8 | 6.2 | 5.4 | **19** | high |
| F9 | Per-task launch/sync/alloc inflation on tiny inputs | mechanical | 10.7 | 7.1 | 3.1 | −4.9 | **16** | medium |
| F4 | No scan lookahead: the next scan's upload never overlaps an exchange (could hide F2, F3, F5) | design | (≤34) | (≤6) | (≤48) | (≤50) | (≤126, not additive) | medium |
| F10 | CUPTI labels pinned-host → device copies as Peer-to-Peer at g4 | measurement | 0 | 0 | 0 | 0 | 0 | high |

### F1 Host-link ceiling (hardware): 381 ms, 53%

Mechanism:
- part, partsupp, customer and supplier are pinned *uncompressed*. There is no Simpatico plan for them; only lineitem and
  orders are compressed.
- Every scan streams through 2 Gen5 x16 switch uplinks: about 52 GB/s for 1 GPU, 105 GB/s for 4 GPUs
  (microbench `h2d_concurrency`).

Evidence:
- Scan-phase rates are 52-55 GB/s at g1 and 99-107 GB/s at g4 (table 1b).
- g2x scan phases (300 / 190 / 151 / 138 ms) equal g4 (303 / 191 / 156 / 126). g2s equals g1. Scan time depends only on the
  number of uplinks.
- gpupin removes the link: Q2 −271 ms, Q11 −163 ms, and on the clean prefix Q16 −100 and Q22 −65 (§1f).
- nsys g4 Q2: the 30.6 GB of host bytes arrive at 26-28 GB/s per GPU. A 575 MB chunk takes 20.6 ms, matching the 4-GPU
  microbench share of 26 GB/s per GPU.

Pricing:
- Price = (host GB / 105 GB/s) − (g1 scan / 4).
- Q22's scan is below the floor (compressed `o_custkey`), so its whole scan excess is priced here.

Bytes multipliers (design; remedy R1):
- Q2 scans partsupp twice, 9.6 GB each (pipelines 33 and 45).
- Q11 scans partsupp at 8.0 GB and then 11.2 GB, where the first scan's columns are a subset of the second's.
- Q22 scans customer twice, at 3.45 and 4.05 GB.

### F2 Cross-GPU gathers (design + mechanical): 161 ms, 23%

Mechanism:
- A partitioned consumer pins partition p to `gpus[p % N]` (`src/op/partition_placement.hpp:59`).
- Its prepare then clones about 3/4 of the input pieces (`batch_lock_utils.hpp:123-137` →
  `cucascade/.../representation_converter_builtins.cpp:900-950`).
  - Each clone handles one column at a time and ends with `target_stream.sync()`.
  - Cross-switch pairs (2/3 of remote pieces) go D2H, sync, then H2D through host staging (`:574-640`).
- At g1 the same CONCATs are local: about 1 ms of GPU-origin prepare per query.

Evidence (nsys g4 iter2):

| Q | host-staged clones | peer clones |
|---|---|---|
| Q2 | 80 calls / 43 ms | 40 / 8 ms |
| Q11 | 33 / 42 ms | 17 / 9 ms |
| Q16 | 94 / 122 ms | 47 / 20 ms |
| Q22 | 56 / 122 ms | 28 / 20 ms |

- The 2:1 ratio of staged to peer clones is exactly what the topology predicts.
- Q16 CONCAT(6) is clone cost, not compute. It runs 100 thread-ms against 0.7 ms of kernels, with 990
  `cuStreamSynchronize` calls. Its effective clone rate is about 13 GB/s.
- Q16 MERGE_GROUP_BY(20) is 24 ms at g4 vs 3 ms at g1. The `count(distinct)` partials must be re-aggregated at N>1:
  52 thread-ms of `grouped_aggregate_merge::execute`.

New experimental confirmation: **P<N fan-in.** In x_g4_hpb4g, Q22 CONCAT(14) drops from P=2 to P=1, a single task on 1 GPU.
- prepare from GPU memory is 49 ms; the pipeline runs 84-137 ms instead of 75-112 ms.
- Q22 goes 243 → 276 ms (+33).
- The number of gather destinations is set by `hash_partition_bytes`, not by N. Fewer destinations give a longer serial
  clone chain.
- In gpupin, Q11's CONCAT(42) (P=2) is still 22 of the 78 ms link-free wall.

Pricing: CONCAT/MERGE_GROUP_BY critical-path wall at g4 minus g1/4 (table 1d).

### F6 Per-query planning (fixed cost): 46 ms, 6.5%

- From `QueryBegin SQL` to `executing query` takes 17 / 10-11 / 10-11 / 13-15 ms. It is the same at g1 and g4, and it is paid on
  every hot iteration even though the log says "reusing finalize-validated Sirius plan".
- This is DuckDB parse, bind and optimize, plus statistics hooks. Sirius plan generation fits in the last ~1 ms.
- Pricing: outside(g4) − outside(g1)/4.

### F8 Scan-task quantization and ramp: 43 ms, 6% (now experimentally confirmed)

Mechanism: with `scan_task_batch_size` = 1 GB, supplier (0.70 / 1.45 GB) becomes 2 tasks on 2 GPUs.
- Q16 pipeline 0: 14 ms at both g1 and g4, against a floor of 6.7 ms.
- Q2 pipeline 24: 22 ms, against a floor of 14 ms.
- Scan-phase ramp (wall over the floor) is 14.9 / 7.5 / 9.1 / 0 ms. The scan compute tail adds 11 ms.

Experiments (the family is on the *opposite* side of the batch-size trade-off from the suite):

| run | change | effect |
|---|---|---|
| batch4g | Q2 partsupp scans go from 23/17/17 tasks to 6/5/5 | scan #1 takes 113 ms instead of 90 (84 vs 106 GB/s): with 6 tasks on 4 GPUs, 2 GPUs carry a second task. Q2 supplier scan becomes 1 task (29 ms vs 22). |
| batch4g | Q2 CONCAT(38) | shrinks 42 → 4 ms with fewer fragments |
| batch4g | net | Q2 +23 ms, Q16 +16 (part scan: 6 tasks, 109 vs 85 ms) |
| batch8g | partsupp becomes 3 tasks on 3 GPUs, leaving one GPU and half an uplink idle | Q2 +115 ms (scans 118-150 ms each), Q11 +71 ms |

Pricing is unchanged from v1: wall in excess of the link floor attributable to the task count. The experiments show the price
is a *lower bound* on the cost of coarser batches.

### F3 N-forced partitioning plus allocator contention: 30 ms, 4%

Mechanism: `natural_num_partitions` floors P at N once the input is at least 16 MB x N
(`src/op/partition/partition_sizing.hpp:86-120`).
- At g1 these PARTITIONs are pass-through, 0-1 ms.
- At g4 they run hash_partition plus a slice deep copy, with about 48 `cudaMallocFromPoolAsync` calls per task.

Evidence:
- Q16 PARTITION(5), g4: 23 tasks, 180 thread-ms, 8.9 ms of kernels.
- Inside it:

  | call | count | total |
  |---|---|---|
  | malloc | 1105 calls at 42 µs | 46.6 ms |
  | free | 691 calls at 40 µs | 27.5 ms |
  | `pthread_mutex_lock` | — | 56.5 ms |
  | `rwlock_wrlock` | — | 38.3 ms |

- The microbench `alloc_contention` explains this: the driver pool allocator is serialized process-wide. Mean latency is
  10.9 µs for 4 threads on 1 GPU and 56 µs for 16 threads on 4 GPUs.
- thr8 makes it worse in thread-time (126 → 222 thread-ms) at the same wall.

### F5 Dynamic-filter publication at N>1: 20 ms, 3%

- **Accumulated Bloom on the critical path (Q16).**
  - 59 MB per GPU, built from 23 batches.
  - `dynfilter::accum::finish` takes 11.1 ms *inside the last PARTITION(5) task*, and CONCAT(6) waits for it.
  - The log says "HOST staging, 13.2-13.6 ms after final contribution" (g4 iters 2-4).
  - Q2 shows the same pattern at 0.7 ms.
- **F5b (new): the host-staged replicate is stuck behind bulk scan H2D on the same GPU.**
  - In Q2 g4 nsys, CONCAT(38)'s sink runs `dynfilter::publish_hook → replicate_devices` for 25.2 ms (147.3-172.5).
  - 21.1 ms of that is one `cudaStreamSynchronize` on dev0 stream 177. That stream has *no* work of its own during the wait
    (`syncwait.py`).
  - The sync returns at 172.49, exactly when dev0's 575 MB scan chunk (stream 50, 151.39-172.02) leaves the H2D copy
    engine.
  - So the Bloom staging leg queues FIFO behind the next scan's uploads. The partsupp scan #1 has already started (143 ms),
    unfiltered.
  - This is the mechanism behind v1's "replicate race": Q22 replicate 8.5 ms concurrent with the orders scan; Q2 iter2/3
    missing the filter on op41/op56 (1.81 / 0.35 / 0.47 GB vs 0.01 GB); Q11 profiled replicate 99 ms.
  - Code: `src/op/dynamic_filter/dynamic_filter_publisher.cpp:1201-1224`, serial per-device replicate with host staging.
- **Pricing.** Q16 is the accumulate on the critical path (11.1 ms). Q22 is the unfiltered HJ(26) (+7.8 ms). Q2 is 0.7 ms.
  - Q2's Bloom-miss variance (iter3 383 vs 368 ms) is not priced.
  - The replicate is off the critical path when the scan is link-bound: it delays filtering, not the link.

### F7 Single-GPU serial tails: 19 ms, 3%

| Q | tail | g4 | g1 |
|---|---|---|---|
| Q2 | MERGE_TOP_N + RESULT | 5.8 ms | 2.1 ms |
| Q16 | SORT_SAMPLE + MERGE_SORT + RESULT | 8.4 ms | 1.8 ms |
| Q22 | MERGE_AGGREGATE (gather of 31 partials) | 3.6 ms | 1.0 ms |
| Q22 | NLJ | +2.8 ms | — |
| Q11 | tail | 2.8 ms | — |

### F9 Per-task inflation on tiny inputs: 16 ms

- Q2 HJ(73) → TOP_N: 4 tasks, 31.1 thread-ms, 7.8 ms of kernels and 12.4 ms of sync. At g1 it is 1 task with 5.8 / 2.9 ms.
- Family API totals, g1 → g4:
  - `cudaLaunchKernel`: 35 → 252 thread-ms (mean 6.4 → 29.5 µs)
  - `cudaEventCreateWithFlags`: 3 → 143 thread-ms
- Q22 is net negative here because HJ(33) scaled superlinearly.

### F4 No scan lookahead (re-attribution, not additive)

- Only `scans.front()` is scheduled (`src/pipeline/task_scheduler.cpp:209-221`), with `strategy=active` and
  `priority=source` (`src/creator/config.hpp:101,106`).
- Link idle before the last scan is 137 ms at g4 vs 44 ms at g1, so up to 126 ms of gap is hideable.
- About 90 ms is realistic, because staged clones also use the uplinks, and F5b shows that bulk H2D starves small
  latency-critical copies on the same engine.

### F10 Measurement: CUPTI copyKind is unreliable for pinned-host H2D at g4 (0 ms; affects other analyses)

In nsys2_g4 Q2:

| copyKind | srcKind | bytes |
|---|---|---|
| 1 (HtoD) | 1 (pinned) | 17.54 GB |
| **10 (Peer-to-Peer)** | 1 (pinned) | **13.08 GB** |

- The copies labeled Peer-to-Peer are `convert:host_to_gpu:flush_submit` / `offsets_cast` batch copies
  (`cudaMemcpyBatchAsync`), with a "src device" that is just the context owning the pinned buffer.
- At g1 all 30.22 GB is labeled HtoD.
- `nsys_range_breakdown.py`'s "Peer-to-Peer" GB, and any copy-kind-based HtoD or P2P total at g4, therefore overstate P2P and
  understate H2D by about 43%.
- Classify by `srcKind`/`dstKind` (1 = pinned host, 2 = device) instead.

## 3. Candidate remedies (g4 family wall saving)

| # | remedy | targets | est. saving | basis |
|---|---|---|---|---|
| R1 | Share repeated scans within a query: reuse the first scan's device chunks, or a materialized-scan CTE | F1 bytes | **200 ms** (Q2 91, Q11 76, Q22 33); gap −103 (g1 also gains) | 21.05 GB / 105 GB/s |
| R2 | Scan lookahead: prefetch the next scan during exchange phases | F4 (hides F2, F3, F5) | ≤137, ~90 realistic | §1e |
| R3 | Cheaper gathers: batched async clones with one sync per batch; switch-aware placement; P ≥ N and P a multiple of N for gathers (never P<N) | F2 | ~80 of 161 | staged clone 1.3-2.2 ms/call vs peer 0.2-0.7; hpb4g P=1 costs +33 ms on Q22 |
| R4 | Compress part, partsupp, customer and supplier | F1 | unpriced, ≤50% of 778 ms scan time at 2x if decode is hidden (SMs 77-97% idle) | needs E7 |
| R5 | Dynamic filter: replicate peer and parallel, on a priority / dedicated copy path (not queued behind bulk H2D); bounded probe wait; finish the accumulation off the last producer task | F5, F5b | ~20 + variance | §F5 |
| R6 | Plan cache for repeated SQL | F6 | 46 of gap (60 absolute) | log |
| R7 | Skip the P=N floor for cheap consumers; pre-size partition outputs; per-GPU pools | F3, F9 | ~30-40 | PARTITION(5) is 41% allocator |
| R8 | **Per-table** scan batch size: about 1 GB (≥ 4N tasks) for uncompressed dimension tables, 4 GB for compressed lineitem/orders; at least N tasks for small tables | F8, and lets the suite keep batch4g (−3.1 s) without the family's +41 ms | ~14 + 41 avoided | batch4g / batch8g §1f |
| (tested) | joinpol / best / hpb4g / thr8 / hpb256 | — | none help: +34 / +73 / +46 / +6 / +2 ms | §1f |

## 4. Not priced / not pursued

- **gpupin memory-pressure confound** (Q16 and Q22 slower than base despite no link): this is downgrade of intermediates at
  about 75 of 92 GB per GPU. It is not part of the host-pinned regime, so it is not priced.
- **Compression ratio** for the uncompressed tables (R4) was not measured.
- **Root cause of the remaining mutex/rwlock time** in PARTITION: numa_small_pinned_mr `map_mutex_` vs the shared slab
  pool (CRITIC N1). It is only bounded by the allocator microbench.
- **nsys3_g4** does not cover the family queries. No additional GPU runs were made.
- **2-GPU analysis** beyond the scan-phase check: g2s ≈ g1 and g2x ≈ g4 for this family.
