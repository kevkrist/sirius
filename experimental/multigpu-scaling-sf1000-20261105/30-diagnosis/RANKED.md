# RANKED: why Sirius does not scale 1→4 GPUs (TPC-H SF1000, host-pinned, 4x RTX PRO 6000, bb557b1a)

Synthesis of every `10-analysis/*.md` report (X1 allocator, X2 exchange, X3 link, and 7 query families), the CRITIC
thread-time decomposition, and all 12 config-flip runs (all finished; the last, `x_g1_best`, at 17:39).

- Figure of merit: hot-mean suite total.
- g1 = 35.82 s. Ideal g4 = 35.82 / 4 = **8.96 s**.
- Measured g4 = **20.04 s**. Gap = **11.08 s** (1.79x instead of 4x).
- Every number is **wall seconds on the g4 suite**. Thread-time has been converted to wall time using the methods
  listed under "Waterfall construction".

Scripts in this directory (CPU only, re-runnable):
- `family_map.csv` maps each family finding to a root cause.
- `waterfall.py` aggregates that map and applies the adjustments.
- `link_check.py` checks the link share per query against gpupin.
- `outside_window.py` computes fixed per-query overhead outside the Quent window.

## Bottom line

1. **Hardware is a third of the gap: 3.5 s.**
   - Two Gen5 uplinks serve four GPUs, so host→GPU bandwidth only doubles (52 → 105 GB/s) while GPU count
     quadruples.
   - No software can get the g4 suite below about **12.4 s** at today's wire bytes. That is 2.9x, not 4x.
2. **Most of the rest is cross-GPU exchange cost that does not exist at 1 GPU: ≈5.5 s.**
   - It splits into four parts:
     - driver-allocator lock: 1.8 s;
     - per-fragment clone overhead: 1.7 s;
     - host-staged shuffle bytes: 1.2 s;
     - exchanges that are pass-through at N=1 or should be broadcasts: 0.8 s.
   - All four are made worse by the same multiplier: **627 scan tasks × P fragments**.
3. **1.2 s is link-bound scan time that a pipelined engine would hide.**
   - The next scan is created only after the previous exchange barrier.
   - The link sits idle through exchange phases (no multi-GPU prefetch).
4. **Smaller items (1.4 s in total):**
   - fixed per-query overhead: 0.34;
   - granularity and tails: 0.31;
   - Bloom host-staged publication: 0.30;
   - small-pinned cross-GPU event waits: 0.20;
   - measurement credits: −0.25.
   - **Unexplained ≈ 0.0 s.** The family decompositions close by construction. The uncertainty is in the bucket
     sizes, not in missing mechanisms: about ±0.8 s across buckets.
5. **What the experiments show:**
   - `scan_task_batch_size=4GB` (config only) closes **2.43 s of gap**: 20.04 → 16.91 at g4, and 35.82 → 33.04 at g1.
   - More threads make g4 worse: thr8 adds **+1.19 s** because of the allocator.
   - Removing the link (gpupin) saves **3.82 s** on 19 unconfounded queries.
   - After batch4g, the link is 46% of the remaining gap.

## Waterfall: ideal 8.96 s → measured 20.04 s

| # | Bottleneck (root cause) | Kind | Wall s | % of gap | Cumulative s |
|---|---|---|---|---|---|
| | **Ideal g4 = g1 / 4** | | | | **8.96** |
| 1 | Host-link hardware floor (2 uplinks / 4 GPUs) | hardware | 3.48 | 31.4% | 12.44 |
| 2 | Process-wide CUDA driver stream-ordered allocator lock | mechanical | 1.80 | 16.2% | 14.24 |
| 3 | Per-fragment cross-GPU clone overhead (serial fan-in of 627×P fragments, per-buffer syncs) | design + mechanical | 1.72 | 15.5% | 15.96 |
| 4 | Cross-switch shuffle bytes through serialized 2-leg host staging | design / topology | 1.23 | 11.1% | 17.19 |
| 5 | Link-bound scan phases not overlapped with link-idle exchange phases (no lookahead or prefetch) | design | 1.20 | 10.8% | 18.39 |
| 6 | Exchange policy: N-forced P≥N partitioning, shuffle where broadcast would do, redundant re-shuffle | design / config | 0.76 | 6.9% | 19.15 |
| 7 | Fixed serial per-query overhead outside the execution window | design (Amdahl) | 0.34 | 3.0% | 19.49 |
| 8 | Granularity: P mod N, scan-task quantization, single-GPU tails, barrier skew | config / design | 0.31 | 2.8% | 19.80 |
| 9 | Accumulated-Bloom publication: host-staged, synchronous, on the critical path | design | 0.30 | 2.7% | 20.10 |
| 10 | Small-pinned host pool shared across GPUs (cross-GPU pooled-event waits, H1) | design | 0.20 | 1.8% | 20.30 |
| 11 | Measurement credits: g1 Q13 warm-up and pool growth (−0.23), join-mode confound (−0.02) | measurement | −0.25 | −2.2% | 20.04 |
| | **Unexplained residual** | | **0.00** | 0.0% | **20.04** |

### Waterfall construction

**Source.** Seven family analyses partition the 22 queries. Each family decomposed its own gap
(g4 − g1/4 per query) on the critical path, and each decomposition closes to within 3 ms. The family gaps are
1.59 + 1.31 + 2.24 + 2.73 + 1.37 + 0.71 + 1.13 = 11.08 s.

`family_map.csv` maps all 61 family findings onto the root causes above. That bottom-up mapping gives:

| Root cause | Bottom-up sum (ms) |
|---|---|
| LINK | 4682 |
| ALLOC | 1698 |
| FRAG | 2015 |
| BYTES | 1229 |
| POLICY | 763 |
| BLOOM | 298 |
| GRAN | 306 |
| FIXED | 238 |
| MEAS | −246 |
| residual | 100 |

**Four adjustments**, each justified by a cross-cutting measurement:

1. **LINK is split into a hard floor (3.48) and LINK_SERIAL (1.20).**
   - The families price the link per phase: Σ max(0, phase bytes / 105 GB/s − g1 phase / 4).
   - X3 prices the per-query hard floor: Σ_q max(0, scan bytes_q / 105 − T1_q/4) = 3.48 s.
   - The difference, 1.20 s, is the ideal duration of non-scan phases. A perfect engine would hide that under the scan
     link time. This engine runs those phases strictly after the scan (X3 L2).
   - X3 independently bounds this by gap-filling: Δgap ≤ 1.65 s, realistic ≈ 1.0 s.
   - `link_check.py` checks the family link price against the gpupin per-query delta. It caps each query at what
     removing the link actually saved, wherever gpupin is not confounded. That gives 4.26 s in place of 4.68. So
     LINK_SERIAL is somewhere between 0.78 and 1.20 s.
2. **0.30 s moves from FRAG to ALLOC.**
   - X1 measures task-end `cudaFreeAsync` inflation at 0.63 s profiled, about 0.47 s un-profiled.
   - Those frees land at the end of consumer (CONCAT/MERGE) tasks. The families' batch4g dose-response fits booked
     that time as clone overhead.
3. **0.20 s is carved out of ALLOC as H1.**
   - X1 measures it: 11.2 thread-s of cross-device pooled-event waits, 0.36 s profiled, scaled ×0.57-0.92.
   - The families had booked it as part of "per-op inflation".
4. **FIXED is set to the suite-wide measured 0.336 s.**
   - That is `outside_window.py`: out_g1 442 ms, out_g4 447 ms.
   - Four families booked only 0.238 s of it. The missing 0.098 s was sitting in the other families' residuals, so the
     residual falls to 0.002 s.

**Thread-time → wall conversions used upstream:**
- X1/X2: wall share = ∫ 1/n_active dt, with link-saturated bins capped.
- Families: critical-path phase excess (g4 phase − g1 phase/4).
- nsys-derived prices are scaled profiled→un-profiled by ×0.75 (allocator; 0.57-0.92) and ×0.92 (suite: 20.04/21.78).

## Ranked table

"Share" is the primary, additive attribution from the waterfall. "Lever" is what fixing the cause alone would save. It
is not additive across rows, because the causes compete for the same phases.

| Rank | Bottleneck | Kind | Share s (%) | Confidence | Lever, measured or estimated | Top queries | Best remedy (complexity) |
|---|---|---|---|---|---|---|---|
| 1 | Host-link floor | **hardware** | 3.48 (31%) | high | gpupin −3.82 s on 19 clean queries (ΔT4) | Q19, Q17, Q8, Q20, Q12, Q21, Q5, Q7 | Per-GPU uplink (hardware). Under it: compression coverage, Δgap 0.5 / ΔT4 1.1 (low); shared repeated scans ≈0.2-0.5 (medium); GPU-resident hot columns (medium, needs the gpupin downgrade bug fixed) |
| 2 | Driver allocator lock | mechanical | 1.80 (16%) | high | X1: 2.4 s (1.8-2.9) at g1 per-call cost; sub-allocator ΔT4 3.0 (2.3-3.7); thr8 **+1.19 measured** | Q18, Q1, Q21, Q9, Q4, Q12, Q8, Q10, Q5, Q15 | Per-GPU sub-allocator (rmm pool/arena) under the reservation adaptor (medium). Zero-copy partition slices, ≈0.6 (medium). batch4g (config) |
| 3 | Per-fragment clone overhead | design + mechanical | 1.72 (15%) | high | batch4g: exchange excess −2.09 s **measured** (shared with #2/#6); hpb256 **+3.62 measured** | Q9, Q8, Q5, Q12, Q21, Q18, Q1, Q4, Q16, Q22, Q15 | `scan_task_batch_size=4GB` (config, measured). Per-GPU coalescing before exchange plus one packed transfer per (src, dst) (medium-high). Local pre-merge (medium) |
| 4 | Cross-switch shuffle bytes, 2-leg staging | design / topology | 1.23 (11%) | medium | Pipelined clones ≈1.4 after batch4g, 2.2 at baseline (est., includes part of #3); orderkey co-location ≤0.5 more | Q9, Q18, Q13, Q21, Q3, Q10, Q14 | Overlapped D2H/H2D legs, single sync per clone (medium). orderkey range-pinned co-location (high). Switch-aware hash placement = 0 (do not pursue) |
| 5 | Serialized pipelines leave the link idle | design | 1.20 (11%) | medium | X3 L2 Δgap ≈1.0 (≤1.65) | Q21, Q18, Q10, Q9, Q8, Q7, Q5, Q4 | Lookahead creation / eager independent scans, multi-GPU prefetcher (high) |
| 6 | Exchange policy (forced P≥N, shuffle vs broadcast, re-shuffle) | design / config | 0.76 (7%) | medium-high | joinpol **−0.97 measured** (blanket), −1.40 oracle at baseline; −0.50 oracle after batch4g; forced exposure 2.40 → 0.76 at best | Q3, Q10, Q21, Q12, Q5, Q8, Q4 | Cost-aware broadcast rule P > (N−1)·B plus per-fragment latency (low-medium). Drop the P≥N floor for small inputs (low). Partitioning-property propagation (medium) |
| 7 | Fixed per-query overhead | design (Amdahl) | 0.34 (3%) | high | ≤0.34 | all; Q21 29, Q18 23, Q20 22, Q17 20 ms | Plan cache / faster bind (medium). Not a scaling lever |
| 8 | Granularity / tails | config / design | 0.31 (3%) | medium | hpb4g Q15 −11, Q18 −311 measured; batch8g regressions | Q16, Q2, Q20, Q15, Q18, Q14, Q1 | P a multiple of N; per-table scan batch ≥4N tasks (low) |
| 9 | Bloom host-staged publication | design | 0.30 (3%) | high | ≈0.3 | Q8, Q21, Q10, Q5, Q9, Q4, Q16, Q12 | Hierarchical (P2P in switch + one host leg), asynchronous, off the barrier task (low-medium) |
| 10 | Small-pinned pool across GPUs (H1) | design | 0.20 (2%) | medium | 0.2-0.3 | suite-wide (device_scalar) | Key small-pinned pools per GPU (low) |

## Reconciliation with the thread-time decomposition (CRITIC §0)

Thread-time grows from 132 to 252 s (+120) for identical work. The wall effect differs per bucket because thread-time
only becomes wall time when it is on the critical path and not hidden under a saturated link.

| CRITIC thread bucket (g1→g4) | Where it went (measured split) | Wall bucket here | Why wall ≪ thread |
|---|---|---|---|
| HOST prepare +52.3 s | link waits +29.3 (StreamSync +23.2, BatchAsync +6.1); allocator +18.0; memcpy issue +3.2 (allocator write-lock collateral) (X3 L5) | #1 / #5 link; #2 (decompress class 0.42 s wall) | Prepare overlaps a saturated link. Quent starve is only 0.18 s per switch, and 4.7-7 prepares are in flight per switch |
| Compute: slower identical work +30.8 s | allocator lock (X1: 65 thread-s total excess across prepare, compute and frees) + H1 11.2 thread-s; kernels flat (Q18 PARTITION 188 vs 183 ms; Q1 HGB 903 vs 857 ms); clocks 2290-2360 MHz both | #2, #10 | 16 threads share the lock. X1 bins convert 65 thread-s → 3.18 s profiled → 2.4 un-profiled |
| Compute: new forced partition work +15.9 s | 75-80% allocator inside PARTITION (nsys); the rest is hash/slice/launch | #2 (alloc part) + #6 (rest) | Runs as a post-scan tail with 16 threads |
| GPU-origin prepare (clones) +25.1 s | count-bound (dose-response vs fragments) + byte-bound (62-97% of sync wait is the clone's own DMA) | #3 + #4 (+ part of #6: Q3 probe shuffle) | Fan-in consumers run 1 task per GPU, so it is nearly 1:1 on the critical path |
| HASH_JOIN −3.9 s | join-mode confound (Q3 STANDARD at g1) | #11 credit | — |
| Occupancy 0.943 → 0.805 | 1-task-per-GPU consumers, P mod N, Bloom stalls, tails | #3, #8, #9 | — |

Cross-check with X2: the exchange family measures 5.15 s of excess. Here #3 + #4 + #6 + the PARTITION part of #2
(about 1.2 s per X1) = 1.72 + 1.23 + 0.76 + 1.2 = 4.9 s, plus the Bloom stalls that sit inside PARTITION tasks.
These agree within 5%.

Cross-check with CRITIC's counterfactual: removing compute inflation, clones and forced partitioning gives about
14.0 s, so ≈6.0 s. Here #2 + #3 + #4 + #6 + #10 = 5.7 s.

## Reconciliation with experiments (experiments beat estimates)

| Run | g1 | g4 | x | Same-config gap | Δgap vs baseline | What it tests | Verdict |
|---|---|---|---|---|---|---|---|
| baseline | 35.82 | 20.04 | 1.79 | 11.08 | — | — | — |
| g2s {0,1} / g2x {0,2} | — | 29.40 / 22.21 | 1.22 / 1.61 | — | — | 1 vs 2 uplinks at N=2 | **#1 confirmed.** Same GPU count, +7.19 s from sharing an uplink. g4 has the same aggregate link as g2x (105 vs 102 GB/s), so the link-bound Q2/11/17/19/20 gain only 3.51 → 3.19 s from g2x to g4 |
| x_g4_gpupin (no host link) | — | 18.26 (sd 1.3) | — | — | — | removes #1/#5 | **#1 confirmed.** Clean 19 queries 16.34 → 12.51 (−3.82). Q17, Q19 and Q20 fall *below* T1/4. Q1, Q3 and Q18 are polluted by 9,675 cross-device downgrade errors (a bug) |
| batch4g (4× fewer scan tasks, same bytes) | 33.04 | 16.91 | 1.95 | 8.65 | **−2.43** | #2, #3, #6 per-task terms | **#2 and #3 confirmed.** PARTITION thread 30.0 → 7.3 s, exchange excess 5.15 → 3.05. Link-bound Q17/19/20 unchanged. Per-query Δ vs X1 allocator price: r = 0.96 |
| batch8g | — | 17.34 | — | — | — | granularity | **#8 confirmed.** Too few tasks: Q2 +115, Q11 +71, Q20 +74 ms vs batch4g |
| thr8 (8 threads per GPU) | 35.55 | 21.23 | 1.67 | 12.34 | **+1.26** | #2 vs latency hiding | **#2 confirmed.** Per-query regressions track the allocator price (r = 0.96). Microbench: 104 µs per call at 4×8 at the same ~300k/s |
| joinpol (broadcast 8 GB) | 34.74 | 19.07 | 1.82 | 10.39 | −0.69 | #6 | **#6 confirmed.** Q3/5/8/9/12 −0.2 to −0.4 s at g4 only. Q7 +0.15 and Q14 +0.07 regress. g1 Q3 flips mode (−0.87) |
| best (batch4g + joinpol) | 31.93 | 16.92 | 1.89 | 8.94 | −2.14 | #6 after #3 | **Overlap.** Blanket joinpol adds 0 on top of batch4g. Oracle per query 16.41 (−0.50) |
| hpb256 (4× more partitions) | — | 23.66 | — | — | +3.62 | #3 dose | **#3 confirmed.** Clone thread 25.1 → 47.8 s for the same bytes |
| hpb4g | — | 19.91 | — | — | −0.13 | #8 merge waves | Q18 −0.31 (waves and fragments), Q9 +0.11 |

g4-only flips: Δgap is taken as ΔT4, assuming g1 does not change.

Where the analyses disagree, the experiments decide:
- **Link: families 4.68 s vs X3 hard floor 3.48 vs gpupin 3.82.** This is resolved by splitting #1 and #5. The
  phase-level gpupin drops (multijoin scan phases −285 to −517 ms; Q12 P0 637 → 313) support the per-phase prices.
- **Allocator: families 1.70 vs X1 2.4.** Primary attribution here is 1.80, the low end of X1. The X1 lever (2.4) is
  larger because part of the allocator time is co-bound with the link: removing it alone saves less than its X1
  price in link-saturated phases. thr8 on Q10 shows this co-binding (lineitem scan 106 → 82 GB/s).
- **CRITIC N1 / "per-op compute inflation" is not a GPU effect.**
  - Kernels are flat and clocks are not throttled.
  - It is the allocator (88% of Q1 HGB inflation) plus H1.
  - The gpupin Q1 HGB is still 10.57 ms with no link at all (g1 4.84).
  - batch4g cuts Q1 HGB inflation from 2.08x to 1.09x.
- **CRITIC N4 (shared stream pool) is refuted:** no foreign kernels ran on any clone stream.
- **"Staging contends with scan H2D" is refuted:** 37 ms of overlap over the whole suite.

## 1. Host-link hardware floor (HARDWARE): 3.48 s, 31.4%

**Mechanism.**
- One NUMA pinned pool. GPUs 0+1 sit behind switch A and 2+3 behind switch B, each with **one** Gen5 x16 uplink.
- Microbench H2D is 52 GB/s for 1 GPU and for {0,1}, 102 GB/s for {0,2}, and 105 GB/s for all four.
- So the bytes move at most 2.0x faster while GPU count rises 4x.
- Scan bytes are unchanged: 1254 GB at g1 and 1249 GB at g4.
- Path: `src/compression/compressed_representation.cpp:62-86` (lineitem/orders, 1 MiB `cudaMemcpyAsync` blocks) and
  raw `cudaMemcpyBatchAsync` for the 4 uncompressed tables.

**Evidence.**
- Per-switch rate while busy is 48-55 GB/s, i.e. line rate.
- Per-switch link-busy time is 25.5 s at g1 and 11.97 s at g4 (2.13x).
- dmon: g4 per-GPU rx p90 is about 30 GB/s, which is the per-GPU cap.
- Link-bound queries scale at the link ratio: Q17 2.20x, Q19 2.18x, Q20 2.10x, with link busy 90-95% at both counts.

**Arithmetic.**
- Σ_q max(0, scan bytes_q / 105 − T1_q/4) = **3.48 s**. The largest terms are Q19 0.39, Q17 0.36, Q8 0.33, Q20 0.32,
  Q12 0.23, Q21 0.21, Q5 0.20 and Q7 0.20.
- Upper check: gpupin −3.82 s on 19 clean queries.
- The minimum g4 suite at today's bytes is 12.43 s (13.40 s including staged shuffle bytes).

**What software can still do under it.**
- **Cut bytes.**
  - Enable the 4 disabled table plans: −131 GB.
  - `o_comment` → snappy: −49 GB (`plans/tpch_sf1000/orders.txt:50` is a GPU-tier identity override applied to a host
    pin).
  - `l_shipmode` → dictionary: −54 GB (`lineitem.txt:74` str_split).
  - Together: −233 GB = 19% of wire bytes. ΔT4 ≈ 1.1 s, Δgap ≈ 0.5 s (0.35-0.87). Complexity: low (plans plus
    re-pin).
- **Scan repeated tables once per query.** Q2 and Q11 scan partsupp twice; Q22 scans customer twice; in Q17, P3 and
  P15 rescan lineitem with the same filter. Worth ≈0.2 s in the small family and ≈0.28 s on Q17. Complexity: medium.
- **GPU-resident hot columns.** gpupin shows the potential, but only after fixing the cross-device downgrade defect
  ("stream belongs to CUDA device N", 9,675 errors). Complexity: medium.
- **Hardware.** One uplink per GPU, or a C2C host, removes ≈3.5 s.

**Floor after all software.** After batch4g the link is 4.01 of the 8.65 s same-config gap (46%). That is why 4x is
unreachable on this box.

## 2. CUDA driver stream-ordered allocator lock (MECHANICAL): 1.80 s, 16.2%

**Mechanism.**
- Every device allocation follows this path:
  cucascade `reservation_aware_resource_adaptor::do_allocate_unmanaged`
  (`cucascade/src/memory/reservation_aware_resource_adaptor.cpp:462-475`)
  → rmm `cuda_async_memory_resource`, the default factory (`cucascade/src/memory/common.cpp:551-556`,
  `memory_space.cpp:122-127`)
  → `cuMemAllocFromPoolAsync` / `cuMemFreeAsync`.
- Those driver calls take a process-wide rwlock (write) and a mutex.
- There is no caching layer. Aggregate throughput is capped at about 280-385k calls/s whatever the GPU count, so 16
  executor threads queue where 4 did.
- Biggest allocation source: hash PARTITION deep-copies every slice per column (`src/op/partition/gpu_partition_impl.cpp:212-221`,
  also `:251-256`).

**Evidence.**
- Suite (nsys iteration 2):
  - malloc 911k → 1.197M calls, mean latency 8.0 → 35.4 µs.
  - free 2.9 → 29.8 µs.
  - Locks nested inside these calls are 63.3 of 78.1 thread-s at g4, vs 2.4 of 10.0 at g1.
- Latency depends only on the number of callers in flight, on the same curve at g1 and g4.
- The allocator is busy 53% of g4 wall vs 20% at g1.
- Microbench: 10.9 µs mean at 1 GPU × 4 threads, 56 µs at 4×4, 104 µs at 4×8. Pool peer grants have no effect.
- Q18 PARTITION(5) makes an identical 77.7k calls at both counts, at 4.2 vs 68.4 µs, with identical kernels. Its
  phase takes 433/316/313/324 ms at g1/g2s/g2x/g4, flat beyond 2 GPUs, which points to a global ceiling.

**Arithmetic.**
- Family critical-path prices:

  | Family | ms |
  |---|---|
  | scanagg | 369 |
  | joinship | 120 |
  | semijoin | 476 |
  | multijoin | 270 |
  | groupby | 353 |
  | small | 15 |
  | subq | 95 |
  | **Total** | **1.70 s** |

- Adjustments: +0.30 s of task-end frees moved from #3, and −0.20 s H1 carved out (→ #10). Result: **1.80 s**.
- X1 suite-wide price is 2.4 s (1.8-2.9), via 3.18 s profiled × 0.57-0.92.

**Remedies.**
- **R1: per-GPU user-space sub-allocator** in front of `cuda_async`, below the reservation adaptor, at the seam
  `reservation_manager_configurator::set_gpu_memory_resource_factory`
  (`cucascade/include/cucascade/memory/reservation_manager_configurator.hpp:103`).
  - Expected ΔT4 ≈ 3.0 s (2.3-3.7), plus 0.9-2.1 s at g1, so Δgap ≈ 2.4-2.8.
  - Complexity: medium.
  - Watch cross-stream reuse, fragmentation vs `capacity_bytes`, and the OOM retry path (`:440-460`).
- **R2: zero-copy partition views** or `contiguous_split`. Worth ≈0.6 s and a subset of R1. Complexity: medium.
- **batch4g** cuts the call count about 4x (measured).
- **Do not raise `num_threads`** until R1 lands (thr8 costs +1.19 s).

## 3. Per-fragment cross-GPU clone overhead (DESIGN + MECHANICAL): 1.72 s, 15.5%

**Mechanism.**
- Each of the 627 scan-sized producer tasks (1 GB `scan_task_batch_size`) emits P fragments. Placement is
  `gpu_ids[p%N]` (`src/op/partition_placement.hpp:58-70`).
- The consumer then clones about 470 remote fragments **one at a time** in Preparing:
  `sirius_physical_operator.cpp:140-190` → `batch_lock_utils.hpp:123-140` → `convert_gpu_to_gpu`
  (`cucascade/src/cudf/representation_converter_builtins.cpp:892-960`).
- Each clone syncs at entry and exit. Host-staged legs go D2H → sync → H2D → sync per leaf buffer (`:589-697`), and
  strings add a readback (`:786-815`). That is 6.4 syncs per staged clone, and 27 for Q1 partials.
- Nothing pre-combines fragments per GPU, and only 1 consumer task per partition runs, so 3 of 4 threads idle.

**Evidence.**
- Q1 MERGE_GROUP_BY prepare: 134-136 ms at g4 vs 3 ms at g1 (470 clones, 0 GB/s, kernels idle).
- Q18 MERGE prepare: 27 → 6,077 thread-ms.
- Q12 CONCAT(3): 470 converts, 3,768 syncs per task.
- Dose-response:
  - batch4g (627 → 145 fragments, same bytes): Q12 CONCAT(3) 109 → 28.5 ms, i.e. 0.167 ms per fragment; Q8 CONCAT
    138 → 33; Q1 merge 148 → 42.
  - hpb256: clone thread 25.1 → 47.8 s.

**Arithmetic.**
- Family prices:

  | Family | ms |
  |---|---|
  | scanagg | 256 |
  | joinship | 184 |
  | semijoin | 434 |
  | multijoin | 639 |
  | groupby, count part | 253 |
  | small | 161 |
  | subq | 88 |
  | **Total** | **2.02 s** |

- −0.30 s of frees moved to #2 gives **1.72 s**.

**Remedies.**
- `scan_task_batch_size=4GB`: config, measured. Combined with #2/#6 it gives Δgap −2.43, of which exchange −2.09.
  Use a per-table batch size so the small family does not regress (+41 ms).
- Code (same effect, decoupled from scan size): coalesce each GPU's producer output before hashing, then send one
  packed transfer per (src, dst). Complexity: medium-high.
- Batch each clone with one sync. Complexity: medium.
- Local pre-merge before fan-in (Q1/Q6/Q15/Q18 partials). Complexity: medium.

## 4. Cross-switch shuffle bytes via serialized 2-leg host staging (DESIGN / TOPOLOGY): 1.23 s, 11.1%

**Mechanism.**
- P2P exists only within a switch (0↔1, 2↔3). The 8 cross pairs host-stage through `peer_copy_into`.
- Hash shuffles are uniform, so 66.6% of the 176.6 GB moved per suite is staged: 117.6 GB H2D + 117.6 GB D2H.
- Staging costs 3.6x the per-byte cost of a peer copy, because the legs never overlap and each buffer syncs.

**Evidence.**
- 33,667 staged clones carried 117.6 GB in 21.7 thread-s; 16,830 peer clones carried 59.0 GB in 3.55 thread-s.
- 62-97% of the sync wait is the clone's own DMA. In Q3, 4,978 of 5,123 ms.
- The clone phase reaches about 30 GB/s per switch vs 45-52 GB/s capacity.
- Exchange runs as a kernel-idle phase after the scan: only 37 ms of overlap with scan H2D.

**Arithmetic.**
- Family prices:

  | Family | ms |
  |---|---|
  | scanagg Q14 build | 70 |
  | joinship Q10 customer join | 77 |
  | semijoin F2b | 292 |
  | multijoin bandwidth floor | 351 |
  | groupby bandwidth part (Q18 MERGE bytes + Q13 DCJ gather) | 439 |
  | **Total** | **1.23 s** |

- This overlaps X3 L3 (link-byte part, ≈0.4 s), which is already inside it.

**Remedies.**
- Overlap the two legs: issue all D2H, record an event, make the destination stream wait on it, sync once. Or use
  `cudaMemcpyBatchAsync` with double-buffered bounce blocks.
- Take string sizes from the source column.
- Worth ≈1.4 s after batch4g (2.2 s at baseline). This lever also absorbs part of #3. Complexity: medium.
- Range-pin lineitem and orders by orderkey per switch (key co-location): ≤0.5 s beyond that. Complexity: high.
- **Switch-aware hash placement is worth 0 s.** Under any bijection, 2/3 of moved bytes still cross a switch. Do not
  pursue it.

## 5. Link-bound scans serialized with link-idle exchange phases (DESIGN): 1.20 s, 10.8%

**Mechanism.**
- `task_scheduler.cpp:209-221` schedules only `scans.front()`.
- Creation is demand-driven, `strategy=active` and `priority=source` (`src/creator/config.hpp:101,106`), so the next
  scan's tasks appear only after the previous PARTITION/CONCAT barrier.
- `memory_prefetcher` is skipped when more than one GPU is eligible (`sirius_scan_manager.cpp:2152-2157`).
- So the link idles through every exchange, and the exchange phases are longer at g4.

**Evidence.**
- Per-switch link-idle time is 10.25 s at g1 and 8.05 s at g4 (only 1.27x shorter).
- On the busier switch at g4: 4.61 s of interior gaps of 5 ms or more (95 gaps), plus a 4.42 s tail.
- Quent starve is only 0.18 s per switch, so no scan work exists during the gaps.
- Q12: the orders scan is created at 885 ms, after PARTITION/CONCAT ran from 636 to 885 ms.

**Arithmetic.**
- Family per-phase link prices total 4.68 s, minus the hard floor of 3.48 s, gives **1.20 s**.
- `link_check.py` gives a lower bound of 0.78 s when each query is capped by its gpupin delta.
- X3 gap-fill bound: Δgap ≤ 2.5 − 3.4/4 = 1.65 s, realistic ≈ 1.0 s.
- Largest per query: Q21 0.83, Q18 0.40, Q10 0.36, Q9 0.26 (bounds, profiled).

**Remedy.** Lookahead creation or eager scheduling of independent scans, plus a multi-GPU-capable prefetcher that
converts host chunks once a task latches its device. Worth ≈1.0 s. Complexity: high (scheduler and memory headroom).

## 6. Exchange policy: work that does not exist at N=1 (DESIGN / CONFIG): 0.76 s, 6.9%

**Mechanism.**
- `partition_sizing.hpp:86-120` floors P at N once the input is at least 16 MB × N.
- BUILD_PROBE uses `min(natural, N)` partitions (`sirius_physical_hash_join.cpp:873`). At N=1 that means
  pass-through (`sirius_physical_partition.cpp:158`).
- Broadcast needs the build under 256 MiB with probe/build ≥ 1.25N, and rows × 16 B under 2 GB
  (`sirius_physical_hash_join.cpp:824-912`).
- `wrap_hash_group_by` (`sirius_physical_plan_generator.cpp:1425-1475`) re-shuffles even when the input is already
  partitioned on a group key.

**Evidence.**
- Forced-class exchanges: g4 PARTITION thread 16.0 s vs 0.13 s at g1; excess 2.40 s at baseline.
- Q3 probe shuffle: 51.4 GB, of which 25.7 GB is staged. joinpol takes Q3 from 1.056 to 0.655 s.
- Q10 re-shuffle: P8 is 138 ms at g4 vs 143.5 at g1, i.e. zero speedup.

**Arithmetic.** This bucket holds only the **non-allocator, non-clone** remainder: Q3 probe shuffle 260, joinship
forced 39 + re-shuffle 129, semijoin 149, multijoin 171, small 15. Total **0.76 s**.
- Its lever is larger because removing an exchange also removes its #2/#3/#4 cost:
  - joinpol measured −0.97 s blanket at baseline g4, −1.40 oracle;
  - after batch4g, the oracle is −0.50;
  - residual forced exposure at best is 0.76, of which ≈0.4-0.6 is recoverable.

**Remedies.**
- A cost-aware broadcast rule: broadcast when P > (N−1)·B, plus a per-fragment latency term. This avoids the Q7 (4.5 GB
  build) and Q14 (6.5 GB) regressions. Complexity: low-medium.
- Fix the gates that keep the Q10, Q21 and Q4 joins shuffled (the estimate ratio; unverified).
- Drop the P≥N floor when the consumer total is under about 1 GB. Complexity: low.
- Partitioning-property propagation to skip the Q10 re-shuffle (≈−0.1 s). Complexity: medium.
- Use equal join policy at both GPU counts when reporting scaling.

## 7. Fixed per-query overhead outside the execution window (AMDAHL): 0.34 s, 3.0%

- Timings minus the Quent window: 442 ms (g1) vs 447 ms (g4) over the suite. That is parse/bind/optimize, stats hooks
  and result return, identical at both counts.
- Per query it is 13-38 ms (Q21 38, Q18 31, Q20 29).
- Gap share = 447 − 442/4 = 336 ms (`outside_window.py`).
- Remedy: a plan cache for repeated SQL (medium). This is not a multi-GPU defect.

## 8. Granularity, P mod N, tails (CONFIG / DESIGN): 0.31 s, 2.8%

- Items:
  - Q15 has 5 merge partitions on 4 GPUs: 11 ms (hpb4g confirms −11).
  - Q18 has 36 merge tasks on 16 threads, i.e. 3 waves: 19 ms.
  - Scan-task quantization on small or uncompressed tables: 43 ms.
  - Single-GPU serial tails (merge top-N, sort, NLJ): 19 ms.
  - Subq tails and dependency bubbles: 82 ms.
  - Q1 scan barrier skew: 19 ms. Q14 1-task-per-GPU join tail: 18 ms.
  - Part-scan quantization: 25 ms. Small-pipeline floor: 40 ms. Per-task launch inflation on tiny inputs: 16 ms.
- Round-robin chunk placement is balanced to 0.2% (0.04 s; included).
- Remedies (all low complexity):
  - Make P a multiple of N.
  - Scan batch = min(cfg, table bytes / (k·N)), so that batch8g's "3 tasks on 4 GPUs" cannot happen.
  - Run tiny single-partition tails without a cross-GPU gather.

## 9. Accumulated-Bloom publication: host-staged, synchronous (DESIGN): 0.30 s, 2.7%

**Mechanism.**
- `sirius_dynamic_bloom_filter.cu:1080` chooses HOST staging for the whole publication if *any* pair lacks pool access.
  At g4 the 8 cross pairs always lack it.
- The staged loop is serial (`:1171-1207`) and runs inside the last build-side PARTITION task, which holds the
  barrier (`dynamic_filter_publisher.cpp:381-432`).
- At g1 the build is single-partition and publishes directly.

**Evidence.**
- Logged stalls after the final contribution: Q4 22, Q5 11+19, Q8 12+38, Q9 34, Q10 21+45, Q12 12, Q16 13.6,
  Q21 29+28 ms.
- Q21's finish is 93% stream sync.
- Unchanged under batch4g. joinpol halves it (60 → 28 publications).
- Q2's replicate also queues 21 ms behind 575 MB scan chunks on the same copy engine.

**Arithmetic.** Critical-path stalls: joinship 63 + semijoin 92 + multijoin 123 + small 20 = 0.30 s. X2-H prices only
the extra-probe-byte path (≤0.05) and is not additive with this.

**Remedy.** Hierarchical publication (P2P inside each switch, then one host leg), asynchronous and off the barrier
task. Complexity: low-medium.

## 10. Small-pinned host pool shared across GPUs (H1) (DESIGN): 0.20 s, 1.8%

- With one NUMA node, one pool serves all GPUs (`sirius_context.cpp:1056-1083`;
  `small_pinned_host_memory_resource.cpp:166-185`).
- A slab's `ready_event` can sit on another GPU's stream, so `device_scalar` → `make_pinned_vector` → `sync_stream`
  waits on cross-device work.
- Measured: 91k cross-device pooled-event syncs at g4, 7.0 thread-s, mean 77 µs, 917 of them ≥1 ms; plus 6.1 thread-s
  with work. **Zero at g1.**
- Price: 0.36 s profiled × 0.57-0.92 = 0.2-0.3 s.
- Remedy: key pools per GPU at `sirius_context.cpp:1060-1073` / `numa_small_pinned_mr::node_for_current_device`.
  Complexity: low.

## 11. Measurement credits: −0.25 s

- **g1 Q13 is not warm.** Iterations take 5.03/3.92/2.47/2.65 s, because of host-pool growth (+4.77 GB) on a spill
  that only g1 makes: a DENSE_COUNT_JOIN over-reservation of 91.8 GB per task
  (`sirius_physical_dense_count_join.cpp:498-512`).
  - This inflates T1, so ideal g4 is too high: credit −0.228 s.
  - Steady g1 is 35.37 s, ideal g4 8.84 s, and the true gap 11.20 s.
- **Q3 join-mode confound.** Q3 runs STANDARD at g1 vs BUILD_PROBE at g4 (2 GB `max_build_hash_table_bytes`).
  - multijoin books −0.018 s for it. In Q3 it hides 0.18 s of gap.
  - Honest Q3 scaling is 1.78x, not 1.92x.

## Remedy roadmap (stacked, overlap-aware; estimates unless marked measured)

| Step | Remedy | Complexity | g4 suite s | Basis |
|---|---|---|---|---|
| 0 | baseline | — | 20.04 | measured |
| 1 | `scan_task_batch_size=4GB` (per-table, ≥4N tasks for small tables) | config | 16.91 | **measured** (same-config gap 8.65) |
| 2 | Cost-aware broadcast per join | low-medium | ≈16.4 | **measured oracle** 16.41 |
| 3 | Per-GPU sub-allocator (+ zero-copy partition slices) | medium | ≈15.8 | X1: batch4g + R1 ≈ 16.0-16.5 |
| 4 | Overlapped 2-leg clones, one sync per clone, per-source batching | medium | ≈14.4 | X2 (B) 1.4 s after batch4g |
| 5 | Residual forced-partition gates / P≥N floor | low-medium | ≈13.9 | X2 (D) 0.4-0.6 |
| 6 | Scan lookahead / multi-GPU prefetch | high | ≈12.9 | X3 L2 ≈1.0 |
| 7 | Bloom hierarchical/async + per-GPU small-pinned pools + P mod N | low | ≈12.4 | #8 + #9 + #10 ≈ 0.5-0.8 |
| — | **Hardware floor at today's bytes** | — | **12.4** | X3: Σ max(floor, T1/4) |
| 8 | Compression coverage (4 tables, `o_comment`, `l_shipmode`) | low | ≈11.3 | X3 L4 ΔT4 1.1 (the floor drops to about 10.5) |
| 9 | One uplink per GPU | hardware | — | gpupin bound −3.8 |

- Several remedies also speed up g1 (batch4g −2.78, sub-allocator −0.9 to −2.1, compression −2.8 to −4.5). So the
  achievable *ratio* on this box is about 2.3-2.6x, not 4x.
- **Do not:**
  - raise `num_threads` (+1.19 s);
  - use `hash_partition_bytes` 256 MB (+3.62 s);
  - use a blanket 8 GB broadcast on top of batch4g (Q7/Q14 regress);
  - use `scan_task_batch_size` 8 GB (+0.43 s vs 4 GB);
  - invest in switch-aware hash placement (0 s).

## Not priced / measurement hygiene

- **Allocator write-lock collateral on memcpy issue.** rdlock 0.46 → 4.6 thread-s; cudaMemcpyAsync 5.8 → 11 µs with the
  allocator in flight. Folded into #2.
- **Second non-allocator coupling in Q6 UNGROUPED_AGGREGATE / GPU_SCAN.** Still 1.76x inflated at batch4g and 1.58x
  under gpupin. Hidden under the link (0 wall today); candidate is H1.
- **1 MiB host blocks** hurt only g1 (47.7 vs 52 GB/s). Fixing them *widens* the gap by about 0.4 s. Not a scaling
  cause.
- **gpupin defect.** 9,675 `[downgrade] convert failed ... stream belongs to CUDA device N` errors plus 2,524 column
  count mismatches (Q1/Q3/Q18), and memory-pressure regressions (Q10 +0.37). This is a correctness and robustness bug
  to file.
- **CUPTI labelling.** CUPTI labels 43-75% of pinned-host→device bytes at g4 as copyKind 10 (PtoP, srcKind=1).
  Classify memcpys by srcKind/dstKind.
- **hpb256 exposes a Q13 DENSE_COUNT_JOIN hazard.** Each partition builds a full 150M-slot table (+0.71 s).
- **Refuted or zero:**
  - GPU clock throttling (2290-2360 MHz at both counts);
  - shared clone stream pool (N4);
  - exchange vs scan uplink contention (37 ms overlap);
  - user-space mutexes (0.31 → 0.95 thread-s);
  - chunk-to-switch imbalance (0.04 s);
  - per-task prepare starving the link at g4 (0.18 s per switch).
