# Verify (measurement lens): rank 2 "driver-allocator-lock" (claimed 1.80 s, 16.2% of gap)

Verdict: CONFIRMED for the 1.80 s share (my number: 1.7 s, range 1.5-2.0).
The supporting lever numbers are OVERSTATED: X1's 2.4 s (1.8-2.9) and R1's ΔT4 3.0 s (2.3-3.7) both use a
profiled→un-profiled factor of ×0.75. The data supports ×0.55-0.69. Corrected: X1 ≈ 2.0 s (1.8-2.2), R1 ΔT4 ≈ 2.45 s (2.2-2.7).

Scripts and outputs are in `30-diagnosis/vm_alloc/`: `common.py`, `apisum.py` (+`per_query_api.txt`) and `opcmp.py`.
The remaining analyses were inline python against the same files.

## 1. Raw numbers re-derived: they match
- `apisum.py` sums each nsys2 report over its iter=2 [range_start, range_end] window from the map:
  - g1: malloc 911,291 calls at 8.0 µs (7.33 thread-s); free 911,242 calls at 2.9 µs (2.66 thread-s).
  - g4: malloc 1,197,402 calls at 35.4 µs (42.43 thread-s); free 1,197,206 calls at 29.8 µs (35.65 thread-s).
  - These match X1 E1 exactly.
- Window and iteration choice is correct (iter2 only, query window only).
- The microbench (`alloc_contention/results.txt`, un-profiled) counts individual API calls (`alloc.cu:26-39`):
  - 1 GPU x 4 threads: 340k/s, mean 10.9 µs.
  - 4 GPUs x 4 threads: 279k/s, mean 55.9 µs.
  - 1 GPU x 16 threads: 385k/s, mean 37.7 µs.
  - So the lock is real without CUPTI. It scales with the number of threads in the process, not with GPU count.

## 2. Main issue: CUPTI amplifies this contention more than X1 allows for
Profiling inflation per query, g4 (nsys window − un-profiled hot mean):
- Suite total is +1.74 s (21.78 vs 20.04).
- It is concentrated in the allocator-heavy queries: Q18 +0.30, Q1 +0.26, Q21 +0.18, Q4/Q10/Q12/Q8/Q9/Q15 +0.11-0.13.
- Link-bound Q17/Q20/Q22/Q2/Q11 are about 0.
- Regressed on X1's per-query profiled price (`price_g4_signed.json`, Σ 3.18 s): **r = 0.98, slope 0.52**.
- At g1 the inflation is 1.36 s excluding cold Q13. That is CUPTI's baseline per-call overhead, and it is already in
  X1's reference (the g1 profiled mean).
- At g4 that baseline is worth 0.34-0.76 s of wall: ×1/4 assumes thread-time spread over 16 threads, ×T4/T1 assumes
  it scales with query time.
- So contention amplification at g4 = 1.74 − (0.34-0.76) = 0.98-1.40 s.
- Un-profiled price = 3.18 − (0.98-1.40) = **1.78-2.20 s**, i.e. a factor of ×0.56-0.69. X1 uses ×0.75 centrally.

Independent check from Quent, profiled run (nsys2) vs un-profiled run, iter2 op thread-s (`opcmp.py`):

| op | g1 un-prof → prof | g4 un-prof → prof |
|---|---|---|
| PARTITION | 6.06 → 7.32 | **29.70 → 41.04 (+11.3)** |
| HASH_GROUP_BY | 7.42 → 8.05 | 11.44 → 14.68 (+3.2) |
| all tasks | 131.9 → 138.5 (+6.6) | 252.5 → 278.5 (+26.0) |

- PARTITION: X1's profiled allocator excess is 25.7 thread-s. Subtracting the ~10 thread-s of g4-only CUPTI
  amplification leaves ≈15-16 un-profiled, a ratio of ≈0.6.
- HASH_GROUP_BY: X1 excess 5.9 → ≈3.6 un-profiled, a ratio of ≈0.6.
- Whole-task thread-time gives a looser ×0.74. This is an upper bound: amplified allocator time is partly absorbed
  by shorter link waits.
- Conclusion: use ×0.6 (0.55-0.69).
  - X1 lever: 3.18 × 0.6 = **1.9-2.0 s** (1.8-2.2).
  - Sub-allocator lever: 3.98 × 0.6 = **2.4 s** (2.2-2.7), not 3.0.
  - Δgap shrinks in proportion, to about 2.0-2.3.
- Other profiled-only figures overstate the un-profiled lock in the same way and should be labelled "profiled":
  - "53% of wall busy" and the 3.6 mean callers inside;
  - the 63.3 thread-s of nested locks (OSRT interposition sits inside the critical section);
  - the 35.4 µs mean.

## 3. Why the 1.80 share still holds
- The family prices behind 1.70 (scanagg 369, groupby 353, semijoin 476, multijoin 270, joinship 120, subq 95,
  small 15) are mostly **un-profiled** Quent phase deltas, with nsys used only to attribute them to the allocator.
  They are therefore not CUPTI-inflated.
  - groupby Q18 PARTITION(5): 5,099 → 1,696 thread-ms, which is −216 ms on 16 threads.
  - scanagg Q1: +4,057 thread-ms over 16 threads = 254 ms.
- Small over-attributions, about −0.15 s in total:
  - semijoin multiplies a profiled allocator fraction by un-profiled phase wall. The fraction is about 8% high (≈ −0.04).
  - groupby books all of the PARTITION(5) inflation to the allocator, but non-allocator PARTITION thread-time also
    grew (≈1.5 → 2.2 thread-s). The allocator part is ≈165-190 of the 216 ms (≈ −0.04).
  - The +0.30 s "task-end frees" moved from FRAG overlaps the groupby D "merge frees 53 ms" that is already in ALLOC
    (≈ −0.05). Scaled at ×0.6, X1 frees are 0.38 s, so +0.30 is otherwise fine.
- Corrected share: **1.65-1.8 s, central 1.7**. That agrees with the X1 lever at ×0.6 (1.9-2.0), which should be
  slightly larger because of link co-binding.

## 4. Experiment correlations: real, but not a size check
- **batch4g.** ΔT4 against price gives r = 0.96. It survives controlling for clone (GPU-origin prepare) thread-s:
  ΔT4 = 1.02·price − 0.01·clone, R² 0.93. Clone alone gives R² 0.20.
  - So batch4g's per-query pattern is allocator-driven, not fragment-driven.
  - But the slope against the **profiled** price is 0.94. With an un-profiled price ≈ 0.6 × profiled, about 40% of
    batch4g's gain is non-allocator work that scales with the same per-task count.
  - So r = 0.96 confirms the mechanism, not the magnitude.
  - batch4g's scaling-specific delta (ΔT4 − ΔT1/4) is 2.43 s, r = 0.93.
- **thr8.** Against price r = 0.97, slope 0.37. Against clone r = 0.51. It is allocator-specific and supports the
  mechanism.

## 5. Confounds checked
| Confound | Effect on this item |
|---|---|
| Q13 cold at g1 (iter2 3.92 vs 3.01 hot mean) | Allocator price for Q13 is about 0. Excluded from the inflation regression. No effect |
| Q3 join-mode (STANDARD at g1) | X1 uses the suite-mean g1 per-call reference, not Q3's own (17.4 µs). Q3 is about 0.08 s un-profiled. Negligible |
| H1 pooled-event waits | Carved out (−0.20, #10) |
| Clone-path syncs | Booked in X2/#3. Clone-target mallocs are only 0.07 s profiled |
| Link co-binding | Handled by the soft cap. It only makes the share smaller than the lever |
| Kernels/clocks | Q18 PARTITION kernels 188 vs 183 ms; not re-checked here |

## 6. Bottom line
- Mechanism: confirmed. The process-wide driver lock is visible un-profiled in the microbench, and in un-profiled
  Q18 PARTITION(5): 77.7k calls in 324 ms ≈ 240k/s, flat for g2s/g2x/g4.
- Share: **1.7 s (1.5-2.0)**, compared with 1.80 claimed.
- Corrections needed in the RANKED text:
  - The X1 lever is 2.0 (1.8-2.2), not 2.4 (1.8-2.9).
  - R1 ΔT4 is ≈2.4 (2.2-2.7), not 3.0 (2.3-3.7). Δgap is ≈2.0-2.3, not 2.4-2.8.
  - Label as profiled the 53% busy, the 63.3 thread-s, and the 35.4 µs latency.
