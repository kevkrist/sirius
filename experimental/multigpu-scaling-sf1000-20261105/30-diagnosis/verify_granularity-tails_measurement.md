# Verify rank 8 "granularity-tails" (measurement lens)

Verdict: **OVERSTATED, mildly.** Claim 0.31 s → corrected **0.26 s** (range 0.20-0.33). The sum is right. Two of the
cited experiments do not support the items they are attached to. About 45 ms is residual or allocator time filed as
granularity.

Scripts: `30-diagnosis/verify_gran_meas/{pipes,phase,waves,q1skew}.py`. Data: Quent `00-baseline/g4_a/quent` and
`10-analysis/fam_subq/qf_hpb4g`, plus timings.csv from g4_a, g4_b, x_g4_hpb4g, x_g4_batch4g and x_g4_batch8g.

## Re-derived numbers

- **Sum.** family_map GRAN rows: 37 + 25 + 14 + 19 + 40 + 43 + 19 + 16 + 82 + 11 = **306 ms**. Arithmetic OK. The
  "round-robin 0.04 s included" item is not a separate row.
- **batch8g deltas are mislabelled.** The claim says they are versus batch4g. They are versus the *baseline*.

  | Query | batch8g − baseline (ms) | batch8g − batch4g (ms) |
  |---|---|---|
  | Q2 | +115 | +92 |
  | Q11 | +71 | +76 |
  | Q20 | +74 | +76 |

  Either way, this shows what a coarser batch *would* cost. It is not a gap present in the baseline config (1 GB
  batch). It shows the remedy's risk, not the bottleneck's size. Q13 +120 and Q16 +60 (vs baseline) also regress at
  batch8g and are not cited.
- **hpb4g Q15 −11 ms is NOT "exactly P mod N".** Q15 hot timings: base 0.712 / 0.711 / 0.715, hpb4g 0.706 / 0.693 /
  0.708. Δ = −11 ms, sd ≈ 6. Quent phase spans (3 hot iterations each, base → hpb4g):

  | Phase | Base (ms) | hpb4g (ms) | Δ (ms) |
  |---|---|---|---|
  | PARTITION(3) | 97.1 | 90.1 | **−7.0** |
  | MERGE_GROUP_BY(4), 5 → 4 tasks | 120.0 | 114.8 | **−5.2** |
  | scan | 459.8 | 462.0 | +2.2 |

  - The PARTITION part comes from fewer slices, so fewer allocator calls (#2), as fam_subq itself notes.
  - The P mod N lever is therefore about **5 ms**, not 11. Each of the 4 partitions is 25% larger, so it eats part of
    the tail gain.
  - Baseline merge tail: max − 2nd GPU end is 10.1 / 12.2 / 10.1 ms. GPU0's 2 tasks run *concurrently* on 2 of its
    4 threads, so this is contention, not a second wave.
- **hpb4g Q18 −311 ms is not granularity evidence.** Phase spans, base → hpb4g:
  - PARTITION(5): 321 → 197 (**−124**, fewer slices, allocator #2).
  - MERGE_GROUP_BY(6), 36 → 9 tasks: 592 → 400 (**−192**: 4× fewer clone fragments, #3).
  - The wave effect cannot be separated out. The family priced waves at only 19 ms, as a subtraction residual
    (595 − 452 − 53 − 71).
  - A fluid bound on the baseline merge phase gives span − Σtask/16 = 92 / 80 / 90 ms (iterations 2-4). Correcting
    for wave-3 tasks running faster when alone (165 vs ~225 ms) leaves ≈ 70 ms.
  - So the true wave loss is 19-70 ms. The rest of it sits in FRAG (#3). The waterfall total is unaffected; only the
    split between rows moves.
- **Q1 barrier skew.** Per-GPU scan end, max − mean: 8.3 / 15.5 / 16.7 ms (iterations 2 / 3 / 4), mean **13.5**. The
  claim's 19 came from iteration 3 only, with a different t0.
  - Task counts are 156-157 per GPU (±1 task ≈ 6 ms).
  - The GPU that finishes last changes every iteration (3, 2, 3), so the skew is random, not structural.
- **Items that are not granularity.**
  - small-F9, 16 ms, "per-task launch/event/alloc inflation": `cudaLaunchKernel` mean 6.4 → 29.5 µs and EventCreate
    3 → 143 thread-ms. That is allocator/driver-lock (#2) and H1 (#10), not task count.
  - subq-F5 (82) contains 11 ms of "scan-below-floor residuals" (Q17 3 + Q20 8). These are unexplained, not a
    mechanism.
  - groupby "small-pipeline floor (misc)", 40 ms:
    - Q18 is traceable to phases E + G + I = 5 + 7.5 + 9 = 21.5 ms.
    - Q13's 16 ms is not traceable. Its non-scan, non-DCJ phases give only 3 + 4 + 1 = 8 ms.
    - About 10 ms is residual.
- **Share vs lever (subq tails).** The Q20 tail chain is 31 ms at g4 vs 33 at g1, and Q15 is 17 vs 21. The tails do
  not inflate at g4. Their "gap" is pure Amdahl, 3/4 of a fixed serial chain.
  - "Skip cross-GPU gather" therefore recovers far less than the 82 ms share.
  - Classed with FIXED (#7) by mechanism.
- **Measurement validity: OK.**
  - Prices come from un-profiled Quent critical-path phase spans: wall, not thread-time. Quent overhead is nil.
  - nsys is used only descriptively (Q15 GPU0 merge 159-169 vs 123-129 ms).
  - No warm-up iteration is used, and there is no Q13 or join-mode confound in these items.
  - Small phases carry about ±2-5 ms of noise each.

## Corrected estimate (ms)

| Item | Claim (ms) | Corrected (ms) | Reason |
|---|---|---|---|
| Q1 skew | 19 | 13.5 | 3-iteration mean |
| Q14 join tail | 18 | 18 | |
| Part scan | 25 | 25 | |
| Multijoin gaps | 14 | 14 | |
| Q18 waves | 19 | 19 | range 19-70, the rest is in FRAG |
| Small-pipeline floor | 40 | 30 | Q13 residual removed |
| Small F8 | 43 | 43 | |
| Small F7 | 19 | 19 | |
| Small F9 | 16 | 0 | moved to #2/#10 |
| Subq F5 | 82 | 71 | residual removed |
| Q15 P mod N | 11 | 5 | hpb4g decomposition |
| **Total** | **306** | **≈ 258** | |

- Range is 0.20-0.33 s. The upper end assumes the larger Q18 wave loss.
- The claim's evidence text needs two fixes:
  - Drop "Q18 −311" as granularity evidence.
  - Relabel the batch8g deltas as versus baseline, and present them as the remedy's risk.
