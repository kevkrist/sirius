# Family 'subq' (TPC-H Q15, Q17, Q20): why these queries reach only 1.8-2.2x on 4 GPUs

Regime: SF1000, all tables pinned in host memory, commit bb557b1a, 4x RTX PRO 6000 (2 PCIe switches, one Gen5 x16 uplink each).
Absolute times come from un-profiled runs (00-baseline, hot mean of iters 2-4). Quent phase times are for iter3. nsys
(01-nsys/nsys2_{g1,g4}, iter2, restricted to the map.csv window) is used for proportions only.

Scripts and outputs are in `10-analysis/fam_subq/`:
- `pipe_table.py`: Quent per-pipeline accounting, two runs side by side.
- `h2d_timeline.py`, `h2d_timeline.txt`: host-to-device bytes over time, per switch.
- `alloc_phase.py`, `alloc_phase_q15.txt`: allocator calls per NVTX operator and per phase.
- `merge_clones.py`: cross-GPU clones inside each MERGE_GROUP_BY task.
- `api_latency.txt`: CUDA API latency, g1 vs g4.
- `nsys_{g1,g4}_q{15,17,20}.txt`: per-NVTX-range kernel vs wall breakdown.
- `qf_g2x/`, `qf_g2s/`: flattened Quent for the 2-GPU runs.

## Bottom line

The family's gap to linear is **1,130 ms** of the 11.08 s suite gap. About **71% of it (797 ms) is the host-link
hardware ceiling (M1)**.
- Q17 and Q20 are host-to-device bound for about 96% of their wall time at g4. The aggregate rate is 109.5 GB/s, which is
  54.8 GB/s per switch uplink.
- A natural experiment proves this. With 2 GPUs on separate switches (g2x), every GPU has its own uplink. The lineitem
  scans then speed up 1.97-1.99x, with **zero** per-task inflation (prepare thread-time 3,389 ms vs 3,431 ms at g1). With
  2 GPUs on the same switch (g2s) they speed up only 1.15x. At g4, the scan phases run at exactly 2x the g2s rate.

The rest is concentrated in Q15, which has a large group-by shuffle (4.5 GB of partials):
- **Driver allocator serialization**: 95 ms.
- **Serial fan-in of 470 fragment clones per merge task**: 88 ms.
- **5 partitions on 4 GPUs**: 11 ms.

Fixed per-query costs that do not scale (serial pipeline tails and planning or result overhead) add 97 ms across the
three queries.

## Time accounting

Wall times are un-profiled. "ideal" means g1/4. Phase times come from Quent iter3, measured from query init.

| | Q15 | Q17 | Q20 |
|---|---|---|---|
| g1 / g2s / g2x / g4 (s) | 1.296 / 1.094 / 0.811 / 0.713 | 1.838 / 1.603 / 0.948 / 0.835 | 1.616 / 1.429 / 0.862 / 0.769 |
| speedup g2s / g2x / g4 | 1.18 / 1.60 / **1.82** | 1.15 / 1.94 / **2.20** | 1.13 / 1.87 / **2.10** |
| ideal g4 / gap (ms) | 324 / **389** | 460 / **375** | 404 / **365** |
| per-GPU busy at g4 (Quent union, % of wall) | 661-682 ms (95-98%) | 792-801 ms (98-99%) | 702-719 ms (95-97%) |
| thread-time g1 → g4 (sum of task ms) | 4,967 → 9,402 (1.89x) | 7,084 → 12,283 (1.73x) | 6,093 → 10,263 (1.68x) |
| H2D bytes per query (nsys) | 48.9 GB | 85.3 GB | 74.7 GB |
| link floor at g4 (bytes / 109.5 GB/s) | 446 ms | 779 ms | 686 ms |
| outside-query overhead g1 / g4 (timings minus Quent wall) | 13.3 / 13.2 ms | 26.7 / 26.5 ms | 28.6 / 28.9 ms |

Per-phase breakdown (ms of wall; g1 → g4; excess = g4 − g1/4):

| Query | Phase (pipelines) | g1 | g4 | ideal | excess | What bounds it at g4 |
|---|---|---|---|---|---|---|
| Q15 | lineitem scan + PROJ + HASH_GROUP_BY (P0, 627 tasks) | 1,083 | 461 | 271 | **191** | H2D at 97 GB/s (floor 440), threads 99% occupied |
| Q15 | PARTITION(3), 627 tasks, P=5 | 98 | 98 | 25 | **74** | driver allocator, process-wide (see F2) |
| Q15 | MERGE_GROUP_BY(4), 5 tasks | 77 | 119 | 19 | **100** | 88 ms of clone prepare per task + GPU0 holding 2 of 5 |
| Q15 | tail: CTE, aggregates, supplier scan, join, sort (P3-P19) | 21 | 17 | 5 | 12 | serial chain of tiny pipelines |
| Q17 | part scan (P0) + 2 lineitem scans with DF (P3, P15) | 1,779 | 782 | 445 | **337** | H2D at 109 GB/s (floor 779) |
| Q17 | bubbles between scans (delim-join chain before P15) | 16 | 12 | 4 | 8 | dependency: P15's filter input |
| Q17 | tail (P16-P25) | 16 | 14 | 4 | 10 | serial chain |
| Q20 | part, partsupp, lineitem scans (P10, P13, P25) | 1,534 | 686 | 383 | **302** | H2D at 104-108 GB/s (floor 677) |
| Q20 | head plus bubbles between scans (nation and supplier, delim and DF chains) | 19 | 23 | 5 | 18 | 2-task supplier scan; dependency chains |
| Q20 | tail (P26-P39: CONCAT, joins, MERGE, ORDER_BY, RESULT_COLLECTOR 5.9 ms) | 33 | 31 | 8 | 23 | serial chain |
| all | outside-query overhead | 13/27/29 | 13/27/29 | 3/7/7 | 10/20/22 | fixed serial cost |

Reconciliation, as a sum of excesses vs measured gap: Q15 385 vs 389, Q17 375 vs 375, Q20 365 vs 365.

**Operator thread-time, g1 → g4** (Quent, summed over executor threads; 4 threads per GPU):
- Q15:
  - HASH_GROUP_BY(2): 896 → 1,829 ms (median 1.24 → 2.53 ms).
  - PARTITION(3): 366 → 1,510 ms (median 0.58 → 2.39 ms, **4.1x**).
  - GPU_SCAN(0): 847 → 1,384 ms.
  - MERGE_GROUP_BY prepare: 3 → 438 ms.
- Q17: DYNAMIC_FILTER(4): 635 → 1,054 ms. Scan prepare: 6,199 → 10,774 ms. All of it is hidden behind the saturated link.
- Q20: GPU_SCAN(28) post-processing: 1,449 → 2,398 ms. Scan prepare: 4,562 → 7,626 ms. Also hidden behind the link.

**nsys, NVTX operator ranges in the g4 Q15 window** (launched-kernel ms vs range wall ms):

| Range | g4 kernel / wall (ms) | g1 kernel / wall (ms) | Allocator time g4 / g1 |
|---|---|---|---|
| PARTITION | 21.3 / 1,937 | 21.6 / 475 | malloc 951 + free 558 = **78% of wall**, vs 23% |
| HASH_GROUP_BY | 102.6 / 2,432 | 107.7 / 1,129 | 1,432 vs 161 ms |

The GPU work is identical at g1 and g4. All of the inflation is on the host side.

## Findings (ranked by wall-ms share of the family gap at g4)

### F1. Host-link ceiling: 2 Gen5 uplinks for 4 GPUs (M1, hardware). **797 ms**: Q15 169, Q17 334, Q20 294. Confidence: high.

- **Mechanism.** Every lineitem, part and partsupp byte crosses a switch uplink on every query, because the tables are
  pinned in host memory. Two GPUs share each uplink.
- **H2D rate at g4** (`h2d_timeline.txt`): Q17 and Q20 hold **109.3-109.8 GB/s aggregate, 54.6-54.9 GB/s per switch**,
  in every 50 ms bin from 100 ms to the end of the last scan. The only exceptions are the inter-scan bubbles. Q15 holds
  97-103 GB/s during its scan.
- **H2D rate at g1**: uncompressed scans (part, partsupp) run at 55.0 GB/s, which is link-bound. The compressed lineitem
  scans run at 43-46 GB/s (84% of the link, because the 4 threads also decode).
- **Per-scan bytes and speedups (nsys):**

  | Scan | H2D | g1 | g4 | Speedup |
  |---|---|---|---|---|
  | Q17 P3 | 48.9 GB | 1,064 ms | 449 ms | 2.37x |
  | Q17 P15 | 30.9 GB | 672 ms | 284 ms | 2.37x |
  | Q20 P25 | 58.0 GB | 1,289 ms | 539 ms | 2.39x |
  | Q20 P10 | 8.15 GB | 148 ms | 76 ms | **1.95x** |
  | Q20 P13 | 8.00 GB | 146 ms | 77 ms | **1.9x** |

  The two uncompressed scans scale at exactly the link ratio.
- **Natural experiment (Quent, iter3)** for Q17 P3 lineitem scan span and prepare thread-time:

  | Run | Span | Prepare thread-time |
  |---|---|---|
  | g1 | 1,027 ms | 3,431 ms |
  | g2x | 515 ms | 3,389 ms (no inflation) |
  | g2s | 890 ms | 6,059 ms |
  | g4 | 446 ms | 5,965 ms |

  For Q20 P25 the spans are g1 1,241, g2x 630, g2s 1,063 and g4 533 ms. When each GPU has its own uplink, the scans
  scale about 2x per doubling. When GPUs share an uplink, the prepare (H2D wait) thread-time absorbs the whole loss.
- **Pricing.** For each scan phase, the share is (g4 bytes / 109.5 GB/s) − (g1 phase / 4):
  - Q17: 779 − 445 = 334.
  - Q20: 677 − 383 = 294.
  - Q15: 440 − 271 = 169.

  This is wall time, because those phases are on the critical path and the link is saturated.
- **Second-order effect.** Behind M1, Q17 and Q20 thread-time inflates 1.7x (prepare waits plus 1.66x DYNAMIC_FILTER).
  It becomes visible only if M1 is removed. It is not priced (see "Not priced").

### F2. Driver stream-ordered allocator is process-wide serialized: Q15 PARTITION phase is flat at about 98 ms on every topology (M2, mechanical/design). **95 ms**: Q15 74 + 21. Confidence: high for 74, medium for 21.

- **Mechanism.** Each GPU's memory space defaults to `rmm::mr::cuda_async_memory_resource`
  (`cucascade/src/memory/memory_space.cpp:123-125`), so every allocation is a driver `cudaMallocFromPoolAsync` or
  `cudaFreeAsync`.
- **The driver throughput ceiling is fixed per process, not per GPU** (`microbench/alloc_contention/results.txt`):

  | Configuration | Aggregate rate | Mean latency |
  |---|---|---|
  | 1 GPU x 4 threads | 340k calls/s | 10.9 µs |
  | 4 GPUs x 4 threads | 279k calls/s | 55.9 µs |
  | 4 GPUs x 4 threads, peer grants | 305k calls/s | 50.4 µs |

- **The engine matches the microbench exactly.** For Q15, cudaMallocFromPoolAsync averages 10.6 µs (g1) vs 51.2 µs (g4),
  and cudaFreeAsync 3.8 µs vs 45.4 µs, over 58k-63k calls (`api_latency.txt`). Inside the malloc and free calls, osrt
  `pthread_rwlock_wrlock` takes 728 ms and `pthread_mutex_lock` 605 ms (PARTITION ranges, g4).
- **Q15 PARTITION(3) (627 tasks, about 34 allocator calls per task, 21.3k calls in total):**
  - Phase wall is 98 / 83 / 96 / 98 ms at g1 / g2s / g2x / g4. Adding GPUs does nothing.
  - At g4 the phase runs at about 230k allocator calls/s un-profiled (82% of the 4-GPU ceiling). nsys shows 169k/s at g4
    vs 167k/s at g1.
  - The allocator takes 78% of PARTITION range wall at g4 vs 23% at g1. Kernel time is unchanged (21 ms).
- **Pricing.**
  - Phase excess 98 − 24.5 = **74 ms**. If PARTITION tasks cost their g1 0.58 ms, 627 tasks on 16 threads would take
    23 ms, recovering 75 ms.
  - In the scan phase, alloc-inflated HASH_GROUP_BY, GPU_SCAN and PROJECTION hold the threads (99% occupancy: 3,596
    prepare + 3,702 compute of 16 x 461 thread-ms). H2D then reaches only 97 GB/s, vs 109.5 in Q17 and Q20. That is
    461 − 440 = **21 ms** (medium confidence: the attribution to the allocator, rather than to thread count, is inferred
    from the nsys allocator shares: 1,432 ms of the HASH_GROUP_BY ranges, 626 ms of GPU_SCAN).
- **Q17 and Q20 are unaffected**: about 6 µs per call and few calls.

### F3. Serial fan-in: each MERGE_GROUP_BY task clones about 470 remote 1.4 MB fragments one at a time (M3/M5, design/mechanical). **88 ms** (Q15). Confidence: high.

- **Mechanism.**
  - `sirius_physical_grouped_aggregate_merge::get_next_task_input_data` (`src/op/sirius_physical_grouped_aggregate_merge.cpp:216-240`)
    drains all 627 per-scan-task fragments of a partition into one task.
  - Its Preparing phase calls `convert_gpu_to_gpu` per fragment (`cucascade/src/cudf/representation_converter_builtins.cpp:892-960`).
  - Each fragment costs a stream sync, per-column `reconstruct_column_p2p`, and a target sync.
  - Cross-switch pairs use `peer_copy_into` host staging (`:589-698`): D2H in 1 MB blocks, sync, H2D, sync.
- **Evidence.**
  - Quent: merge prepare is 87-89 ms per task at g4 vs 0.6 ms at g1. The input is 899 MB per partition, and
    `prepG` is 438 thread-ms.
  - nsys `merge_clones.py`: each of the 5 merge tasks runs 470-471 clones (157 `peer_copy` at 0.09-0.16 ms each, plus 314
    `host_staging` at 0.23-0.32 ms each). That is 92-132 ms per task, profiled.
  - The effective rate is 675 MB / 87 ms = 7.8 GB/s per task, against 43-54 GB/s staged or P2P link capability.
  - g2s (P2P only): 186 thread-ms prepare. g2x (all staged): 365. g4: 438.
- **Pricing.**
  - Gap share 88 ms (prepare, g4 − g1).
  - Recoverable is about **58 ms**. The bandwidth floor for the per-GPU incoming bytes is about 30 ms: GPU0 receives
    1.35 GB, 0.45 GB of it intra-switch at 53.7 GB/s, plus about 1.35 GB through each uplink at about 55 GB/s with the
    legs pipelined.

### F4. Partition count 5 on 4 GPUs: GPU0 runs 2 of the 5 merge tasks (M7, config/design). **11 ms** (Q15). Confidence: high.

- **Mechanism.** `natural_num_partitions` = ceil(4.5 GB / 1 GB) = 5 (`src/op/partition/partition_sizing.hpp:101-120`).
  Placement is `round_robin(count, gpus)` (`sirius_physical_grouped_aggregate_merge.cpp:212`), so partitions 0 and 4 both
  land on GPU0.
- **Evidence.** In iter3, the GPU0 merge tasks finish at 680.7-680.9 ms with 30 ms of compute. GPUs 1-3 finish at
  662-669 ms with 14-20 ms. In nsys, the two GPU0 tasks take 159-169 ms wall vs 123-129 ms for the others.
- **Pricing.** Merge compute critical path 30 ms vs g1/4 = 19 ms.
- **Test.** `x_g4_hpb4g` gives natural=2, floored to N=4, so P=4.

### F5. Serial pipeline tails and the inter-scan dependency bubbles (Amdahl). **82 ms**. Confidence: high.

- **Tails**: Q15 12, Q17 10, Q20 23 ms.
  - At g4, each query ends with a chain of 10-15 tiny pipelines (CONCAT, PARTITION, HASH_JOIN, MERGE, ORDER_BY,
    RESULT_COLLECTOR), each taking 0.2-6 ms.
  - The chains take as long at g4 as at g1 (Q20: 31.3 vs 32.8 ms; Q15: 17 vs 21; Q17: 14 vs 16).
  - Q20's RESULT_COLLECTOR alone is 5.8-6.0 ms at both counts.
  - At g4, each small exchange becomes PARTITION + 4-way CONCAT with clones. For example, Q20 CONCAT(31) takes 22.4
    thread-ms vs 4.2, and CONCAT(27) 25.1 vs 0.4. That is extra serial latency, not data-parallel work.
- **Bubbles**: Q17 8, Q20 18 ms, plus scan-below-floor residuals of Q17 3 and Q20 8 ms (29 ms of the 83, from the phase
  table).
  - Q17's second lineitem scan starts 10 ms after the first ends (503 → 513 ms). It waits for its DYNAMIC_FILTER input
    through the CONCAT(6) → HASH_JOIN(7) → RIGHT_DELIM_JOIN(9) chain, and the link idles: the 500 ms bin drops to 73 GB/s.
  - Q20 lineitem waits 8.9 ms for its 8.7M-row DF chain (166.8 → 175.7 ms; the 150 ms bin drops to 81.6 GB/s).
  - Q20's head is only 1.3x faster at g4 (11.4 vs 14.6 ms): the supplier scan is 2 tasks on 2 of 4 GPUs.
- **Accumulated Bloom publication is not involved.** Q20's publication is 0.2 ms after the final contribution (log), and
  Q17 publishes a 200,585-row non-accumulated filter.

### F6. Fixed per-query overhead outside the execution window (planning and result return). **52 ms**: Q15 10, Q17 20, Q20 22. Confidence: high (measured); cause not decomposed.

- **Evidence.** timings.csv minus Quent init→finished is 13.2 / 26.5 / 28.9 ms at g4 and 13.3 / 26.7 / 28.6 ms at g1.
  It is identical at both counts, and it is largest for the delim-join queries.
- **Suite-wide**: 447 ms at g4 vs 442 at g1, which is **336 ms** of the suite gap. This is cross-cutting, so the
  orchestrator should dedupe it.
- **Pricing.** g4 − g1/4.

### Family sum

| Finding | Q15 | Q17 | Q20 | Family | Share of family gap |
|---|---|---|---|---|---|
| F1 host link (hardware) | 169 | 334 | 294 | **797** | 71% |
| F2 allocator serialization | 95 | 0 | 0 | 95 | 8% |
| F3 merge fan-in clones | 88 | 0 | 0 | 88 | 8% |
| F4 P mod N | 11 | 0 | 0 | 11 | 1% |
| F5 serial tails + dependency bubbles + residual | 12 | 21 | 49 | 82 | 7% |
| F6 outside-query overhead | 10 | 20 | 22 | 52 | 5% |
| **Sum vs measured gap** | 385 / 389 | 375 / 375 | 365 / 365 | **1,125 / 1,130** | |

All entries are wall-ms on the query's hot mean. The suite total is the sum of query means, so these add directly to the
4-GPU suite total.

## Candidate remedies

| # | Remedy | Targets | Estimated g4 wall saving | Basis |
|---|---|---|---|---|
| R1 | Put each GPU on its own uplink: topology or hardware; or keep hot columns GPU-resident (`x_g4_gpupin`) | F1 | up to 797 ms (family) | g2x already shows 1.94x and 1.87x for Q17 and Q20 |
| R2 | **Q17: read lineitem once.** P3 and P15 both apply the same part filter (Brand#13 / JUMBO CAN, 200,585 parts). Reuse P3's DF-filtered rows (`l_partkey`, `l_quantity`, `l_extendedprice`) for the correlated avg instead of a second scan (a CTE-style shared scan, or a window-aggregate rewrite) | F1 bytes | **282 ms** at g4 (30.9 GB / 109.5 GB/s); 672 ms at g1 | P15 H2D bytes |
| R3 | Better host compression of the lineitem columns these queries read. Current ratio (decompressed Quent `in_bytes` / H2D) is only 1.47x (Q17), 1.34x (Q20) and 1.76x (Q15). For example, bit-pack `l_quantity` (1-50) and `l_partkey`/`l_suppkey` | F1 bytes | link floor scales with bytes: 1.5x fewer bytes gives about 260 (Q17) + 229 (Q20) + 147 (Q15) ms, provided GPU decode stays below about 46 GB/s per GPU of input | bytes / 109.5 |
| R4 | Per-GPU user-space sub-allocator (rmm pool or arena) over the async pool in `memory_space.cpp:123-125`, or fewer allocations per PARTITION task (34 today) | F2 | about 75 + 21 = **96 ms** on Q15 (more suite-wide) | g1 per-task cost on 16 threads |
| R5 | Fan-in: one packed transfer and one sync per source GPU, or a per-GPU local pre-merge before the exchange. For Q15, pre-merging 157 fragments per GPU cuts 4.5 GB of partials to about 0.8 GB, since there are 10M distinct `l_suppkey`, about 200 MB per GPU | F3, and F2 via fewer PARTITION rows | **58 ms** (bandwidth floor), more with pre-merge | per-GPU incoming bytes / link rates |
| R6 | Round the partition count up to a multiple of N (P=8 or P=4) | F4 | 11 ms | GPU0 compute 30 vs 19 ms |
| R7 | Shorten the serial tail: skip PARTITION/CONCAT for tiny inputs (below the small-table threshold, single-partition), cheaper RESULT_COLLECTOR | F5 | about 20-40 ms (rough: half of F5) | tail chains are 14-31 ms |
| R8 | Start the dependent lineitem scan without waiting for its DF chain (late-binding filter) | F5 bubbles | Q17 8 + Q20 9 ms | link idle bins |

## Experiments (20-experiments)

- `x_g4_joinpol` (done): Q15 0.721, Q17 0.838, Q20 0.771 s vs baseline 0.713 / 0.835 / 0.769. **No effect on this
  family**, as expected: the joins here are tiny after the dynamic filters, and the shuffles are group-by, not join.
- `x_g1_joinpol` (done): Q15 1.309, Q17 1.840, Q20 1.620 s. No change to the g1 denominator for this family.
- `x_g4_hpb4g` (done, hash_partition_bytes 4 GB). Q15 goes from P=5 to P=4 (natural 2, floored to N): **0.713 → 0.702 s
  (−11 ms), which is exactly the F4 estimate.** Q17 and Q20 are unchanged. Quent iter3 (`fam_subq/qf_hpb4g`):
  - PARTITION phase 98 → 88 ms (fewer partition slices, so fewer allocator calls).
  - MERGE phase 119 → 114 ms: 4 tasks, one per GPU.
  - Merge prepare per task is **unchanged at about 87 ms** (348 thread-ms / 4). F3's cost is per fragment, not per
    partition, as predicted.
- `x_g4_hpb256` (done, P=18): **Q15 0.713 → 0.972 s (+259 ms)**. Q17 and Q20 are unchanged. This confirms F2 and F3 scale
  with P x upstream tasks:
  - PARTITION phase 98 → 169 ms (comp 1,510 → 2,645 thread-ms).
  - MERGE phase 119 → 298 ms. There are 18 tasks of about 470 clones each, and prepare is 438 → 2,651 thread-ms.
- `x_g4_batch4g`, `x_g4_thr8`, `x_g1_thr8` and `x_g4_gpupin`: see the addendum below if present. Otherwise they were not
  available when this was written.

## Not priced

- **Compute inflation behind the link (Q17 and Q20).**
  - Q17: DYNAMIC_FILTER(4) 635 → 1,054 thread-ms; total compute 874 → 1,424.
  - Q20: GPU_SCAN(28) 1,449 → 2,398.
  - Prepare thread-time is 1.7x.
  - All of it is hidden while H2D is saturated, with 0 wall impact now. It becomes the next bottleneck after R1, R2 or R3.
- **g1 under-uses its own link on compressed scans** (43-46 vs 55 GB/s). This flatters the scaling ratio: a faster g1
  makes the gap bigger. `x_g1_thr8` would show it.
- **Overlap of PARTITION with the scan in Q15** (source-first priority, `src/creator/task_creator.cpp:233-238`). In
  principle it could hide F2's 98 ms phase under the link-bound scan, but with 4 threads per GPU fully occupied there is
  no free thread. Needs `x_g4_thr8`.

## Not pursued

- Accumulated Bloom (N3): Q20 at 0.2 ms, Q17 not accumulated. Irrelevant here.
- Join-mode confound (M8): the joinpol flip shows ±1%.
- Small-pinned slab mutex (sync H1): the PARTITION and HASH_GROUP_BY inflation is fully accounted for by
  cudaMallocFromPoolAsync/cudaFreeAsync time (1,399 of the 1,462 ms PARTITION range delta, about 96%). No residual is left
  for it in this family.
- GPU-side slowdown: launched-kernel ms is equal at g1 and g4 (PARTITION 21.6 vs 21.3; HASH_GROUP_BY 107.7 vs 102.6).

## Addendum: x_g4_batch4g and x_g4_thr8 (un-profiled hot means; Quent iter3 in fam_subq/qf_{batch4g,thr8})

| query | g4_a | batch4g (scan_task_batch_size 4 GB, 145 instead of 627 lineitem tasks) | thr8 (8 threads per GPU) |
|---|---|---|---|
| Q15 | 0.713 | **0.580 (−133 ms)** | **0.808 (+95 ms)** |
| Q17 | 0.835 | 0.824 (−11) | 0.836 (0) |
| Q20 | 0.769 | 0.767 (−2) | 0.772 (+3) |

**batch4g confirms F2 and F3 as per-fragment and per-task costs.**
- Q15 PARTITION(3) phase: 98 → **24 ms**, which is exactly ideal. There are 4.3x fewer tasks, so about 4.9k allocator
  calls instead of 21.3k, and PARTITION compute drops from 1,510 to 365 thread-ms.
- MERGE phase: 119 → 72 ms. Prepare per task drops from 87 to 48 ms (238 thread-ms / 5) with about 109 instead of 470
  remote fragments per task. The fan-in is now part latency, part bandwidth (675 MB / 48 ms = 14 GB/s).
- Scan phase: 461 → 451 ms. It is still link-bound.
- Realized saving on Q15 is 133 ms, against the F2 + F3 + F4 recoverable estimate of 75 + 58 + 11 = 144 ms.
- Q17 and Q20 barely move, because they are link-bound (F1).

**thr8 confirms that the contention is global and that F1 cannot be hidden with more threads.**
- Q15: the scan phase gets longer (461 → 518 ms) and so does PARTITION (98 → 121 ms). Compute thread-time goes from 5.3
  to 13.7 s, because 32 threads hammer the process-wide allocator.
- Q17: compute thread-time rises 1,424 → 6,334 ms (4.4x), yet wall is identical (809.6 vs 810.0 ms Quent) and the scan
  spans are identical to 0.3 ms, because the uplinks are saturated.

## Addendum: x_g4_gpupin (tables pinned in GPU memory, so no host link) and x_g1_thr8

| query | g4_a | g4 gpupin | Quent iter3 (gpupin) |
|---|---|---|---|
| Q15 | 0.713 | **0.583** (−130 ms) | scan phase 461 → 306 ms; **PARTITION 98 → 133 ms; MERGE 119 → 121 ms** |
| Q17 | 0.835 | **0.204** (−631 ms) | lineitem scans 446 + 282 → 74 + 54 ms; wall 810 → 177 ms |
| Q20 | 0.769 | **0.224** (−545 ms) | lineitem scan 533 → 129 ms; wall 740 → 194 ms |

- **F1 is confirmed as the whole story for Q17 and Q20.** With the link gone, they run 4.1x and 3.4x faster than g4_a,
  and well under the host-pinned ideal g1/4 (460 / 404 ms). There is no g1 gpupin run, so the gpupin scaling ratio is
  unknown.
- **Q15 at gpupin is dominated by F2 + F3.** PARTITION + MERGE are 254 of 573 ms (44%). PARTITION gets *slower* (133 ms)
  once the scan stops throttling the allocator. This is the next bottleneck for the family after F1.
- **x_g1_thr8**: 1.296 / 1.834 / 1.617 s, unchanged vs g1_a. The g1 denominator for this family is stable under more
  threads.

## Addendum 2 (18:4x): equal-config scaling, batch8g, best (Quent iter3 flattened in `fam_subq/qf_{g1_batch4g,batch8g,best}`)

Un-profiled hot means (s):

| query | g1_a | g4_a | g1 batch4g | g4 batch4g | g4 batch8g | g1 best | g4 best |
|---|---|---|---|---|---|---|---|
| Q15 | 1.296 | 0.713 | **1.119** | 0.580 | **0.554** | 1.118 | 0.574 |
| Q17 | 1.838 | 0.835 | **1.758** | 0.824 | 0.837 | 1.758 | 0.835 |
| Q20 | 1.616 | 0.769 | **1.552** | 0.767 | **0.843** | 1.547 | 0.786 |

- **batch4g also speeds up g1**, so part of its g4 win is not a scaling win.
  - Q15 at g1: 1,281 → 1,107 ms Quent wall. The scan phase drops 1,083 → 1,001, PARTITION(3) 98 → 28 and MERGE 77 → 57.
    So at g1 too, F2-type per-task costs exist: 627 small tasks against the per-device allocator.
  - Q17 and Q20 lineitem scans at g1: 1,027 → 994, 651 → 626 and 1,239 → 1,188 ms. With fewer, larger tasks, g1 gets
    closer to its link.
  - Equal-config scaling (batch4g at both counts): Q15 **1.93x**, Q17 **2.13x**, Q20 **2.02x**. Gaps at g4: 300 / 384 /
    379 = **1,063 ms**, vs 1,130 ms on the baseline config.
  - Batch4g removes 89 ms of Q15's gap but **adds** 9 + 14 ms to Q17 and Q20, because g1 improves more than g4 there
    (g4 is pinned at the link floor).
  - The F1 share is unchanged or slightly larger under batch4g. Q17's lineitem scans run 994 + 626 at g1 vs 444 + 281 at
    g4 (2.24x), which is link-bound: P3 prepare thread-time is 3,183 → 5,862.
- **Q15 at equal config (batch4g), phase excess at g4 vs g1/4:**
  - scan 451 − 250 = **201** (link floor 440, so about 190 of it is F1).
  - PARTITION 24 − 7 = **17** (F2 residual).
  - MERGE 72 − 14 = **58** (F3: prepare is 238 thread-ms over 5 tasks, vs 0.9 at g1. Compute is 84 vs 191 thread-ms, so it
    is all clone fan-in).
  - That leaves about 24 ms of tail and overhead.
- **batch8g regresses Q20 by +74 ms (P mod N on small tables, new):**
  - The partsupp scan GPU_SCAN(11), 8.1 GB, drops from 6 to **3 tasks on 4 GPUs**. Its span goes 80 → 118 ms, so one GPU
    and half of one uplink sit idle.
  - GPU_SCAN(14) also runs as 3 tasks (75 → 77 ms). The lineitem scan runs 73 tasks on 16 threads, with span 534 → 533.
  - Hot iterations are bimodal (0.864 / 0.805 / 0.861).
  - Q15 gains 26 ms because MERGE has 4 tasks (73 fragments each): prepare 238 → 184 thread-ms, phase 72 → 63.
  - Lesson: scan_task_batch_size must keep task count ≥ k·N for small tables. A fixed byte batch is not N-aware. The
    remedy is batch = min(cfg, table_bytes / (k·N)).
- **best (batch4g + joinpol)** matches batch4g within noise for this family (±20 ms). joinpol is irrelevant here, as in
  the baseline.
- **Unpriced observation (Q17): a staged clone competes with the scan on the saturated uplinks.**
  - CONCAT(11) is the 4-way gather of the delim-join output, about 0.1 GB. Its GPU-origin prepare is 51.7 thread-ms at
    g4_a and **506.7 thread-ms at g4 batch4g** (127 ms per task, span 506 → 686 ms).
  - It runs concurrently with the link-saturated second lineitem scan. This matches the P2P microbench "4 cross +
    H2D on all: 11 GB/s/pair".
  - It is off the critical path (P15 starts at 507 ms; the query ends at 797 ms), so the wall cost is 0. It does hold 4
    threads for about 127 ms.
- **nsys note for other analysts:** at g4, CUPTI logs the host-pinned scan uploads as `copyKind=10` (PtoP) with
  `srcKind=1` (pinned) and `srcDeviceId` 0-3, not as `copyKind=1`. Filtering on `copyKind=1` undercounts g4 H2D by
  4-90x (Q20: 0.85 vs 75.5 GB). The family H2D bytes above include kind 10 with srcKind=1: Q15 54.1 (this includes about
  3.4 GB of shuffle), Q17 85.5 and Q20 75.5 GB.

### Updated family pricing (baseline regime = canonical; equal-config batch4g in brackets)

| Finding | Q15 | Q17 | Q20 | Family (wall ms, g4 suite) |
|---|---|---|---|---|
| F1 host link (hardware, M1) | 169 [~190] | 334 [~340] | 294 [~300] | **797** [~830] |
| F2 allocator serialization (M2) | 95 [17] | 0 | 0 | 95 [17] |
| F3 merge fan-in clones (M3/M5) | 88 [58] | 0 | 0 | 88 [58] |
| F4 P mod N (M7) | 11 [11] | 0 | 0 [batch8g: +74] | 11 |
| F5 serial tails + dependency bubbles | 12 | 21 | 49 | 82 |
| F6 outside-query overhead (cross-cutting; dedupe) | 10 | 20 | 22 | 52 |
| **gap** | 389 [300] | 375 [384] | 365 [379] | **1,130 [1,063]** |
