# VERIFY (methodology lens): adversarial check of BASELINE_SUITE / PROFILE_SUITE / BASE_REFERENCE / MB_BLOOM / MB_LIFECYCLE

Run 2026-11-18 18:20-18:40. Scripts and raw output: `$R/runs/verify/` (`bin/`, `spot1.out`, `counts.out`, `mb/`, `raw_hot.json`).
Quent of my runs: `$R/telemetry/verify_*/`. Config untouched, repo sources untouched, every GPU / CPU-heavy command under
`flock $R/machine.lock` (one acquisition per run). All my runs rc=0, windows `outcome=ok`, 0 fallback / error lines.

## 0. What I ran

| item | command | reps |
|---|---|---|
| Lock audit (no GPU) | `bin/timeline.py`: start (`q<N>.gpu.txt` mtime) / end (`q<N>.stdout` mtime) of all 274 recorded engine runs, GPU `memory.used` at start, build windows | - |
| Raw timing recompute (no GPU) | `bin/raw_table.py` over every `runtimes.csv` of tip A/B, DF-off A/B, base A/B, confirm block | - |
| Quent concurrency (no GPU) | `bin/conc.py`: tasks of other pipelines active while a site's Publishing Task runs | 6 sites x 3 hot |
| Spot q16 base vs tip | `bin/spot1.sh`: base, tip, tip, base; 6 iterations each (1 warm + 5 hot) | 8 steady samples per side |
| Spot q2 and q16 DF on vs off | on, off, off, on; both sides with `SIRIUS_ENABLE_TEST_OPTIONS=1` (like for like) | 8 steady samples per side |
| MB4 repeat | `bin/mb4.sh` (copy of mblife/run.sh, binary from `wt-mb-life`): G=2 GPUs 0,1 twice, G=2 GPUs 2,3 once, G=4 once | 7 reps (G=4: 3) |
| Build-side cardinalities | `bin/counts.sh`: DuckDB CPU (no Sirius) on the SF1000 parquet, with the runner's query constants (`queries.py`) | 1 |

"Steady" = hot iterations excluding the first one after the warm run (see finding M2).

## 1. Methodology audit

- **M1 lock discipline: confirmed.** 274 engine runs, 0 run/run overlaps, 0 runs starting with > 600 MiB used on any GPU.
  Build windows fall in the gaps between runs to the second (base build 16:11:07-16:17:12; mblife build ends 16:23:24 = next
  run start; bloom bench compile in the gap 16:57:48-16:59:22). The only ambiguous windows are two mblife rebuild logs
  (16:24:55-16:26:58, 16:39:01-16:40:09) that span untimed log-pass runs; the log creation time includes the lock wait, so this
  is not proof of a violation. No timed run (16:48-18:05) is affected. Not provable from files: that every build was locked.
- **M2 warm vs cold: weakened (no effect on conclusions).** Iteration 1 is a second warm-up for several queries, at every build:
  q2 172-177 vs 148 ms, q8 ~1020 vs 937 (tip), q14 770 vs 697, q10, q11, q16, q7. The reported "hot spread 21% / 14% / 10%" is this,
  not noise. Steady-state spread is 1-7 ms for most queries (q13 84 ms, q5/q10/q20 ~30 ms position effects; base q8 87, q22 32, q3 22).
  Suite ratios are unchanged when iteration 1 is dropped: off/tip 1.082 (geomean 1.078), tip/base 1.0075 (geomean 1.0046; with it 1.010 / 1.0075).
- **M3 DF-off confound: refuted as material.** `SIRIUS_ENABLE_TEST_OPTIONS=1` is not registration-only (it also turns on
  `track_units` counters, scan-lowering stats under a mutex, extra setting lookups: `sirius_physical_plan_generator.cpp:2145`,
  `sirius_context.cpp:1781`). Measured effect: none. q16 DF-on with the variable 313 ms vs without 312; q2 148.5 vs 148.5.
- **M4 "critical chain explains all but 1-4 ms" is true by construction** (the backward walk always finds a task) and proves
  nothing by itself. Independent test: while the Publishing Task of q3 j15, q8 j41, q21 j18, q16 j11, q9 j29, q7 j35 runs, other
  pipelines have 0.0 ms of active task time (18 of 18 hot iterations). So the 1:1 claim holds for the Publishing Task.
- **M5 NVTX nesting: no double count.** Sum of the individual `dynfilter::*` ranges equals the reported top-level sum
  (q21: 293.33 + 0.92 + 0.71 + 0.33 + 0.18 + 0.10 = 295.57 vs 295.6).
- **M6 mixed instruments.** "DF-site delta minus HOST-staged" subtracts an nsys number from a Quent number. nsys inflates the
  sync-heavy loop (q21 Publishing Task 317.3 ms in Quent, 323.0 under nsys), so remainders are biased low by a few ms
  (q7 comes out negative). PROFILE table 3.2 also counts q21 j33's HOST-staged finish (26.5 ms) as non-excluded (its "29.0 ms").
  The 45 ms figure is 45 +- ~15 ms.

## 2. Verdicts per key finding

| # | Finding | Verdict | Evidence / corrected number |
|---|---|---|---|
| 1 | DF worth 1.08x on the suite (tip 15.372 s vs DF-off 16.627 s) | **confirmed** | Raw recompute identical; 1.082 with or without iteration 1; A/A < 0.5% on totals |
| 2 | q2 / q16 / q20 / q11 DF net <= 0 is "at or inside noise" | **refuted for q2 and q16** | Spot ABBA, steady: q2 on 148.5 vs off 142.0 ms = **-6.5 ms (-4.4%)**, ranges do not overlap (148-152 vs 141-146). q16 on 313 vs off 307.5 = **-5 ms (-1.7%)**. q2 is the same at base (150 ms) so it is not a stack regression; q16 is (row 4) |
| 3 | Base -> tip: suite 1.010, q3 +5.5%, q8 +4.0% real | **confirmed** | Raw: q3 tip 664-667 vs base 600-653; q8 tip 930-942 vs base 896-905 (steady). Steady deltas q3 +31, q8 +35 ms |
| 4 | "All others within noise" (q16 +2.6%, q5 +1.6%) | **refuted for q16, weakened for q5** | Spot ABBA q16: base 300-309 (median 304), tip 304-317 (median 312): **+8 ms (+2.6%)**, equal to its 8.5 ms Publishing Task. q5: +15 ms steady, positive at all 6 iteration positions (+5..+20), equal to j32's 16.7 ms; not re-run |
| 5 | Regression cause "H03 critical-path placement" | **weakened (relabel)** | The q3 / q8 / q16 / q5 deltas equal the HOST-staged finish that the tip runs inside the Publishing Task (57 / 35.5 / 8.5 / 16.7 ms). Base ran its replicate off the critical path. Mechanism is the synchronous finish before deposit; magnitude here is mostly the excluded bounce |
| 6 | "Base one-shot 1248 ms vs tip 526 ms" (NVTX sums) | **weakened** | Not comparable: tip time is 1:1 wall (M4), base time is mostly off the critical path. Timing says tip is 0.75-1.0% slower |
| 7 | HOST-staged bounce = 600 ms (3.9%) and everything in `accum::finish` is excluded | **weakened: attribution error** | MB4 repeat (3 processes, within 0.3% of the original): PEER 20.25 ms/GB for one target, +2.5 ms/GB per extra source; HOST G=4 one source 60.9 ms/GB = 3.01 x the PEER one-target figure; HOST 4 sources 185.7 ms/GB. So only the ingress legs are bounce excess (~39 ms/GB per non-root source). A 4-GPU P2P machine would still pay about 3 x 20.25 + 3 x 2.5 = **68 ms/GB = 37% of the HOST figure**. Corrected split of the 600 ms: **~215 ms P2P-equivalent (1.4% of suite), ~385 ms bounce excess (2.5%)**. Model, not measurable here (needs 3 P2P targets) |
| 8 | PEER model "q21 ~30 ms, q3 6.6 ms, suite 64-70 ms (0.4%)" | **refuted (arithmetic)** | The rate is per target and was applied once; there are 3 targets. Corrected: q21 ~104 ms (7.2%), q3 ~22 ms (3.4%), q7 ~20 (2.7%), q8 ~15 (1.6%), q10 ~13 (1.6%), q5 ~9 (1.4%), q4 ~7 (2.3%), q16 ~4 (1.2%); suite ~215 ms (1.4%) |
| 9 | "DF construction costs ~45 ms (0.30%) with the bounce excluded" | **refuted as a bound** | It excludes row 7's P2P-equivalent finish. Corrected non-excluded cost: ~65 ms insert on the critical path + ~15 ms fixed + ~215 ms finish = **~295 ms (1.9% of suite)** |
| 10 | H04: q21 insert 26-44 ms on the critical path | **confirmed, upper end** | Pre-finish 56.2 ms vs DF-off site 12.1 ms = 44 ms; Quent remainder 36 ms + nsys bias (M6) = ~41. Use **~40 ms (2.8% of q21)**. Suite insert ~65 ms, not 46 |
| 11 | H06 / H09 "fail / buys nothing" | **weakened** | True for earlier visibility (no scan starts before publication: structural, scans are created after the build site). Not tested: deposit first, finish off the critical path. PROFILE table 2.1 shows slack between publication and first consumer scan (q3 +111 ms, q8 +102, q16 +73, q7 j35 +58, q5 j32 +55, q9 j29 +52, q12 +31, q21 j33 +29, q21 j18 +76), nothing else runs during the Publishing Task (M4), and base, which did this, is 31 / 35 / 8 ms faster on q3 / q8 / q16. Upper bound min(finish, gap) summed: ~330 ms on this machine (mostly bounce), ~170 ms P2P-equivalent. Contention with the build CONCAT's own cross-GPU moves is unmeasured |
| 12 | H02a duplicate candidates: q21 j18, q10 j10, q12 j8 | **refuted** | DuckDB CPU counts match the build rows exactly: q21 j18 = orders with `o_orderstatus='F'` (730,806,711), q10 j10 and q4 j9 = date-filtered orders: unique `o_orderkey`, no duplicates. q12 j8: 31.17M rows, 29.24M distinct (1.07x). **Only q9 j29 is duplicate-heavy**: 43.48M rows, 10.87M distinct `ps_partkey` (4.0x), 9.89M distinct `ps_suppkey` (4.4x): 166 MB could be ~40 MB |
| 13 | MB_BLOOM "read-before-write is the only kernel lever left, for duplicate-heavy builds" | **weakened to dead on TPC-H** | Row 12: the only duplicate-heavy build (q9 j29) has 1.2 ms of insert on the critical path |
| 14 | MB4 numbers (single process, no repeat) | **confirmed** | Row 7 repeat; GPUs 2,3 give the same as 0,1 (20.25 / 22.7 ms/GB) |
| 15 | No probe scan starts before publication; no reservation wait | **confirmed (not re-measured)** | Structural, consistent with M4 |
| 16 | H01 / H08 / H13 not reached; lock, rc, fallback hygiene | **confirmed (not re-measured)** | Log counts consistent across both agents |

## 3. Spot data (ms, iterations in order; first value = warm, second = second warm-up)

```
q16 base1: 459 332 305 303 306 300      q16 tip1: 468 342 315 312 317 315
q16 base2: 471 327 309 308 303 303      q16 tip2: 467 339 304 312 312 311
q16 on1:   468 339 312 304 313 314      q16 off1: 469 334 307 309 310 304
q16 on2:   466 329 315 301 314 313      q16 off2: 463 329 307 299 313 308
q2  on1:   408 172 148 148 151 148      q2  off1: 386 166 142 141 142 146
q2  on2:   408 173 148 149 152 148      q2  off2: 388 165 142 141 142 141
MB4 finish ms/GB: G=2 PEER src=1 20.25, 20.26, 20.25 (GPUs 2,3); src=2 22.85, 22.78, 22.73; G=4 HOST src=1 60.94, src=4 185.70
MB4 256 MB: PEER 5.09 / 5.87; HOST 15.25 / 46.43.  3 GB: PEER 60.7 / 68.4
```

## 4. Not verified

- Row 7 / 8 / 9 / 11 corrections rest on a model: 3 PEER targets cost 3 x one target (root link bound at ~49 GB/s). This box has
  P2P only inside pairs, so 3 PEER targets cannot be measured. The HOST one-source result (3.01 x) supports per-target scaling.
- Row 11 is an inference from published tables plus the base natural experiment; no prototype was built.
- q5 (+15 ms) and q9 (+4 ms) base-vs-tip were not re-run. q22 and q13 base figures are too noisy to judge.
- MB_BLOOM claims (second stream gains nothing, sort-by-block, kernel bound) were not re-run; ncu is still missing.
- H05 probe-side deltas (q9 +36.6 ms etc.) were not re-measured.
