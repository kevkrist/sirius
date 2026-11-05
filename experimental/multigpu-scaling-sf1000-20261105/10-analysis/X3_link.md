# X3: host-to-GPU link and the scan path, 1 GPU vs 4 GPUs (TPC-H SF1000, host-pinned)

Scope: cross-cutting analysis of the PCIe host link. All runs are at commit bb557b1a. No GPU work was run for this note;
it uses existing data only.
- Scripts and CSVs: `10-analysis/X3/`.
- nsys data: `01-nsys/nsys2_{g1,g4}` warm iteration 2. Every event is restricted to the map.csv
  `[range_start_ns, range_end_ns]` window.
- Quent data: `00-baseline/{g1_a,g4_a}/quent` (hot iterations 2-4) and `01-nsys/nsys2_g4/quent`.
- Absolute times: un-profiled hot means from `g1_a` / `g4_a`. Gap = T4 − T1/4 = 20.04 − 8.96 = **11.08 s**.
- Profiled-to-unprofiled factor: 20.04/21.78 = 0.92 at g4 and 35.82/36.86 = 0.97 at g1. It is used where an nsys
  wall time is turned into a wall-time price.

## 0. Bottom line

1. **The link explains about a third of the gap: 3.6 s, range 3.48-3.82 s. It is hardware.**
   - The link gets 2.13x faster from g1 to g4, not 4x: per-switch link-busy time is 25.5 s at g1 and 11.97 s at g4.
   - Lower bound: even a perfect engine cannot beat the per-query floor of max(T1/4, scan bytes / 105 GB/s). Summed
     over the suite, that floor is 12.43 s, which is 3.48 s above linear.
   - Upper bound, measured with **x_g4_gpupin** (no host link at all): on the 19 clean queries, g4 drops from 16.34 to
     12.51 s, a **−3.82 s** saving. Q17, Q19 and Q20 drop *below* T1/4: 0.20, 0.47 and 0.22 s against ideals of
     0.46, 0.52 and 0.40.
2. **Shuffle traffic staged through the host adds about 0.4 s at g4 through link bytes (design).**
   - It carries +117.6 GB H2D and 117.6 GB D2H over the same uplinks.
   - Most of it is in Q3 (27 GB), Q18 (20), Q9 (19), Q21 (13), Q10 (9.5) and Q13 (9.2).
   - It mostly lands in post-scan phases, when the link is otherwise idle. Its latency cost is priced under M3, not
     here.
3. **The other half of the gap is time when the link is idle, and that time barely shrinks.**
   - Per-switch link-idle time is 10.25 s at g1 and 8.05 s at g4 (un-profiled-equivalent), only 1.27x faster.
   - At g4, scan work is never queued while the link is idle: Quent "starve" is 0.18 s per switch.
   - So the per-task synchronous prepare does **not** starve the link at g4. Four threads per GPU keep 5-6 HOST
     prepares in flight per switch. The suspected intra-pipeline overlap defect is refuted.
   - The idle link comes from **strict pipeline serialization**: the next scan is created only after the previous
     PARTITION/CONCAT barrier, which leaves 4.6 s of interior gaps of 5 ms or more, plus post-scan tails of 4.4 s.
   - A cross-pipeline H2D prefetch could recover at most 2.5 s at g4 (Δgap ≤ 1.65 s). The realistic estimate is about
     1 s of gap.
4. **Compression coverage (config) is worth about 0.5 s of gap and about 1.1 s of g4 time** (priced with the
   per-query link sensitivity measured by gpupin).
   - The levers are: enable the 4 disabled table plans (−131 GB), `o_comment` snappy (−49 GB), and `l_shipmode`
     dictionary (−54 GB). Together that is −233 GB, 19% of scan wire bytes.
5. **Measurement facts the other analyses need:**
   - **3/4 of host-to-GPU bytes appear in nsys as `Peer-to-Peer`**: copyKind 10, srcKind=Pinned, with
     srcDeviceId set to another GPU's context. Any H2D total keyed on `copyKind=HtoD` undercounts g4 by about 75%.
     These copies run at the same rate as the true HtoD ones (29.9 vs 30.0 GB/s in Q3), so the label is an
     artifact, not a staging path.
   - The +52 s of "HOST prepare" thread-time is only about 57% link waiting. About 35% is allocator calls
     (cudaMallocFromPoolAsync/cudaFreeAsync) and about 6% is memcpy issue.

6. **Update 18:35. Once software fixes land, the link becomes the main constraint** (section 6).
   - With batch4g on both counts (g1 33.04 s, g4 16.91 s), the hardware floor share is 4.01 s of an 8.65 s gap (46%).
   - With "best" (batch4g plus joinpol: g1 31.93 s, g4 16.92 s), it is 4.23 s of an 8.94 s gap (47%).
   - The best achievable g4 suite on this box with today's bytes is about 12.2-12.4 s (2.6-2.9x). Linear (4x) cannot
     be reached here without cutting bytes.

## 1. Facts: bytes, rates and busy fractions

### How host bytes are copied

- Host blocks come from `numa_region_pinned_host_memory_resource`. It uses `cudaHostRegister` / `cudaHostAlloc` with
  Portable|Mapped (`cucascade/src/memory/numa_region_pinned_host_allocator.cpp:34-66`), registered under whichever
  GPU context is current.
- CUPTI therefore reports a host-to-GPU copy as `PtoP` (kind 10, srcKind=1 Pinned, dstKind=2) whenever the
  registering context differs from the destination GPU.
- **Classify host-to-GPU copies by srcKind/dstKind, never by copyKind.** In g4 rep.4 (Q5 and Q6 iterations 1 and 2),
  pinned-to-device bytes were 34.4 GB as HtoD and 101.9 GB as PtoP.
- Compressed scan copies are one `cudaMemcpyAsync` per 1 MiB host block per leaf buffer
  (`src/compression/compressed_representation.cpp:62-86`). Raw tables use `cudaMemcpyBatchAsync`.

### Per-query table (`X3/per_query_table.md`; generated by `link_per_query.py`, `floors.py`, `interior_scan2.py`)

| Q | T1 s | T4 s | T1/4 | gap s | H2D g1 GB | H2D g4 GB | of which staging | link floor g4 (scan / +stg) s | hw share s | link-busy/switch g1 / g4 | g4 scan-copy / interior idle / tail ms |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 2.46 | 1.09 | 0.61 | 0.47 | 45.4 | 45.4 | 0.0 | 0.44 / 0.44 | 0.00 | 39% / 36% | 448 / 638 / 258 |
| 2 | 0.59 | 0.37 | 0.15 | 0.22 | 30.2 | 30.6 | 0.3 | 0.29 / 0.30 | 0.15 | 93% / 75% | 275 / 63 / 29 |
| 3 | 2.03 | 1.06 | 0.51 | 0.55 | 45.0 | 72.3 | 27.2 | 0.44 / 0.70 | 0.00 | 47% / 66% | 419 / 70 / 632 |
| 4 | 0.82 | 0.62 | 0.21 | 0.42 | 25.7 | 27.8 | 1.4 | 0.26 / 0.27 | 0.05 | 65% / 37% | 258 / 213 / 278 |
| 5 | 1.25 | 0.89 | 0.31 | 0.58 | 52.3 | 56.4 | 3.2 | 0.51 / 0.54 | 0.20 | 86% / 54% | 491 / 129 / 357 |
| 6 | 1.04 | 0.47 | 0.26 | 0.21 | 39.9 | 39.9 | 0.0 | 0.38 / 0.38 | 0.12 | 80% / 77% | 390 / 86 / 40 |
| 7 | 1.47 | 0.75 | 0.37 | 0.39 | 59.1 | 62.5 | 3.3 | 0.57 / 0.60 | 0.20 | 83% / 76% | 548 / 120 / 94 |
| 8 | 1.80 | 1.20 | 0.45 | 0.75 | 79.1 | 82.5 | 2.0 | 0.78 / 0.80 | 0.33 | 89% / 59% | 733 / 180 / 401 |
| 9 | 2.99 | 1.76 | 0.75 | 1.01 | 94.9 | 114.7 | 18.7 | 0.93 / 1.12 | 0.19 | 64% / 59% | 876 / 296 / 705 |
| 10 | 1.73 | 1.20 | 0.43 | 0.76 | 58.3 | 69.9 | 9.5 | 0.58 / 0.67 | 0.15 | 68% / 51% | 566 / 440 / 313 |
| 11 | 0.39 | 0.25 | 0.10 | 0.15 | 19.3 | 19.9 | 0.6 | 0.20 / 0.20 | 0.10 | 89% / 76% | 181 / 16 / 50 |
| 12 | 1.70 | 1.07 | 0.43 | 0.64 | 68.0 | 69.0 | 0.6 | 0.66 / 0.67 | 0.23 | 82% / 56% | 646 / 429 / 110 |
| 13 | 3.01 | 1.09 | 0.75 | 0.34 | 99.8 | 94.8 | 9.2 | 0.83 / 0.92 | 0.07 | 73% / 79% | 776 / 3 / 324 |
| 14 | 1.49 | 0.85 | 0.37 | 0.48 | 56.9 | 60.2 | 3.3 | 0.55 / 0.58 | 0.18 | 75% / 62% | 524 / 95 / 282 |
| 15 | 1.30 | 0.71 | 0.32 | 0.39 | 48.9 | 51.2 | 2.2 | 0.48 / 0.50 | 0.16 | 76% / 61% | 471 / 349 / 4 |
| 16 | 0.46 | 0.30 | 0.11 | 0.19 | 15.4 | 17.1 | 1.3 | 0.16 / 0.17 | 0.04 | 62% / 53% | 147 / 64 / 94 |
| 17 | 1.84 | 0.84 | 0.46 | 0.38 | 85.3 | 85.4 | 0.1 | 0.82 / 0.82 | 0.36 | 94% / 95% | 778 / 24 / 22 |
| 18 | 2.11 | 1.56 | 0.53 | 1.03 | 44.6 | 64.6 | 20.0 | 0.43 / 0.62 | 0.00 | 43% / 36% | 429 / 1413 / 21 |
| 19 | 2.10 | 0.96 | 0.52 | 0.44 | 94.7 | 94.8 | 0.0 | 0.92 / 0.92 | 0.39 | 90% / 91% | 884 / 66 / 26 |
| 20 | 1.62 | 0.77 | 0.40 | 0.36 | 74.7 | 75.1 | 0.3 | 0.72 / 0.72 | 0.32 | 93% / 91% | 685 / 29 / 40 |
| 21 | 3.24 | 1.99 | 0.81 | 1.18 | 103.5 | 118.1 | 12.9 | 1.02 / 1.14 | 0.21 | 66% / 52% | 974 / 910 / 284 |
| 22 | 0.39 | 0.24 | 0.10 | 0.15 | 12.8 | 14.5 | 1.6 | 0.12 / 0.14 | 0.03 | 64% / 57% | 118 / 61 / 54 |
| **Σ** | **35.82** | **20.04** | **8.96** | **11.08** | **1254** | **1367** | **117.6** | **12.08 / 13.22** | **3.48** | 71% / 60% | 11617 / 5696 / 4418 |

Column notes:
- **link floor:** the busier switch's bytes / 52 GB/s, where 52 GB/s is the per-switch aggregate from the microbench.
- **hw share:** max(0, min(scan floor, T4) − T1/4).
- **link-busy:** the union of all host-to-GPU copy intervals on the busier switch, divided by the nsys query wall.
- **last column:** from big (≥256 KiB) scan copies only.
  - interior idle = (first to last scan copy) − copy union.
  - tail = last scan copy to query end.

### Suite totals (nsys iteration 2, profiled)

| metric | g1 | g4 |
|---|---|---|
| H2D bytes (srcKind=Pinned/Pageable → Device) | 1254 GB | 1367 GB (scan 1249, host-staged shuffle 117.6) |
| D2H bytes | ~0 | 124.0 GB (staging 117.6) |
| Intra-switch device-to-device P2P | 0 | 59.3 GB |
| Bytes per GPU | 1254 | 341.4 / 342.5 / 341.8 / 341.0 |
| Bytes per switch | - | 683.97 / 682.75 GB (balanced to 0.2%) |
| Link-busy per GPU (union / wall) | 71.3% | 52.8% (each GPU) |
| Link-busy per switch / any GPU | 71.3% | 59.8% / 66.5% |
| Rate while busy, per GPU | 47.7 GB/s | 29.7 GB/s |
| Rate while busy, per switch | 47.7 | 48-55 GB/s (that is line rate: microbench 52) |
| Average rate over wall, per GPU | 34.0 GB/s | 15.7 GB/s |
| Link floor, bytes / BW | 1254/52 = 24.1 s | aggregate 1367/105 = 13.0 s; per switch 684/52 = 13.15 s; scan only 1249/105 = 11.9 s |

dmon, 1 s samples over the query phase (`X3/dmon_summary.py`; runs `00-baseline/g{1,4}_noquent`):

| | rx mean | rx p50 | rx p90 | rx max | other |
|---|---|---|---|---|---|
| g1, GPU0 | 36.8 GB/s | 48.5 | 51.4 | 59.9 | SM 93% |
| g4, per GPU | 15.8-19.0 GB/s | 17-28 | 29.7-29.9 | 32-34 | SM 72-78%, 141-145 W |

- At g4 the aggregate mean is about 70 GB/s, 67% of 105.
- Clocks are 2290-2360 MHz in both runs, so there is no throttling at g4. That is relevant to N1 in CRITIC: clocks
  are not the cause.
- The two sources agree: nsys measures 60-67% link busy at g4, and dmon's p90 sits at the per-GPU share of the cap.

### Compressed vs decoded bytes

Decoded bytes come from Quent HOST-task `input_bytes`, the estimated decoded size (`gpu_pipeline_task.cpp:729`). Wire
bytes are nsys copies attributed to each `Pipeline K` task range and then to its table through the log's
`assigned pinned entry` lines (`X3/table_bytes.py` → `X3/g4_table_bytes.csv`).

| table (suite, iteration 2) | wire GB | decoded GB | ratio |
|---|---|---|---|
| lineitem | 922.6 | 2165.9 | 2.35x |
| orders | 153.7 | 308.4 | 2.01x (Q13 1.11x: `o_comment` raw) |
| partsupp / part / customer / supplier | 62.4 / 56.8 / 40.1 / 4.1 | same | 1.00x (no plan) |
| shuffle staging (non-scan pipelines) | 127.0 | - | - |

- Without compression the g4 link floor would be about 2637/105 = 25 s. Compression already halves it.
- Per-query lineitem ratios range from 1.45x (Q20) and 1.47-1.56x (Q17, dominated by `l_partkey` at 1.14x and
  `l_extendedprice` at 2.66x) up to 4.4-4.6x (Q10, Q18).

## 2. Findings, ranked by priced wall-time share of the g4 suite

Pricing conventions:
- **ΔT4** is the saving on the 4-GPU suite.
- **Δgap** = ΔT4 − ΔT1/4. A lever that also speeds up g1 closes less of the gap than it saves at g4.
- **Link sensitivity s:** the measured g2s→g2x data shows wall sensitivity to link time of about 0.63.
  - Doubling the link from 52 to 104 GB/s cut the per-switch copy time from about 24.1 s to about 12.6 s.
  - Wall fell from 29.40 s to 22.21 s, so 7.19/11.5 ≈ 0.63.
  - This is a lower bound, because g2x also pays host staging that g2s does not.

### L1. Host-link ceiling: 2 Gen5 uplinks for 4 GPUs (hardware). Gap share 3.6 s (range 3.48-3.82), confidence high

**Mechanism.**
- Every scan byte comes from the single NUMA node's pinned pool.
- GPUs 0 and 1 share switch A's x16 uplink, and GPUs 2 and 3 share switch B's.
- Aggregate H2D is 52 GB/s for 1 GPU and 105 GB/s for 4 (microbench), so the link scales 2x while the ideal is 4x.

**Evidence.**
- Scan bytes are identical: 1254 GB at g1 and 1249 GB at g4.
- Per-switch rate while busy is 48-55 GB/s, which is line rate.
- Per-switch link-busy time is 25.5 s at g1 and 11.97 s at g4 (×0.97 and ×0.92 applied): 2.13x faster.
- The link-bound queries scale at almost exactly the link ratio:
  - Q17: 2.20x, link busy 94% at g1 and 95% at g4.
  - Q19: 2.18x, 90% / 91%.
  - Q20: 2.10x, 93% / 91%.
  - Their whole gap (1.18 s across the three) is link.

**Pricing.** Three methods:

| method | calculation | result |
|---|---|---|
| (a) hard floor | Σ_q max(0, scan bytes_q / 105 GB/s − T1_q/4) | **3.48 s**. Q19 0.39, Q17 0.36, Q8 0.33, Q20 0.32, Q12 0.23, Q21 0.21, Q5/Q7 0.20. The best possible g4 suite with today's bytes is **12.43 s** (2.88x), or 13.40 s including staging bytes. |
| (b) additive split of the gap into link-busy and link-idle wall (per busier switch, un-profiled-equivalent) | link-busy: g1 25.5 s → g4 11.97 s, where linear is 6.38 s, so **5.6 s of gap**. That 5.6 s = 2x-instead-of-4x ceiling (+6.4) + staging bytes (+1.0) − better per-switch efficiency at g4 (−1.8: two GPUs on one switch fill each other's 1 MiB-copy gaps; one GPU alone reaches only 47.7 of 52 GB/s). Link-idle: 10.25 s → 8.05 s, where linear is 2.56 s, so **5.5 s of gap**, which belongs to the non-link areas. | ceiling net of efficiency = **4.6 s** |
| (c) counterfactual: per-GPU uplinks (210 GB/s), s=0.63 | 0.63 × 11.97/2 | **3.8 s** (5.9 s at s=1) |

Measured with **(d) x_g4_gpupin** (pin tier=gpu, which removes the host link; `X3/gpupin_price.py` →
`X3/gpupin_price.txt`):
- Q1, Q3 and Q18 are excluded. The gpupin run logged 9675 `[downgrade] convert failed ... stream belongs to CUDA
  device N but this representation's memory lives on device M` errors and 2524 "column count mismatch" warnings, in
  Q3 (7193), Q1 (5016) and Q18 (2381). Q3 and Q18 are 2.5x and 1.4x slower there, which points to a cross-device
  downgrade defect in GPU-tier pins. It deserves its own bug.
- On the 19 clean queries: g4 16.34 s → gpupin 12.51 s, a saving of **3.82 s**. Those queries' hard-floor share is
  3.48 s, which brackets the hardware share.
- Wall sensitivity to removing the link is 3.82 s / 9.50 s of g4 scan-copy time = **0.40**. It is about 0.9-1.07
  for the link-bound queries (Q2, Q11, Q17, Q20), about 0.6 for Q8, Q13 and Q19, and about 0 for Q9, Q21 and Q22.
- So methods (b) and (c) overstate it: during link-busy time, compute is partly hidden behind the link.
- The gap on these 19 queries falls from 9.03 to 5.21 s.

Central estimate: **3.6 s** (range 3.48-3.82). Method (b)'s 4.6 s is an upper bound.

**Remedy.**
- Hardware: a topology with one uplink per GPU, or a host/CPU with more lanes per GPU (Grace C2C).
- Software can only cut bytes (L4) or hide the idle link (L3). This share cannot be removed on this box.

### L2. Inter-pipeline serialization leaves the link idle between scans (design). Gap share about 1.0 s (bound 1.65), confidence medium

**Mechanism.**
- At query start the scheduler schedules only `scans.front()` (`src/pipeline/task_scheduler.cpp:209-221`).
- Creation is demand-driven, `strategy=active` and `priority=source` (`src/creator/config.hpp:101,106`), so a scan's
  tasks are created only when the hint chain reaches it, after the predecessor's PARTITION/CONCAT barrier.
- `memory_prefetcher` refuses queries with more than one eligible GPU (`src/scan_manager/sirius_scan_manager.cpp:2152-2157`).
- So nothing fetches the next scan's host chunks while shuffle/merge phases run. At g4 those phases are long (new
  N-forced partitioning, clones), so the link sits idle.

**Evidence (Quent g4_a iteration 3, `X3` session).**
- Q12: lineitem scan 0-635 ms, then PARTITION(2) 636-775 ms and CONCAT(3) 776-885 ms, with no HOST task in existence.
  The orders scan is created at 885 ms.
- Q18: the second lineitem scan is created at 1397 ms. Between 334 and 1253 ms only PARTITION(5) and
  MERGE_GROUP_BY(6) run.
- Q21 and Q10 follow the same pattern.
- nsys g4 (scan copies only, busier switch): interior idle is 5.70 s, of which 4.61 s is gaps of 5 ms or more (95
  gaps), plus a tail of 4.42 s after the last scan copy. The equivalent g1 figures are 7.69 s / 4.76 s / 3.65 s.
- Quent "starve" (HOST task created but not picked up while no HOST prepare is in flight on the switch) is only
  0.18 s per switch at g4 (`X3/g4_starve.csv`). The gaps are therefore not caused by a shortage of threads. **No scan
  work exists** during them.

**Pricing.**
- Upper bound: fill each gap of 5 ms or more with the H2D of later scans, capped by the copy time after the first gap
  (`X3/interior_scan2.py`).
  - g4: 3.11 s profiled, which is 2.85 s; excluding Q1's 0.36 s, whose gaps are thread-bound, it is **2.5 s ΔT4**.
  - g1: 3.49 s profiled, which is 3.4 s.
  - **Δgap ≤ 2.5 − 3.4/4 = 1.65 s.**
- Realistic: memory headroom limits run-ahead, and some gaps precede scans that depend on Bloom/dynamic filters.
  About half of the bound, so **ΔT4 about 1.2 s, Δgap about 1.0 s**.
- Largest per query: Q21 0.83 s, Q18 0.40, Q10 0.36, Q9 0.26, Q8 0.14, Q7 0.11, Q5 0.11, Q4 0.10 (bound, profiled).
- Q12's 0.35 s of gaps is followed by only 20 ms of copy, so prefetch cannot help there.

**Remedy.**
- `strategy=lookahead` or eager scheduling of independent scan sources, so that later scans' H2D overlaps
  PARTITION/CONCAT/MERGE phases.
- A multi-GPU-capable prefetcher that converts host chunks once a task latches its device (see the comment at
  `sirius_scan_manager.cpp:2148-2151`).
- Test: scratch-worktree flip of `config.hpp:101` to `lookahead` on Q9, Q10, Q18 and Q21 at g4.

### L3. Shuffle bytes staged through the host on the same uplinks (design). Gap share about 0.4 s (0.3-0.65), confidence medium

**Mechanism.**
- Round-robin partition placement moves about 3/4 of the partitioned bytes off-GPU.
- Cross-switch pairs (8 of 12) have no P2P, so they go D2H then H2D through the host pool
  (`convert:gpu_to_gpu:host_staging`, `peer_copy_into` in `cucascade/src/cudf/representation_converter_builtins.cpp:596-695`).
- Measured: 117.6 GB H2D plus 117.6 GB D2H staged, against 59.3 GB of intra-switch P2P. That 2:1 ratio matches 8:4
  pairs.
- Per CRITIC C1, true cross-switch P2P would traverse the uplinks too, so the lever is **fewer cross-switch bytes**
  (broadcast or co-partitioned joins, switch-aware two-level partitioning), not "enable P2P".
- The D2H leg also contends: the microbench bidirectional rate is 175 GB/s, not 105 + 114.

**Pricing.**
- Floor: 0.97 s, the per-query floor with staging bytes minus the floor without.
- Additive: 117.6/1367 × 11.97 = 1.03 s.
- s=0.63: 0.65 s.
- Per-query gpupin sensitivity (0.4 where it was not measured): **0.32 s**.
- Central **about 0.4 s** ΔT4. g1 has none, so Δgap is the same.
- Most staging happens in post-scan shuffle phases (Q3 tail 632 ms, Q18), when the link is otherwise idle. Its
  link-byte cost is therefore low, and its real cost is the latency of synchronous staging (M3).
- Hits: Q3 (27.2 GB staged, which takes the Q3 floor from 0.44 to 0.70 s), Q18 20.0, Q9 18.7, Q21 12.9, Q10 9.5,
  Q13 9.2.
- The clone latency (CPU/sync cost of staging) is priced elsewhere (M3). This is the link-byte part only.

### L4. Compression coverage (config). ΔT4 about 1.1 s, gap share about 0.5 s (0.35-0.87), confidence medium

**Mechanism.** Wire bytes that could be cut:

1. part, partsupp, customer and supplier have plans but they are disabled
   (`src/compression/simpatico_codegen/plans/tpch_sf1000/*_disabled.txt`), so 163 GB per suite ships raw.
   - Projected plan ratios are, for example: ps_partkey 101x, ps_suppkey 37x, p_partkey 254x, p_type 24x,
     c_custkey 194x, c_comment 2.8x.
2. `o_comment` uses `identity` with a "GPU-TIER OVERRIDE" (`plans/tpch_sf1000/orders.txt:50`), but this is a host
   pin. `docs/super-sirius/compressed-pinning.md:80-81` itself says snappy at 2.6x is right for host pins.
   - Q13 ships 79.7 GB of raw `o_comment`.
3. `l_shipmode` uses `str_split` at 1.7x (`lineitem.txt:74`), about 29 GB per scan, although it has only 7 distinct
   values. A dictionary (as on `l_shipinstruct`, 61.8x) would cut it to about 1.5 GB.

**Bytes saved** (`X3/compression_lever.py` → `X3/compression_lever.csv`; column-width model applied to measured
per-(query, table) wire bytes): raw tables −130.8 GB, `o_comment` −49.1 GB (Q13), `l_shipmode` −53.5 GB (Q12, Q19).
Total **−233 GB = 19%** of the 1249 GB of scan wire bytes.

**Pricing.** ΔT = ΔB/B_q × copy_q × s:

| | s=0.63 | s=1 |
|---|---|---|
| ΔT4 | 1.25 s | 1.98 s |
| ΔT1 | 2.81 s | 4.47 s |
| Δgap | 0.55 s | 0.87 s |

- With the gpupin per-query sensitivity, clipped to [0, 1], applied to the s=1 column: **ΔT4 1.12 s, Δgap 0.49 s**.
  This is the central estimate.
- Top queries by Δgap at s=1: Q13 0.18, Q19 0.12, Q2 0.10, Q12 0.10, Q11/Q16 0.06 each, Q10 0.06.
- Lowering the floor also lowers L1's hard share, from 12.43 s toward about 10.5 s.
- Decode cost is small: snappy at 260 GB/s is about 80 GB/4 GPUs, roughly 77 ms per GPU.

**Remedy.** Re-pin with the four plans enabled, `o_comment → snappy`, and `l_shipmode → dictionary + bitpack`. This
is experiment E7 in CRITIC.

### L5. Synchronous per-task prepare and allocator calls inside the H2D path (mechanical). Gap share ≤0.3 s, confidence medium

**Question:** can 4 threads per GPU keep the link busy? **Yes at g4. The deficit is at g1.**
- Mean concurrent HOST prepares per switch at g4 are 4.7-7.0 out of 8 threads (`conc_s*` in `X3/g4_starve.csv`).
- Quent starve: 0.18 s per switch at g4 vs **2.33 s at g1** (Q1 1.24 s, Q13 0.80 s; g1 is compute-bound with all 4
  threads computing).
- nsys "prepare open but no H2D copy on the switch" (`X3/link_states.py`): **1.0 s per switch at g4** (Q1 0.46, Q18
  0.11, Q4 0.09, Q12 0.08, Q6 0.07) vs 1.98 s at g1.

**Prepare thread-time breakdown, g1 → g4** (nsys, profiled, `X3/prep_api_agg.py`). `sirius::compression::host_to_gpu`
goes from 70.8 to 115.9 s (+45.1) and raw `convert:host_to_gpu` from 10.8 to 17.1 s (+6.3):

| component | g1 | g4 | delta |
|---|---|---|---|
| Link-related: cudaStreamSynchronize | | | +23.2 s |
| Link-related: raw-path cudaMemcpyBatchAsync, which blocks on the link | | | +6.1 s |
| cudaMallocFromPoolAsync + cudaFreeAsync | 4.7 s | 22.6 s | **+18.0 s** |
| cudaMemcpyAsync issue (1 MiB pieces) | | | +3.2 s |
| decode kernel time | 4.81 s | 4.49 s | flat |

- Link-related waits total +29.3 s.
- The allocator figure matches the alloc_contention microbench: the driver allocator is process-global at about
  300k calls/s. Per-call latency is 11 µs at 1 GPU x 4 threads and 50-56 µs at 4 GPUs x 4 threads.
- **Correction to CRITIC §0:** the "+52.3 s link" bucket is about 57% link waiting and about 35% allocator
  contention.
- Wall impact is small because the switch stays saturated during scan phases. The allocator and issue time land
  mostly while other threads' copies keep the link busy.

**Pricing.**
- Wall loss ≤ prepare-but-idle at g4 excluding the thread-bound Q1: (1.0 − 0.46) × 0.92 = 0.5 s ΔT4.
- Δgap is about 0.5 − (g1 prep-idle 1.98 − Q1 0.22) × 0.97/4 ≈ **0.07-0.3 s**.
- A within-pipeline overlap engine (or more threads) helps g1 more than g4: it would widen the gap by about 0.3-0.6 s
  (g1 starve 2.3 s / 4). x_g1_thr8 vs x_g4_thr8 will show this.

**Remedy.**
- Pool or arena the per-task decode buffers instead of driver stream-ordered alloc/free per buffer.
- Larger host blocks (`memory.host.block_size` 1 MiB → 64 MiB) cut copy issue count.

### L6. Round-robin chunk-to-GPU assignment vs switch pairing (design). Gap share about 0.04 s, confidence high

- `task_creator.cpp:804-811` assigns `_numa_affinity.next_gpu(...)`, a global cursor 0,1,2,3,0,...
  (`src/memory/numa_affinity.hpp:31-37`). So consecutive chunks alternate between the two GPUs of switch A, then B.
- All chunks of a scan are created up front, and each GPU drains its own binding queue, so the switches stay balanced:
  - per-switch bytes 683.97 vs 682.75 GB;
  - per-GPU bytes 341.0-342.5 GB;
  - Σ_q |copy_s0 − copy_s1|/2 = 82 ms profiled, about 0.04 s;
  - per-GPU rate while busy is 29.7-29.8 GB/s on all 4 GPUs.
- Not a factor. It would matter only with uneven per-GPU compute, and then work stealing is the fix (scheduling S2).

### L7. 1 MiB host blocks (mechanical). Gap share negative, confidence medium

- 1 MiB `cudaMemcpyAsync` pieces cost the single GPU about 8% of link rate (47.7 vs 52 GB/s while busy).
- At g4 two GPUs per switch fill each other's gaps (48-55 GB/s per switch while busy).
- Fixing it saves about 2 s at g1 and about 0 at g4. It **widens** the gap by about 0.4 s, so it is not a scaling
  cause. Listed for completeness.

## 3. Summary table (g4 suite, wall-time)

| id | kind | finding | Δgap priced (s) | ΔT4 (s) | method |
|---|---|---|---|---|---|
| L1 | hardware | 2 uplinks for 4 GPUs: the link scales 2.13x, not 4x | **3.6** (3.48-3.82) | 3.82 measured for no link (19 clean queries) | floor (lower) / gpupin (upper) |
| L2 | design | strict inter-pipeline serialization, no multi-GPU prefetch → 4.6 s of interior link gaps | **~1.0** (≤1.65) | ~1.2 (≤2.5) | gap fill bound (nsys) ÷2 |
| L3 | design | cross-switch shuffle staged via host: 117.6 GB extra H2D (+117.6 D2H), link-byte part only | **~0.4** (0.3-0.65) | 0.32-0.65 | bytes × gpupin sensitivity |
| L4 | config | compression coverage: 4 tables raw, o_comment identity, l_shipmode str_split (−233 GB) | **~0.5** (0.35-0.87) | ~1.1 (0.8-2.0) | ΔB/B × copy × gpupin per-query sensitivity |
| L5 | mechanical | synchronous prepare plus allocator calls in the H2D path: +18 s thread-time, but the link stays saturated; thr8 makes g4 worse | ≤0.3 | ≤0.5 | prepare-idle time |
| L6 | design | round-robin chunk to GPU vs switch pairing | 0.04 | 0.04 | switch imbalance |
| L7 | mechanical | 1 MiB H2D pieces (hurt g1 only) | −0.4 | ~0 | rate while busy |

The remaining 5.5 s of gap is link-idle time (non-scan phases) that scales only 1.27x. It belongs to the
partition/clone/compute areas (M2-M4 in CRITIC). This note does not price it.

## 4. Experiments (results landed after the draft)

**x_g4_gpupin: 18.26 s, but sd is 1.30 s and Q1/Q3/Q18 are polluted by errors.** See L1(d).
- On the 19 clean queries it saves 3.82 s (16.34 → 12.51). That is the measured upper bound on link cost at g4.
- The link-bound queries (Q17, Q19, Q20, Q2, Q11, Q6, Q13) gain 0.16-0.63 s each.
- Q9, Q21, Q16 and Q22 gain nothing. They are bound elsewhere, by shuffle and compute.
- Q10 is 0.37 s slower under gpupin, likely from the downgrade and memory pressure that GPU-resident data causes.
- The x_g4_joinpol / x_g1_joinpol runs are not link experiments: 19.07 / 34.74 s, with scan bytes unchanged.

**x_g4_batch4g (scan_task_batch_size 4 GB, re-pinned: lineitem 145 chunks, orders 38): 20.04 → 16.91 s (−3.13 s).**
The gain is not on the link:
- Link-bound queries are unchanged: Q17 0.824, Q19 0.937, Q20 0.767. They sit at the floor.
- The gains are Q18 −0.56, Q1 −0.36, Q21 −0.42, Q12 −0.29, Q4 −0.24, Q8 −0.26, Q5 −0.21.
- Quent (flattened to `X3/q_batch4g`, iteration 3) shows where it comes from:
  - scan-prepare thread-time 131.9 → 124.3 s, almost unchanged, since link waits remain;
  - PARTITION operator 29.8 → 7.2 s;
  - other operators 35.3 → 21.0 s;
  - non-scan (clone) prepare 25.6 → 20.8 s.
- Link starvation stays at 0.17 s per switch.
- So batch4g attacks per-task and fan-in overhead (M3-M5), and leaves the link floor intact.
- With batch4g the link floor of 12.43 s becomes a larger share of the remaining gap: 16.91 − 8.96 = 7.95 s, of which
  about 3.5 s is link, about 45%.

**x_g4_thr8 / x_g1_thr8 (8 executor threads per GPU):**
- g4 gets worse, 20.04 → 21.23 s (Q18 +0.26, Q1 +0.17, Q15 +0.10, Q10 +0.10).
- g1 is flat, 35.82 → 35.55 s, with Q1 −0.21 (g1 Q1 starve was 1.24 s) and Q13 −0.36 (Q13 warm-up noise).
- The link-bound Q17, Q19 and Q20 are unchanged at both counts.
- This confirms L5: at g4 the link is already saturated during scan phases, so more threads only add contention.

**x_g4_hpb256 / x_g4_hpb4g:** 23.66 / 19.91 s. Neither changes scan bytes, so they are not link experiments.

Predictions written before the results landed (kept for the record):
  - **gpupin (g4)** removes about 1249 GB of scan H2D. That scan link-busy time is 11.97 − 1.03 = 10.94 s.
    Expected suite is 20.04 − 10.94 × s, which is about 13.1 s at s=0.63 and 9.1 s at s=1. GPU-tier scans also skip
    the HOST prepare and its allocator calls, so the result may be lower still.
    The *g4 − gpupin* delta measures the total link cost at g4 (L1 plus the link-byte part of L4). It is not the gap
    share, because g1 also pays link.
  - **batch4g:** 4x fewer, larger scan tasks. The link-byte floor is unchanged. Any gain comes from the downstream
    fan-in (M5), not the link. If prepare-idle rises, the per-task synchronous prepare matters more with fewer tasks.
- x_g4_joinpol / x_g1_joinpol (already done): 19.07 / 34.74 s. They do not change scan bytes. The link floor is still
  12.43 s, so the link share of the new gap (19.07 − 8.69 = 10.38 s) rises to about 40-45%.

## 5. Artifacts (`/home/ubuntu/sirius-scaling-20261105/10-analysis/X3/`)

| artifact | contents |
|---|---|
| `link_per_query.py` → `g1_link.csv`, `g4_link.csv` | bytes by kind and NVTX category, per-device/switch busy union, per-copy rate by copyKind |
| `floors.py` → `floors.csv` | link floors and hardware share |
| `link_states.py` → `g{1,4}_states.csv` | copy / prepare-idle / none split per switch |
| `interior_scan2.py` → `g{1,4}_interior_scan2.txt` | head / interior / tail, gaps ≥5 ms, prefetch bound |
| `quent_starve.py` → `g{1,4}_starve.csv` | starve, concurrent prepares |
| `table_bytes.py` → `g4_table_bytes.csv` | wire vs decoded per table |
| `compression_lever.py` → `compression_lever.csv` | |
| `prep_api_agg.py` (+ `g{1,4}_prep_api.txt`) | |
| `dmon_summary.py` | |
| `gpupin_price.py` → `gpupin_price.txt` | |
| `q_batch4g/` | flattened Quent for batch4g |
| `per_query_table.md` | |

## 6. Update 18:35 (re-run: verification plus experiments that landed after 17:26)

**Verified again in this pass:**
- Code citations still match bb557b1a:
  - `task_scheduler.cpp:209-221`: only `scans.front()` is scheduled.
  - `sirius_scan_manager.cpp:2148-2157`: the prefetcher is skipped when there is more than one eligible GPU.
  - `numa_affinity.hpp:31-37`: a global round-robin cursor.
  - `task_creator.cpp:808-811`: the host-tier chunk is mapped to `next_gpu`.
  - `creator/config.hpp:101,106`: `active` / `source`.
  - `compressed_representation.cpp:62-86`: one `cudaMemcpyAsync` per 1 MiB block.
- The PtoP labelling, checked independently on g4 Q17 (rep.14, iteration-2 window):
  - Pinned→device bytes are 85.4 GB.
  - 62.8 GB of that is copyKind 10 (PtoP), with srcDeviceId set to one of the *other* three GPUs and never the
    destination. 22.6 GB is copyKind 1 (HtoD).
  - Per-GPU union busy is 92.2-92.8% of the query window.
  - The rate is about 28 GB/s per GPU for both kinds.

**New experiments, with the link share recomputed per pair of runs** (per-query scan bytes are unchanged by these
flips; floor = busier-switch scan bytes / 52 GB/s, from `X3/floors.csv`):

| pair (g1 / g4) | T1 | T4 | gap | hw floor share Σmax(0, floor−T1/4) | (+staging bytes) | best possible Σmax(floor, T1/4) | queries at the link floor (T4 ≤ 1.15 × floor) |
|---|---|---|---|---|---|---|---|
| g1_a / g4_a | 35.82 | 20.04 | 11.08 | 3.48 (31%) | 4.44 | 12.43 | Q17, Q19, Q20 |
| x_g1_batch4g / x_g4_batch4g | 33.04 | 16.91 | 8.65 | 4.01 (46%) | 5.05 | 12.27 | Q6, Q17, Q19, Q20 |
| x_g1_best / x_g4_best | 31.93 | 16.92 | 8.94 | 4.23 (47%) | 5.28 | 12.21 | Q6, Q12, Q17, Q19, Q20 |

- g1 is also heavily link-bound. The g1 link floor (1254 GB / 52 GB/s, per query) sums to 24.1 s, against T1 of
  31.9-35.8.
- At g1, Q2, Q6, Q8, Q11, Q17, Q19 and Q20 are within 15% of their floor.
- The ideal of T1/4 therefore assumes a 4x link that this box does not have.
- Fixes that speed up g1 (batch4g, best) raise the *share* of the gap the link owns, because they cut compute but
  leave the floor unchanged.
- The **staging-bytes** column is valid only if batch4g keeps about 117.6 GB of host-staged shuffle. That is not
  re-measured, since there is no nsys for batch4g, so treat it as an upper value.
- x_g4_batch8g is 17.34 s, worse than batch4g. Q2 0.37→0.49 and Q11 0.25→0.32 regress, probably from too few tasks on
  small scans (per-GPU binding with fewer than 4 chunks). Link bytes are unchanged, so it is not a link effect.

**What gpupin does and does not price (refinement of L1).**
- x_g4_gpupin −3.82 s (19 clean queries) is the *total* link cost at g4: removing the link entirely.
- The gap share of the hardware ceiling is the cost of the 2x-instead-of-4x part only, which is ≤ the total. It is
  bracketed by:
  - floor 3.48 s, the part even a perfect engine pays;
  - gpupin 3.82 s, which is ≥ the 4-uplink counterfactual because a 4x link still costs something.
- The central value stays **3.6 s at baseline**. It rises to about **4.0-4.2 s once batch4g/best are applied**,
  because the gap shrinks around it.

**Final ranked link-related shares of the 11.08 s gap (baseline g4_a), unchanged from section 3:**
- L1 hardware 3.6 s.
- L2 inter-pipeline serialization about 1.0 s.
- L4 compression coverage about 0.5 s.
- L3 staged-shuffle link bytes about 0.4 s.
- L5 synchronous prepare ≤ 0.3 s.
- L6 switch-pairing 0.04 s.
- L7 1 MiB blocks −0.4 s.

Together that is about 5.4 s of link-related gap. The other about 5.5 s is link-idle time owned by the
partition/clone/compute areas.
