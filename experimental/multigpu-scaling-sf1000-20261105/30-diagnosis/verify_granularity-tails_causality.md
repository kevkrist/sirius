# Verify rank 8 "granularity-tails" (causality lens)

Verdict: **OVERSTATED as a mechanism; size about right only by accident.** Corrected: **~0.22 s** of g4 gap (range 0.15-0.30).
About half of the claimed 306 ms is not granularity. The claim also leaves out ~70-100 ms of real imbalance, mostly in the
multijoin queries. One of the three remedies loses time overall.

## Check run: GPU-occupancy loss (data only, no GPU)

Scripts are in `30-diagnosis/verify_gran/`: `occupancy.py`, `occ_attr.py`, and outputs `occ_g4_a.txt`,
`occ_attr_g4_a.txt`, `batch48_occ.txt`, `base_pipes.txt`.

- **How L is computed.**
  - Quent data, hot iters 2-4, inside the [executing, finished] window.
  - A GPU counts as busy while any of its tasks sits between `preparing` and `finalize`.
  - L = ∫(1 − k/4) dt over the periods when only 1-3 of the 4 GPUs are busy.
  - L is the wall time saved if the work in those periods were spread perfectly over 4 GPUs. That is the most any
    granularity, imbalance or tail fix could save.
- **L tracks the wall cost of granularity (discriminating test).**
  - Going from batch4g to batch8g changes only the task count. Across 22 queries, Δwall vs ΔL gives r = 0.87 and a
    slope of 0.70-0.75.
  - Per-query examples: Q2 +92 / +147, Q11 +76 / +126, Q20 +77 / +89, Q9 +80 / +111, Q16 +44 / +67 ms.
- **Baseline g4_a suite.**
  - Some GPU is idle for 1.53 s of the suite. L = 0.87 s. No GPU at all is busy for 0.08 s.
  - L split by in-flight pipeline (the remainder is too small to matter):

| class | L ms | owner |
|---|---|---|
| PARTITION with 4 tasks (1 per GPU) in Q4/5/8/9/10/21: the barrier task stalls during Bloom publication | 164 | #9 BLOOM, not granularity |
| single-task merges, a fan-in of 627 fragments onto 1 GPU (Q1 MERGE_GROUP_BY 111, Q6 MERGE_AGGREGATE 18, Q21 7) | 160 | #3 FRAG |
| 2-way and 4-way CONCAT gathers (Q22 16, Q11 14) | 109 | #3/#4 cross-GPU |
| P mod N, waves and remainder (Q9 HJ(32) c=21 22.7, Q21 HJ(18) c=6 17.4, Q10 PARTITION(4) c=163 15.7, Q10 MERGE c=9 12.3, Q15 MERGE c=5 9.5) | 204 | granularity |
| skew with task count a multiple of N (Q18 MERGE c=36 20.7, Q9 PARTITION c=12 8.4) | 91 | granularity |
| end skew on big scans (Q1 13.5, Q19 11.8) | 54 | granularity / tail |
| fewer than N scan tasks (Q2 c=2 13.0, Q16 7.6, Q15 7.0, Q20 4.4) | 52 | granularity (scan quantization) |
| 1-task serial tails (TOP_N, RESULT, SORT_SAMPLE) | 32 | Amdahl tail |

- **The granularity pool is 433 ms of L.** At the measured conversion of 0.70, that is **≈0.30 s optimistic**.
  - Part of it is tasks that end late because the allocator inflated them (#2). Shrink the inflation and the tail
    shrinks too.
  - So the true granularity price is ≈0.20-0.25 s.

## The claim's items, checked against cause

| item (ms claimed) | finding | keep |
|---|---|---|
| Q15 P mod N (11) | L drops 23.4 → 17.4 under hpb4g, and the MERGE c=5 term of 9.5 ms disappears. But the subq doc also shows the PARTITION phase falling 98 → 88 ms from fewer allocator calls. P mod N is ~6-10 ms | 8 |
| Q18 3 waves (19) | **hpb4g Q18 −311 ms is not granularity evidence.** The MERGE occupancy loss only goes 20.7 → 14.3 ms with 9 tasks, which are now 9 mod 4 = 1 and imbalanced. The other ~300 ms is clone and allocator cost (#3/#2), since there are 4x fewer clones. The 19 ms itself is a residual (595 − 452 − 53 − 71) | 15 |
| small-table scan quantization (43) | confirmed by occupancy: under-N scans Q2/Q16 give L ≈ 25 ms. The "ramp" part (14.9/7.5/9.1) is start-up latency, not task count | 30 |
| single-GPU tails (19), Q14 1-task-per-GPU join tail (18), Q1 barrier skew (19), part-scan waves (25) | consistent with occupancy terms. Q1 skew is end-of-dynamic-schedule skew (627 tasks), and coarser batches make it *worse* (batch4g range 46 ms vs 31). No remedy listed addresses it | 70 |
| subq tails and bubbles (82) | **Not granularity.** ~45 ms is serial chains that take the *same* time at g1 and g4 (Q20 31.3 vs 32.8 ms): Amdahl latency, which belongs with #7. ~26 ms is dependency bubbles waiting on the DF chain with the link idle, i.e. #5 no-lookahead. ~11 ms is scan residual. Only Q20's 2-task supplier scan (~5) is granularity | 5 |
| small-pipeline floor (40), multijoin idle gaps (14) | latency floor, not task-count dependent. Belongs with #7 | 0 |
| per-task inflation on tiny inputs (16) | `cudaLaunchKernel` mean 6.4 → 29.5 µs and `cudaEventCreate` 3 → 143 thread-ms: driver contention (#2), not granularity | 0 |
| **unclaimed** multijoin/joinship imbalance (Q9 HJ c=21, Q21 HJ c=6, Q10 PARTITION/MERGE, Q9 PARTITION c=12) | ~80 ms L → ~55-70 ms wall, partly overlapping allocator inflation | ~65 |

- Sum of the "keep" column: ≈ 0.19 s from claimed items + 0.065 s unclaimed ≈ **0.25 s**. Less overlap with #2 tail
  inflation: **≈ 0.22 s**.
- About **0.15 s should be re-bucketed**: ~0.10 to #7 serial latency (Amdahl), ~0.03 to #5 lookahead, ~0.02 to #2
  allocator.

## Smaller errors

- The batch8g deltas in the claim are against the **baseline**, not against batch4g:
  - Q2: 0.488 vs 0.373 baseline (+115) and vs 0.396 batch4g (+92).
  - Q11 and Q20 are +76 against batch4g.

## Remedy check (would removing it return the time?)

- **"Round P up to a multiple of N" is net-negative.**
  - Clone cost grows with P × the number of upstream fragments (#3). hpb256 shows it: Q15 +259 ms, Q18 1.56 → 3.67 s.
  - Q15 at 5 → 8 partitions means ~60% more clones: about +50 ms against 8 ms saved.
  - Q18 at 36 → 48 means +33% clones: about +100-150 ms.
  - Round *down* instead, with a larger `hash_partition_bytes`. hpb4g did that and got Q15 −11 ms.
- **"Skip the cross-GPU gather for tiny tails"** saves ≈0 on subq wall. The g4 chains already match g1, so the 3/4 gap is
  serial latency.
- **"Scan batch = min(cfg, bytes/(k·N))"** is correct. It is worth ≈ 25-35 ms at baseline. It matters more *after* batch4g:
  - L for batch4g is 1.03 s and for batch8g 1.73 s, against 0.87 s at baseline.
  - The modN class grows 268 → 488 ms under batch4g.
  - So granularity grows as the #2/#3 fixes coarsen the tasks. It is a trade-off, not an independent bucket.
