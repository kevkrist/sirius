# Verify #5 "serialized-pipelines-link-idle" (measurement lens)

Verdict: **OVERSTATED**. The mechanism is real (scan creation is serialized behind exchange barriers). Two things
are wrong: the 1.20 s price and part of the stated mechanism. **Corrected share: about 0.70 s (range 0.5-1.2) of
the 11.08 s gap.**

Scripts (CPU only) are in `/tmp/claude-1000/-home-ubuntu-sirius/69d4bd72-ffc6-4ef4-9539-2768e3641f5c/scratchpad/v5/`:
- `timeline.py`: per-pipeline Quent timelines.
- `phase_gp2.py` / `phase_gp3.py`: prefetch bound priced per scan phase against gpupin.
- `gapcover.py`: what actually occupies the nsys link gaps.
- `qgp/`: flattened `x_g4_gpupin` Quent.

## 1. What holds up

- **Code.**
  - `task_scheduler.cpp:209-221` schedules only `scans.front()`.
  - `creator/config.hpp:101,106` uses `active` / `source`.
  - `sirius_scan_manager.cpp:2152-2157` skips the prefetcher when more than one GPU is eligible.
- **Timelines (Quent g4_a iter3, un-profiled).** The scan HOST tasks are created only after the preceding CONCAT ends:
  - Q12: orders scan created at 886 ms, after PARTITION 637-776 and CONCAT 777-886.
  - Q18: scan(10) created at 1260 ms, after MERGE_GROUP_BY 660-1255.
  - Q21: scans created at 124, 800 and 1341 ms, each right after a CONCAT.
- **nsys gap totals reproduced** (g4, iter2, map windows, busier switch, scan copies ≥256 KiB): 4648 ms of gaps of
  5 ms or more, 98 gaps (claim: 4610 / 95). Link-idle 8.05 vs 10.25 s also reproduces: (1 − 0.598) × 21.78 × 0.92
  and (1 − 0.713) × 36.86 × 0.97.

## 2. Problems found

### P1. The memory_prefetcher is off at both GPU counts, so it does not explain the g1→g4 difference

`scan_manager/config.hpp:172` has `memory_prefetcher_config::enable{false}`. No run config (g1_a, g4_a, any x_*)
sets it. Neither g1 nor g4 logs contain a `memory_prefetcher` line. The `!= 1` skip at
`sirius_scan_manager.cpp:2152` therefore never runs. g1 has the same serialization and no prefetch. This is a design
property present at both counts, so only its *differential* cost belongs in the gap.

### P2. The 1.20 s is an accounting residual (family per-phase link price 4.68 − per-query floor 3.48), and 0.48 s of it is not this mechanism

The per-query split of fam_link − hw_floor comes from `link_check.py`, and it sums to 1202 ms:

- **Q13: 270 ms.** Q13 has a single big scan: GPU_SCAN(2), 94.6 GB. Its g4 interior gaps total 3 ms and its tail is
  324 ms, so there is no later scan to look ahead to. The residual exists because T1 is inflated by the cold g1
  iteration (3.92 s; warm iterations take 2.47 and 2.65 s), which lowers the floor.
- **Q1: 61 ms and Q6: 2 ms.** Both are single-scan queries.
  - The X3 floor assumes the blocking post-scan tail overlaps the scan. That is unattainable, so this belongs to the
    hardware link.
  - With these moved, #1 becomes 3.48 + 0.33 = 3.81 s. That matches the gpupin measurement of 3.82 s.
- **Q3: 178 ms, of which about 150 is the join-mode confound.**
  - Q3's floor is 0 only because T1 = 2.03 s includes the g1 STANDARD join. x_g1_joinpol drops it by 0.87 s.
  - With equal policy, T1/4 ≈ 0.29 against a scan floor of 0.44, which gives a floor of about 0.15 and a residual of
    about 30 ms.
  - The waterfall credits only −0.02 s for the join-mode confound.
- **Remaining genuinely multi-scan contributions: about 0.72 s.** The main ones are Q21 175, Q9 150, Q18 121 and
  Q10 103.

### P3. The per-query list mixes methods and units

The list "Q21 0.83, Q18 0.40, Q10 0.36, Q9 0.26 …":
- uses X3 nsys gap-fill bounds;
- is profiled;
- is ΔT4, not Δgap;
- assumes every copy-ms after a gap is saved (s = 1).

Those eight values sum to 2.31 s, which cannot be the per-query breakdown of a 1.20 s share.

### P4. The X3 Δgap bound (1.65) subtracts an inconsistent g1 term, and the "realistic ≈1.0" is arithmetically 0.83

- g4 2.5 excludes Q1 (thread-bound gaps in a single scan), but g1 3.4 includes Q1's 925 ms of the same kind.
- Half of the bound is 1.25 − 1.70/4 = **0.83**, not 1.0.

### P5. Better lever measurement: per scan phase, g4 vs gpupin (Quent, un-profiled, iters 2-4)

- **Method.**
  - The per-scan saving is max(0, scan-phase duration at g4 − the same pipeline under gpupin). Lineitem and orders
    stay compressed on the GPU there, so the decode is still paid.
  - That saving is capped by the cumulative scan-idle time before the scan is created, net of what earlier scans
    already used.
  - The first scan gets nothing.
- **Why per phase instead of per query.** Whole-query gpupin is unusable for Q9, Q10 and Q21:
  - their gpupin PARTITION tasks become HOST-origin (spilled), for example Q21 PARTITION(16) with 471/627 HOST tasks
    and Q9 PARTITION(27) with 627/627;
  - Q21's scan phases still shrink 372→195, 203→83 and 374→133 ms.
- **ΔT4 upper bound (perfect prefetch, unlimited memory): 1.72 s.**
  - Q21 442, Q10 247, Q9 246, Q18 172, Q8 152, Q5 106, Q4 98, Q14 88 ms.
  - Q1, Q6, Q13 and Q7 are 0. Q12 is 22, because its 250 ms of gap is followed by only a 74 ms scan.
- **The same bound at g1** uses gpupin compute × 4 (or × 2.5) as the g1 compute: **1.26-1.51 s**.
- **Δgap upper bound** = 1.72 − (1.26..1.51)/4 = **1.34-1.40 s.**

### P6. The link is not fully idle in the gaps

In the g4 gaps of 5 ms or more, host-staged shuffle H2D occupies 626 of 4648 ms (13.5%), with D2H at 582 ms:
- Q9: 93 of 262 ms (35%);
- Q14: 32 of 95 ms;
- Q18: 228 of 1271 ms;
- Q21: 120 of 827 ms.

Kernels run during only 851 of the 4648 gap-ms (18%), so the exchange phases are host- or latency-bound. That is the
#2 allocator and #3 clone territory.

Lookahead would add concurrent HOST prepares, and each one makes stream-ordered alloc/free calls into the same
process-wide allocator lock (#2). x_g4_thr8 shows that extra concurrency costs +1.19 s at g4. So the realistic
fraction of the bound is 0.5 or less.

## 3. Corrected price

| method | Δgap (s) |
|---|---|
| Claim (fam − floor) | 1.20 |
| Same, without single-scan queries (0.33 → #1) and the Q3 join confound (0.15 → #11) | **0.72** |
| Per-phase gpupin bound, after the g1 differential | ≤1.34-1.40 |
| Same, minus staging occupancy of gaps (~13.5%) | ≤1.2 |
| Realistic: about half of the bound (memory headroom, allocator contention per thr8) | ~0.6 |

**Corrected: 0.70 s (range 0.5-1.2), about 6.3% of the gap.** The waterfall still closes when 0.33 s moves to #1
(giving 3.81, which matches gpupin's 3.82) and 0.15 s moves to #11 (join-mode confound).

Corrected top queries (ΔT4 bound, un-profiled): Q21 0.44, Q10 0.25, Q9 0.25, Q18 0.17, Q8 0.15, Q5 0.11, Q4 0.10,
Q14 0.09.

Correct the mechanism text: lookahead or eager creation is the lever. The prefetcher's multi-GPU skip is not a g4
cause, because the prefetcher is disabled by default.
