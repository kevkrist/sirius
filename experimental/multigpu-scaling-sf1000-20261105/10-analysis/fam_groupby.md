# Family "groupby": TPC-H Q13, Q18. Why they miss 4x at 4 GPUs

Regime: SF1000, host-pinned, commit bb557b1a, configs from `00-baseline/`. Absolute times are un-profiled
hot means. Phase times come from Quent iter3, with g1_a/g4_a matching the hot means within 1.5%. nsys
(iter2, profiled 1.13x at g1 and 1.21x at g4) is used for attribution only. Scripts and outputs are in
`10-analysis/fam_groupby/`:
- `pipe_table.py`, `task_list.py`: Quent phases and per-task timelines.
- `nsys_qsum.py`, `nsys_timeline.py`, `nsys_task_phases.py`, `nsys_pipe_spans.py`, `alloc_rate.py`: nsys analysis.
- `nsys_q13_*.txt`, `nsys_q18_*.txt`, `nsys_q18_ranges.txt`: saved outputs.
- `qf/{g2s,g2x,x_g4_joinpol}`: flattened Quent.
- Revision 2 (17:50) added config-flip validation: `qf/{x_g4_batch4g,x_g1_batch4g,x_g4_hpb4g,x_g4_hpb256,x_g4_thr8,x_g1_thr8,x_g4_gpupin}`
  and `exp_phase_tables.txt` (iter3 phase tables for both queries, all runs), `q13_hpb256_dcj_tasks.txt`.
  nsys3_g4 (separate Q18 capture) reproduces nsys2: kernel busy 299-301 ms per GPU, cudaMallocFromPoolAsync
  7975 thread-ms over 144.5k calls (55 µs mean), cudaFreeAsync 6319 ms.

## 0. Headline

| | g1 | g2s | g2x | g4 | x g4 | ideal g4 | gap to 4x (g4) |
|---|---|---|---|---|---|---|---|
| Q13 as reported (g1 mean incl. slow iter) | 3.011 | 1.915 | 1.438 | 1.089 | 2.76 | 0.753 | 0.336 s |
| Q13 corrected (g1 = mean of 4 non-pool-growth hot iters) | **2.558** | 1.34x | 1.78x | 1.089 | **2.35** | 0.640 | 0.449 s |
| Q13 "fair g1" (no DCJ over-reservation; estimate) | ~2.10 | | | 1.089 | ~1.93 | 0.526 | ~0.56 s |
| Q18 | 2.109 | 1.754 (1.20x) | 1.718 (1.23x) | 1.562 | **1.35** | 0.527 | **1.035 s** |

- Family gap is 1.37 s of the suite's 11.08 s (12%). Q18 alone accounts for 1.035 s, the 2nd-largest
  single-query gap.
- **GPU work scales perfectly.** nsys kernel-busy per GPU:
  - Q18: 1167 ms at g1 vs 298-301 ms at g4 (3.9x).
  - Q13: 450 ms vs 110-111 ms (4.1x).
- **All of the loss is host-side or in the links.** Every GPU is "busy" (Quent: some task in flight) 96-99%
  of wall at g4, but kernels run only 16% (Q18) and 10% (Q13) of the profiled window.

Ranked mechanisms, in family wall-ms at g4 (sum of the per-query prices below):

| # | Mechanism | Q13 | Q18 | Family | Kind |
|---|---|---|---|---|---|
| 1 | Cross-GPU exchange and fan-in done as serial per-buffer clones (M3/M5) | 190 | 502 | **692** | design + mechanical |
| 2 | Host link ceiling, 2 uplinks ≈ 110 GB/s (M1) | 340 | 121 | **461** | hardware |
| 3 | **Driver stream-ordered allocator is process-global**: alloc throughput is flat in #GPUs (new for this family; this is the CRITIC N1 "compute inflation") | ~0 | 353 | **353** | mechanical/design |
| 4 | Partition count 36 over 16 threads gives 3 waves (M7) | 0 | 19 | 19 | config |
| 5 | Small-pipeline latency floor (misc) | 16 | 24 | 40 | mechanical |
| M | g1-only DCJ over-reservation, spill and host-pool growth make g1 Q13 slow and variable (flatters scaling) | −228 | 0 | −228 | measurement / design (g1 side) |
| | Sum vs measured gap | 318 vs 336 | 1019 vs 1035 | 1337 vs 1371 | |

The residual (≤ 2%) comes from mixing iter3 phase data with hot means.

### 0.1 Config flips confirm the ranking (revision 2)

Hot means (s), un-profiled, 1 warm-up + 3 hot:

| run | Q13 g4 | Q18 g4 | Q13 g1 | Q18 g1 | Q18 x | what it tests |
|---|---|---|---|---|---|---|
| baseline | 1.089 | 1.562 | 3.011 (2.558 steady) | 2.109 | 1.35 | |
| batch4g (scan_task_batch_size 4 GB) | 1.107 | **1.000** | 2.603 | 1.872 | **1.87** | 4.3x fewer fragments (627 → 145 HGB partials) |
| hpb4g (hash_partition_bytes 4 GB) | 1.096 | 1.251 | — | — | — | 36 → 9 merge partitions |
| hpb256 (256 MB) | **1.797** | **3.672** | — | — | — | 36 → 141 merge partitions; Q13 DCJ 4 → 12 tasks |
| thr8 (8 threads/GPU) | 1.101 | **1.820** | 2.653 | 2.085 | 1.15 | 2x allocator callers |
| gpupin (no host link) | **0.633** | 2.220 (unstable 3.15→1.75, GPU memory pressure) | — | — | — | removes F3 |
| joinpol | 1.097 | 1.634 | 3.077 | 2.117 | — | no join on critical path |

Q18 phase walls (ms, Quent iter3; letters as in §1):

| phase | g1 | g4 | g4 batch4g | g1 batch4g | g4 hpb4g | g4 hpb256 | g4 thr8 | g1 thr8 |
|---|---|---|---|---|---|---|---|---|
| A P0-P2 | 113 | 101 | 102 | 111 | 99 | 102 | 104 | 110 |
| B P3 scan+HGB | 601 | 234 | **158** | 602 | 230 | 226 | 296 | 598 |
| C PARTITION(5) | 433 | 324 | **82** | 284 | 200 | **903** | **418** | 399 |
| C thread-ms | 1696 | 5099 | 1250 | 1123 | 3145 | 14342 | 13220 | 3147 |
| D MERGE+FILTER | 284 | 595 | **342** | 241 | 401 | **2181** | 628 | 298 |
| D prepGPU thread-ms | 27 | 6077 | 4235 | 6 | 2910 | 22865 | 12203 | 28 |
| F+H link scans | 607 | 254 | 250 | 580 | 253 | — | — | — |
| total | 2079 | 1540 | **959** | 1842 | 1218 | 3710 | 1755 | 2056 |

What this shows:
- **F2 (allocator) confirmed three ways.**
  - C thread-ms tracks allocation count: 627 → 145 tasks gives 5099 → 1250 (4.1x). Phase C falls 324 → 82 ms
    at g4 but only 433 → 284 at g1.
  - Doubling threads at g4 makes C worse (324 → 418 ms; thread-ms 2.6x). At g1 it does not (433 → 399).
    The global lock gets slower with more callers.
  - 4x more partitions (hpb256) makes C 2.8x worse (903 ms).
  - Measured batch4g saving on B + C at g4 is **−318 ms**, against the F2 price for B + C of 300 ms.
- **F1 (fan-in) is part count-bound and part bandwidth-bound.**
  - Fewer, larger partials (batch4g: 470 → about 109 remote pieces per merge task, same 27 GB remote) cut D by
    **253 ms** (595 → 342).
  - The rest is bandwidth. D at batch4g is 342 ms against an ideal of 60 (g1 batch4g/4). The driver-staged
    cross-switch floor is 18 GB / 77 GB/s ≈ 234 ms (p2p microbench).
  - Under gpupin, D stays at 619 ms (prepGPU 5689). The host-link-to-pinned-tables path is not involved.
  - hpb256 (141 partitions) multiplies the clone count and D grows to 2181 ms.
- **F3 (host link) confirmed for Q13.** Under gpupin, the orders scan phase falls from 771 to 81 ms and Q13 from
  1.089 to 0.633 s. The DCJ gather is unchanged (272 vs 264 ms), which isolates F4 from F3.
- **Q18 scaling at batch4g is 1.87x** (gap 0.532 s, down from 1.035). Q13 is unchanged by every knob except
  gpupin and hpb256.

## 1. Time accounting

### Q18 (iter3; g1 2079 ms, g4 1540 ms; Quent pipeline spans, ms from query start)

| Phase (pipelines) | g1 wall | g4 wall | ideal g1/4 | Δ | g1 → g4 thread-ms (Quent) | What g4 waits on |
|---|---|---|---|---|---|---|
| A. customer scan, partition, CONCAT (P0-P2) | 113 | 101 | 28 | 73 | P0 prepH 262→473; **P2 prepG 0→188** | P0: link (3.9 GB uncompressed, 35 ms at 110 GB/s). P2: clone gather 47 ms/task |
| B. lineitem scan + HASH_GROUP_BY(4) (P3) | 601 | 234 | 150 | 84 | prepH 636→1037, HGB 1600→2482 | 16 threads 98% busy, H2D only 49 GB/s, kernels 55-64% busy. Allocator (below) |
| C. **PARTITION(5)**, 36-way (P4) | 433 | 324 | 108 | **216** | 1696→5099 (3.0x) | Driver allocator. Kernels ~10% busy, no copies |
| D. **MERGE_GROUP_BY(6)+FILTER** (P5), 36 tasks | 284 | **595** | 71 | **524** | **prepG 27→6077**, compute 1070→2160 | 470 serial clones per task, then frees. 3 waves |
| E. P6-P7 | 2 | 6 | 0.5 | 5 | | small clones |
| F. orders scan + DYNAMIC_FILTER (P8-P9) | 288 | 124 | 72 | 52 | prepH 881→1483 | link, 77-109 GB/s |
| G. small joins (P10-P16) | 30 | 15 | 7.5 | 7.5 | | |
| H. lineitem rescan + DYNAMIC_FILTER (P17-P18) | 319 | 130 | 80 | 50 | prepH 1073→1791 | link, 86-109 GB/s |
| I. tail (P19-P23) | 9 | 11 | 2 | 9 | | |
| **Total** | 2079 | 1540 | 520 | **1020** | 8070 → 21894 | |

Additional detail:
- **Per-GPU busy at g4.** Union of task intervals: 1485-1523 ms of 1540. Tasks per GPU are 754-756, so
  balanced.
- **Phases are strictly sequential at both counts.** P3 starts only after P2, and P4 only after P3.
- **The same phases at 2 GPUs** (Quent iter3, flattened here):

  | Phase | g1 | g2s | g2x | g4 |
  |---|---|---|---|---|
  | C (PARTITION) | 433 | 316 | 313 | 324 |
  | D (MERGE) | 284 | 414 | 599 | 595 |

  **C is flat from 2 to 4 GPUs.** D is P2P-cheap at g2s and the same at g2x as at g4.
- **nsys, Q18 g4 vs g1.** CUDA runtime totals in the query window:

  | Call | g1 | g4 |
  |---|---|---|
  | cudaMallocFromPoolAsync | 699 ms / 109.8k calls | **7006 ms / 144.5k** |
  | cudaFreeAsync | 367 ms | **5929 ms** |
  | cuStreamSynchronize | 9.5k calls | **90.0k calls** |

### Q13 (iter3; g1 2457 ms is the fast mode, g4 1072 ms)

| Phase | g1 wall | g4 wall | ideal g1/4 | Δ | Notes |
|---|---|---|---|---|---|
| customer scan (P0) | 12 | 6 | 3 | 3 | |
| **orders scan (P2)**, 163 tasks | 1724 | **771** | 431 | **340** | nsys H2D **47.6 GB/s at g1 vs 110.6 GB/s at g4** (link-saturated, flat over 0-750 ms). 85.6 GB H2D at both. g2s 1532 ms (1 uplink), g2x 869 ms (2 uplinks) |
| PARTITION(1,3) | 90 | 26 | 22 | 4 | |
| wait for DCJ reservation | 176 | 1 | — | — | g1 only (finding M) |
| **DENSE_COUNT_JOIN (P4)**, 4 tasks | 443 | **264** | 111 | 153 | g4: each task prep 185-195 ms (GPU-origin gather) + 69 ms compute. g1: tasks serialized |
| tail | 12 | 4 | 3 | 1 | |
| **Total** | 2457 | 1072 | 614 | 458 | thread-ms 7999 → 13422 (prepHOST 6297 → 10415, prepGPU 3 → 772) |

Per-GPU busy at g4 is 1052-1072 ms of 1072. The scan-phase threads are 96% occupied, waiting on H2D.

## 2. Findings with evidence and pricing

### F1. Q18 MERGE_GROUP_BY fan-in: 470 cross-GPU clones per task, serial and per-buffer synchronized. **Q18 +452 ms (+50 ms for the customer CONCAT)**

- **Mechanism.**
  - `sirius_physical_grouped_aggregate_merge.cpp:216-243` drains all 627 partials of one partition into
    one task.
  - Placement is `round_robin(36, gpus)` (`:208-213`), so 3/4 of the partials (470 per task) are remote at
    g4.
  - Each remote partial is cloned in the task's Preparing phase by
    `cucascade/src/cudf/representation_converter_builtins.cpp:898-960`:
    - `stream.sync()` at `:902`.
    - A per-leaf peer copy, or for cross-switch pairs the host-staged path at `:620-631` / `:637-695`:
      D2H, then sync, then H2D, then sync per buffer.
- **Evidence (nsys g4, `nsys_task_phases.py`, mean per MERGE task):**

  | | g4 | g1 |
  |---|---|---|
  | prep | **198 ms** | 3.2 ms |
  | op range | 12.8 ms | 14.6 ms |
  | after (input release) | 58.9 ms | 15.7 ms |

  - Inside prep: 470 `convert:gpu_to_gpu` ranges (161.6 ms), **2196 cuStreamSynchronize (76 ms)**,
    940 mallocs + 940 frees (47 ms) and 1880 cudaMemcpyAsync.
  - Actual copy time is only ~32 ms per task (1.0-1.25 GB at 38-42 GB/s). So the clone is about 6x its
    transfer time.
  - **There is no compute inflation in the merge kernel itself** (12.8 vs 14.6 ms).
- **Per-piece cost** (Quent prepG / pieces): g2s (P2P) 0.18 ms, g2x (staged) 0.30 ms, g4 0.36 ms. Each
  piece is 1.6 MB, which is 32 µs at 50 GB/s.
- **Pricing.**
  - Busiest thread runs 3 tasks: prep 160 + 182 + 147 = **489 ms** of the 595 ms phase (Quent iter3,
    `task_list.py`).
  - With zero clone cost the phase would be about 143 ms (compute only), so **452 ms** is due to the
    exchange.
  - The same mechanism is behind P2 CONCAT(2): customer partitions gathered in 47 ms/task vs 10 ms total at
    g1, **+50 ms**.
- **Achievable split.**
  - The bandwidth floor of a perfectly pipelined exchange is ~175 ms. That is 27 GB moved, 2/3 of it
    cross-switch, so each uplink carries ~9 GB down at ~52 GB/s.
  - So the **mechanical fix** (batched or pipelined clones, one sync per source) is worth about
    **−315 ms**.
  - The **design fix** (no exchange; see R3) is worth **−452 ms**.

### F2. The CUDA driver's stream-ordered allocator is a process-global serialization point. **Q18 +353 ms** (PARTITION 216, scan/HGB 84, merge frees 53)

- **Mechanism.**
  - Every device allocation goes through `rmm::mr::cuda_async_memory_resource`
    (`cucascade/src/memory/memory_space.cpp:122-127`), i.e. `cudaMallocFromPoolAsync`/`cudaFreeAsync`.
  - The driver takes a process-wide lock: OSRT shows `pthread_rwlock_wrlock`/`pthread_mutex_lock` nested in
    these calls.
  - Microbench (`10-analysis/microbench/alloc_contention/results.txt`):
    - Aggregate throughput is **~280-350k calls/s whatever the GPU count**: 340k at 1 GPU x 4 threads,
      279k at 4 GPUs x 4 threads.
    - Mean latency is 10.9 µs at 1 GPU x 4 threads and **55.9 µs at 4 GPUs x 4 threads**.
    - Peer-access grants make no difference (grant=0/1 equal).
  - **So any phase whose cost is allocation-count-bound cannot scale with GPUs.**
- **Evidence: PARTITION(5) (`nsys_q18_ranges.txt`, `alloc_rate.py`).**
  - Launched kernels are identical: 188 ms at g1 vs 183 ms at g4. D2D 36 GB in both.
  - **Identical 77,748 malloc+free calls** (124 per task), at 4.2 µs mean at g1 vs **68.4 µs** at g4.
    That is 5316 thread-ms of PARTITION's 7346 thread-ms.
  - Process-wide alloc rate in the phase: 137k/s at g1, already near the ceiling. At g4 it is capped at
    **167k/s** (profiled).
  - Un-profiled phase wall: 433 / 316 / 313 / 324 ms at g1 / g2s / g2x / g4. This is **flat beyond 2 GPUs
    and topology-independent**, which is the signature of a global ceiling (~78k calls / 0.32 s ≈ 245k/s).
  - Allocation count comes from the 36 per-partition deep copies at `src/op/partition/gpu_partition_impl.cpp:212-221`
    (one `cudf::table` per slice).
- **Evidence: HGB and scan prepare, same mechanism.**
  - HGB range: malloc + free 212 → 1401 thread-ms (+1189), against a total HGB increase of +1166.
  - `sirius::compression::host_to_gpu`: allocator 234 → 1912 thread-ms (+1678), against a total increase
    of +1669.
- **Evidence: MERGE "after" segment.** cudaFreeAsync over the same 1261 frees per task takes 4.9 ms at g1
  and **51.2 ms** at g4.
- **Pricing (un-profiled):**
  - **C.** g4 thread-ms would fall from 5099 to the g1 level of 1696, giving 106 ms on 16 threads instead
    of 324. **−216 ms.**
  - **B.** Threads are saturated (3657 thread-ms / (16 x 234 ms) = 98%). Removing the HGB allocator excess
    (+882) and the prep excess (+401) gives 2374 / 16 = 148 ms. **−84 ms** (capped at Δ).
  - **D.** Free inflation on the critical thread is (74 − 33) + (57 − 33) ≈ 65 ms, minus wave-3 slack.
    **≈ −53 ms.**
  - Q13 is ~0: its allocator excess sits in link-bound phases.
- **Overlap with F1.** About 100 ms of F1's clone time is allocator calls inside clones. Fixing only the
  allocator therefore shrinks D by about 150 ms. The two are not additive beyond 452 + 53 in D.

### F3. Host-link ceiling (2 Gen5 x16 uplinks). **Q13 +340 ms, Q18 +121 ms**

- **Evidence.** nsys timeline (`nsys_timeline.py`):
  - **Q13 orders scan.** Host→GPU (HtoD plus pinned-src "PtoP"-classified copies) runs at
    **110.4-111.0 GB/s for 750 ms at g4** vs 47.6 GB/s at g1. Bytes are equal: 85.6 GB scan H2D at both
    (g1 99.8 GB HtoD minus 14.2 GB spill reload).
  - The microbench aggregate is 105-111 GB/s. g2s 1532 ms (1 uplink) vs g2x 869 ms (2 uplinks) confirms
    it.
  - Q18 P8/P17: 77-109 GB/s.
  - Q18 P0: customer is uncompressed, 3.9 GB → 37 ms (35 ms at 110 GB/s).
- **Pricing.** The Δ of each link-bound phase vs g1/4:

  | Phase | Δ |
  |---|---|
  | Q13 P2 | 340 ms (771 − 431) |
  | Q18 F + H | 102 ms |
  | Q18 P0 | 19 ms |

  This is hardware, so the full Δ is the price.
- **Lever: fewer bytes (M9).** `o_comment` is ~79 of Q13's 85.6 GB and is pinned identity. Snappy at ~2.5x
  (host_scan.md estimate) is −48 GB:
  - g4 scan becomes 37.6 / 110 ≈ 342 ms (−429 ms absolute).
  - g1 becomes about 790 ms.
  - So the gap shrinks by about **195 ms**, decode cost not counted.

### F4. Q13 DCJ gather: 13.8 GB at 4 partitions. **Q13 +190 ms**

- **Evidence.**
  - g4: 4 DCJ tasks, one per GPU. Each spends 185-195 ms in GPU-origin Preparing, then 69 ms compute
    (g1 alone-task compute is 66 ms, so no inflation).
  - nsys during the gather (≈ 800-1030 ms): kernels 0% busy; aggregate staged traffic is H2D 44 + D2H 44 +
    P2P 21 GB/s, far below the link.
  - 585 `convert:gpu_to_gpu` (390 host_staging, 723 ms; 195 peer_copy, 100 ms). Bytes: 9.2 GB staged and
    4.6 GB peer.
- **Pricing.** The gather is the phase's excess over compute: **190 ms**.
  - Mechanical (all 3 sources concurrent, no per-buffer syncs): uplink floor ≈ 4.6 GB per uplink direction
    at 50 GB/s ≈ 92 ms. **−100 ms.**
  - Design: the count join could pre-aggregate locally. Each GPU builds a 150M-slot histogram (600 MB) of
    its own `o_custkey` during the scan, then reduces 4 histograms (≈ 1.8 GB moved). **≈ −160 ms.**

### F5. Partition-count quantization. **Q18 +19 ms**

36 merge partitions (36 GB at `hash_partition_bytes` 1 GB) on 16 threads makes 3 waves instead of 2.25.
Wave 3 runs only 4 tasks (Quent iter3 1061-1255 ms). Priced as phase D minus clone minus free inflation
minus ideal: 595 − 452 − 53 − 71 = **19 ms**.

### F6 (measurement / g1-side design). Q13 g1 is inflated and bimodal because of a DENSE_COUNT_JOIN over-reservation. **Credits Q13's reported scaling by about +228 ms of ideal-g4**

- **Mechanism.** `sirius_physical_dense_count_join.cpp:498-512`: `no_history_peak_memory_estimate` takes
  `max(dense_peak, sparse_peak, ...)`.
  - `sparse_peak` = 8 x 16 B x min(planned groups 1.35B, bytes/8 = 575M) ≈ 74 GB per task. This holds even
    though the **dense** path is taken (log: `dense path: keys in [..] range 150M`).
  - Each of the 4 tasks therefore "needs" **91.8 GB** (clamped to the GPU tier; log
    `[downgrade][reserve] ... needed 91778265907 bytes`) for 4.6 GB of input.
- **At g1.** The 4 tasks share one GPU:
  - The first waits 176-263 ms while the downgrade spills 9.2 or 13.8 GB to host.
  - Tasks then run one at a time, the last two reloading from host (prepHOST 169 ms each).
  - The phase takes 619 ms (iter3), against an estimated ~264 ms if the 4 tasks ran concurrently: 2
    concurrent tasks took 131.7 ms each, which is GPU-bound at 2 x 66 ms.
- **At g4.** One task per GPU, so nothing waits (preparing 803.6 = routed 803.5).
- **Bimodality.** The 3.92 s hot iteration in both g1 runs is the first one that spills 13.8 GB. The host
  pinned pool then grows +4.77 GB:
  - Log `host_pool ... peak` 306.97 → 311.74 GB.
  - Free blocks 9646 → 14254.
  - Cause: `expand_pool` cudaHostAlloc under the pool mutex.
  - Iterations after that take 2.47 / 2.65 s.
- **Correction.**
  - Q13 g1 steady = 2.558 s (mean of the 4 non-growth hot iters across g1_a and g1_b), so 2.35x, not
    2.76x.
  - Suite g1 becomes 35.37 s, so ideal g4 is 8.84 s, not 8.96 s.
  - If the estimate were fixed, g1 Q13 would be about 2.10 s (1.93x).
  - Pricing: (3.011 − 2.10) / 4 = 228 ms of "free" ideal-g4 credit in the reported Q13 number.

### Experiments consulted

- **`x_g4_joinpol`** (max_build_hash_table_bytes derived + broadcast 8 GB):
  - Q13 1.097 vs 1.089 s.
  - Q18 **1.634** vs 1.562 s.
  - The Q18 plan is unchanged (`qf/x_g4_joinpol`: same 24 pipelines). Only C (348 vs 324) and B
    (249 vs 234) moved, i.e. variance in allocator-bound phases.
  - Join policy is **not a lever for this family**: no hash join is on the critical path.
- **The remaining flips** are covered in §0.1. Predictions from revision 1 vs measured:

  | flip | predicted | measured |
  |---|---|---|
  | hpb4g | Q18 −150 to −250 ms (C −200) | Q18 **−311 ms**: C −124, D −194 (9 tasks, 1 wave) |
  | hpb256 | Q18 about +300 ms | Q18 **+2110 ms**: C +579, D +1586. Underpredicted, because clones per merge task stay at 470 while tasks go 36 → 141. Q13 +708 ms (next bullet) |
  | thr8 | C worse | Q18 +258 ms: C +94, B +62, D +33. g1 is neutral |
  | gpupin | Q13 about 0.3 s, Q18 about −120 ms, F1/F2/F4 remain | Q13 0.633 s: scan −690 ms, DCJ unchanged. Q18 confounded by downgrade (PARTITION input arrives HOST-origin, 919 ms). Its link phases F/H fall 254 → 89 ms, and D stays 619 ms |
- **Q13 hpb256 (+708 ms) is a new DCJ hazard.** The DCJ becomes 12 tasks (3 per GPU). Each task requests
  38 GB, so only 2 run at once per GPU, and each still builds a dense table over the full 150M key range,
  because the layout comes from the per-task min/max of hash-partitioned keys
  (`sirius_physical_dense_count_join.cpp:590-594`).
  - Compute rises from 277 to 5051 thread-ms (concurrent pairs take 486-499 ms per task, vs 69 ms alone at
    baseline).
  - The cost is per partition, not per row. This is the same family of over-estimate as F6.
  - It is not active at baseline, so it is not priced in the baseline gap.
- **Steady g1 Q13 confirmed.** x_g1_batch4g (2.603/2.604/2.603) and x_g1_thr8 (2.655/2.653/2.650) have no
  slow hot iteration (the pool grows in iter1 instead: 6.3 s and 5.1 s). This supports the 2.56-2.65 s steady
  value used in F6.

## 3. Candidate remedies (ranked by family wall-ms at g4)

| # | Remedy | Targets | Predicted Δ at g4 (Q13 / Q18) | Effect at g1 |
|---|---|---|---|---|
| R1 | Per-device sub-allocator over the driver pool (rmm pool/arena per GPU, or per-thread caching) so steady-state alloc/free never take the driver's global lock. Also, or instead, cut allocation count in hash partition: one output allocation per column for all P partitions plus views, instead of P deep-copied tables (`gpu_partition_impl.cpp:212-221`) | F2 | −0 / **−353 ms** (likely large suite-wide: this is CRITIC N1) | small (+, 4 µs → ~1 µs) |
| R2 | Batch or pipeline remote clones: group a task's remote inputs by source GPU, issue all D2H then all H2D legs with one sync per leg per source, run sources concurrently, and preallocate a target buffer (`representation_converter_builtins.cpp:620-695, 898-960`) | F1, F4 | −100 / **−315 ms** | none |
| R3 | Design: orderkey/custkey-aware placement, so keyed exchanges are local. lineitem and orders are clustered by orderkey, so range-place pinned chunks and use a "local" partition layout. Q18's GROUP BY l_orderkey then needs no PARTITION→MERGE exchange. For the count join, use local histograms plus a reduce | F1, F4, part of F2-C | −160 / **−452 ms** (and C mostly disappears) | small |
| R4 | Compress `o_comment` (snappy) and customer for host pins | F3 | **−195** gap (−429 abs) / −19 | −0.9 s abs |
| R5 | Fix the DCJ estimate: count only the path that will run (dense when the gate passes), and divide planned rows by partition count (`sirius_physical_dense_count_join.cpp:498-512`). Pre-size the host pool so spills never `expand_pool` | F6 | 0 / 0 at g4 | **Q13 g1 −0.36 to −1.8 s**: honest baseline, lower variance |
| R6 | `hash_partition_bytes` so that merge partitions are a multiple of 16 (or ≤16) | F5 (+ fewer allocs) | 0 / −19 to −200 (**measured hpb4g −311**) | ± |
| R0 | **Config, measured:** `scan_task_batch_size` 4 GB, which gives 4.3x fewer fragments for every count-bound cost | count-bound parts of F1 and F2 | **+18 / −562 ms measured** | Q18 −237 ms, Q13 0 (steady). Q18 scaling 1.35 → 1.87x |

R1 and R2 overlap by about 100 ms on Q18's clone allocator calls. R0 harvests most of R1+R2 for Q18 by
cutting fragment count instead of per-fragment cost. With R0 applied, the remaining Q18 gap is 0.532 s:
- D bandwidth part ≈ 282 ms (needs R3).
- Link ≈ 110 ms (F/H).
- A ≈ 73 ms (customer CONCAT clone).
- The rest is spread across B and C.

## 4. Not pursued / not priced

- **N-forced partitioning (M4): not applicable.** Q18 uses 36 partitions and Q13 DCJ uses 4 at both g1
  and g4, so it is the same work.
- **Late accumulated Bloom (M10).** Q18's only accumulated Bloom (customer, 150M rows) is refused by the
  per-GPU cap (300 MB > 256 MiB) at both counts. No effect on scaling.
- **Copy_if/LIKE inflation in Q13's scan** (thread-ms 918 → 1632): hidden behind the link-saturated scan.
  Wall impact is ~0 while F3 holds.
- **Strictly sequential pipelines.** P0-P2 run before P3, and PARTITION(5) starts only when P3 completes, at
  both counts. Overlapping them would help g1 and g4 alike. Not a scaling item; not priced.
- **Q18 plan shape.** HASH_JOIN(17) builds on customer (150M rows) while its probe (orders filtered by the
  qualifying orderkey IN-list) is tiny. Building on the small side would remove phase A (101 ms at g4,
  113 at g1). This is plan quality, not scaling.
- **The "PtoP" memcpy rows with srcKind=Pinned** (`copyKind 10`, src device ≠ dst) are pinned-host → device
  copies whose host buffer belongs to another device's context, not real peer traffic. They are counted as
  H2D above.
- **CUPTI under-reports the allocator ceiling.** It is 167k/s profiled vs ~245k/s implied un-profiled.
  Prices use un-profiled Quent.
