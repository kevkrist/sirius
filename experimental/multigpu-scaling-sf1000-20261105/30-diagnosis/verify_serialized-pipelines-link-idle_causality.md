# Verify (causality): rank 5 "serialized-pipelines-link-idle" — claimed 1.20 s (10.8%)

Verdict: **OVERSTATED. Corrected share ≈ 0.75 s (range 0.6-0.9).** The mechanism is real for multi-scan queries
(Q21, Q9, Q18, Q10, Q14, Q4, Q16), but:
- about 0.45 s of the 1.20 s residual is not this mechanism;
- one of the three cited code mechanisms (the prefetcher guard) has no causal role.

Scripts and outputs are in `30-diagnosis/verify_serial/`, all CPU-only (no GPU use):
- `timeline.py`: per-pipeline created/prep/end times from Quent.
- `scan_phases.py`: lookahead reach R_q, calibrated with gpupin.
- `gap_staging.py`: staged-shuffle H2D inside the scan-link gaps.
- `q_gpupin/` and `q_batch4g/`: flattened Quent for those runs.

## 1. Code check

| Cited mechanism | Verdict | Evidence |
|---|---|---|
| `task_scheduler.cpp:209-221` schedules only `scans.front()` | **Correct** | — |
| `creator/config.hpp:101,106`: `active` / `source` | **Correct.** Not configurable: `sirius_config.cpp:178-185` rejects the yaml keys | One-task lookahead exists (`task_creator.cpp:528-590`, fired from `task_scheduler.cpp:295` when a device is ready). The scan hint is always READY (`sirius_gpu_scan_operator.cpp:672-676`), so later scans *could* be created early |
| Timeline: no scan work exists in the gaps | **Confirmed** | Quent g4_a Q12 iter3: orders scan created at 886 ms, the moment CONCAT(3) ends at 886. Q21 iter3: GPU_SCAN(28) created at 800 = CONCAT(27) end; GPU_SCAN(47) at 1341 = CONCAT(46) end |
| `sirius_scan_manager.cpp:2152-2157` multi-GPU prefetcher guard | **Not causal** | `memory_prefetcher_config::enable{false}` (`src/scan_manager/config.hpp:172`). Neither config enables it. `[memory_prefetcher]` appears 0 times in the g1_a and g4_a logs. The prefetcher is off at **both** GPU counts, so the guard only blocks the remedy; it explains none of the g1→g4 loss |

## 2. What the 1.20 s actually is

The 1.20 s is the per-query sum of (family link price − hard floor), from `link_check.py`. That accounting residual
mixes three things.

### 2a. Tails with no later scan to prefetch (the remedy cannot reach them)

| Query | fam − hw | R (lookahead reach) | Why it is out of reach |
|---|---|---|---|
| Q13 | 270 ms | 0 | Single orders scan, then the DENSE_COUNT_JOIN barrier. Interior gaps 0 ms (nsys 3 ms) |
| Q1 | 61 ms | 0 | Single scan, then the merge tail |
| Q3 | 178 ms | 48 | 632 ms probe-shuffle tail |
| Q12 | 32 ms | 22 | X3 notes the same: only 20 ms of copy follow the gaps |
| Q22 | 31 ms | ~0 | gpupin gains ~0 |

These are post-last-scan barrier tails (source-first priority plus FULL barriers). Fixing them would take streaming
of exchange/merge under the scan. That is a different mechanism from "no lookahead".

### 2b. T1-basis artifacts

The hard floor uses hot-mean T1, but the family phase prices use steady per-phase g1.

| Query | Floor with hot-mean T1 | Floor with corrected T1 | Residual: before → after |
|---|---|---|---|
| Q3 (joinpol g1 = 1.164 s, removes the STANDARD-join confound) | 0 | 0.44 − 0.29 = 0.149 s | 178 → 29 ms |
| Q13 (steady g1 = mean of iters 3-4 = 2.557 s) | 0.07 s | 0.19 s | 270 → 150 ms |

That is about 0.27 s of the 1.20 s booked as "serialization" which is really the link floor (#1) or the #11 credit.

### 2c. Negative per-query residuals

Q2, Q8, Q11, Q17, Q19 and Q20 sum to −76 ms. That is noise in the family pricing.

## 3. Discriminating check: is the link-excess of *later* scans really hideable? (gpupin-calibrated)

Method:
- For each query, take the scan pipelines in host-pinned g4_a (iters 2-4).
- Interior gaps = non-scan time of 5 ms or more between scan phases.
- Later-scan link excess = Σ over scans after the first gap of (scan phase duration host-pinned − same scan phase
  under x_g4_gpupin).
- Reach R_q = min(gaps, later excess) is the most ΔT4 that lookahead or prefetch can buy.

This replaces X3's "copy after first gap" with the measured link-attributable part of each later scan.

**Results** (un-profiled, ms; `verify_serial/scan_phases_g4_vs_gpupin.txt`):

| Q | gaps | later excess | R |
|---|---|---|---|
| Q21 | 723 | 546 | 546 |
| Q10 | 333 | 294 | 294 |
| Q9 | 241 | 677 | 241 |
| Q18 | 996 | 172 (gpupin-confounded) | 172 |
| Q8 | 147 | 525 | 147 |
| Q5 | 101 | 334 | 101 |
| Q4 | 98 | 115 | 98 |
| Q14 | 88 | 285 | 88 |
| **Suite** | 3.50 s | 4.49 s | **1.87 s** |

- X3's suite ΔT4 bound was 2.5 s. R (1.87 s) is lower.
- Under batch4g the gaps shrink to 2.10 s and R falls to 1.48 s.
- **Q21 does pay for the link, even though its gpupin total barely moves** (1954 → 1996 ms):
  - Its three lineitem scan phases fall from 372/203/374 ms to 195/73/128 ms.
  - Its non-scan phases grow under gpupin (memory pressure).
  - So the "Q9/Q21 gain ~0 from removing the link" reading in X3/RANKED is a confound for Q21. Q21 is the
    strongest real case for this bottleneck.

**The gaps are not entirely link-idle** (`gap_staging_g4.txt`, nsys g4 iter2, busier switch):
- Host-staged shuffle H2D is in flight for 616 of the 4543 ms of gaps (13.5%): 27.4 GB.
- Q18 accounts for 226 ms / 9.9 GB, Q21 for 104 ms / 4.8 GB, Q9 for 104 ms / 4.5 GB.
- X3 excluded `convert:gpu_to_gpu` copies when it called these gaps "idle".
- A prefetch would contend with these critical-path clone legs. Staging runs at about 30 of 52 GB/s, so this costs
  about 0.05-0.1 s of reach.

## 4. Corrected share (mechanism-faithful)

Share_q = min(max(fam − hw (corrected basis), 0), R_q):

| Query | Share (ms) |
|---|---|
| Q21 | 175 |
| Q9 | 150 |
| Q18 | 121 |
| Q10 | 103 |
| Q14 | 48 |
| Q16 | 36 |
| Q4 | 31 |
| Q3 | 29 |
| Q12 | 22 |
| Q15 | 9 |
| Q7 | 9 (Q7 label mismatch: orders scan 59 vs 50 ms) |
| Q5 | 6 |
| **Total** | **≈ 0.74 s** |

- Q13, Q1, Q6 and Q22 contribute 0.
- This coincides with RANKED's own gpupin-capped lower bound (0.78).
- Reallocate the other ≈0.45 s:
  - ≈0.27 to #1 / #11 (the basis artifacts);
  - ≈0.25 to a separate "post-scan barrier tails" item (Q13 DCJ, Q3 probe shuffle, Q1 merge; overlapping #3/#6);
  - −0.08 of noise.

## 5. Lever of the stated remedy (lookahead + multi-GPU prefetcher)

| | Value |
|---|---|
| ΔT4 bound | 1.87 − ~0.1 (staging contention) ≈ 1.8 s |
| ΔT4 realistic | ≈ 0.9 s (memory headroom; Bloom-dependent later scans) |
| ΔT1 | Same design at g1, where the prefetcher is also off. X3 g1 gap-fill bound is 3.4 s profiled; scaled by the same gpupin calibration ratio (0.74), about 1.8 s bound / 0.9 realistic, so ΔT1/4 ≈ 0.2-0.45 |
| **Δgap realistic** | **≈ 0.5-0.7 s** (claim: 1.0; bound 1.65) |

Interaction risk, not priced: eager scans during exchange phases add H2D-decode allocator calls (X3 L5: +18
thread-s at g4) next to allocator-heavy clone and partition tasks. thr8 shows extra concurrency costs +1.19 s through
#2. Until the per-GPU sub-allocator (#2 R1) lands, the net could be close to 0.

## 6. Minor evidence corrections

- "Those phases are longer at g4": the interior gaps of 5 ms or more are 4.61 s at g4 vs 4.76 s at g1 (profiled).
  They do not *shrink*; they are not longer.
- The cited 4.42 s tail is **unreachable** by the remedy (no later scan exists to prefetch).
- Q12, the cited example, is a case where prefetch cannot help: R = 22 ms; X3 measured 20 ms of copy after the gaps.
