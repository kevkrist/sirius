# Profile suite: Quent + nsys attribution of DF construction at tip (pr2570 `ae939824`), TPC-H SF1000, 4 GPUs

Run 2026-11-18. Build `$R/repo` (release, source untouched). Config `/home/ubuntu/sirius/specific_config.yaml` unmodified.
Raw output `$R/runs/profile/`; scripts `$R/runs/profile/bin/`. Every GPU process ran under `flock $R/machine.lock`.
Suite reference (from BASELINE_SUITE.md): tip total 15372 ms, DF-off 16627 ms.

## 0. What was run / what was reused

| item | source | reps |
|---|---|---|
| Quent, tip | reused `$R/telemetry/baseline_tip`, `baseline_tip_b` (timed, info log, no nsys) | 2 processes x 3 hot iterations = 6 hot per query |
| Quent, DF-off | reused `$R/telemetry/baseline_off`, `baseline_off_b` (`baseline_dfoff*` are failed runs, ignored) | 6 hot per query |
| nsys, tip, new | `bin/nsys_block.sh nsys 3 21 3 7 9 10 8 5 2 4 12` -> `$R/runs/profile/nsys/` (Quent of these: `$R/telemetry/profile_nsys/`) | 1 capture per query, 3 iterations (iter0 warm, 2 hot) |
| nsys, tip, reused | `$R/runs/base/nsys_tip/` (other agent, same recipe): q2 3 5 8 9 11 12 16 17 18 19 20 21 22 | 1 capture each, 2 hot |

New captures: all rc=0, 0 fallback / error / OOM-retry lines, `construction started` = `published` except q2 (15 vs 13: join 42 skipped, as in baseline).
nsys hot walls are within 1-3% of the unprofiled ones (q3 660 vs 656, q21 1466 vs 1437), still: nsys numbers are attribution, not verdicts.

Scripts: `qload.py` (NDJSON loader), `quent_df.py` (per-join build site + consumer scan timeline), `quent_cp.py` (critical chain),
`quent_tip_vs_off.py`, `quent_table.py`, `nsys_query.sh` / `nsys_block.sh`, `nsys_df.py` (same-thread NVTX -> API -> kernel/memcpy
attribution per hot iteration), `nsys_table.py`, `combine.py`. Outputs: `quent/*.json|md`, `nsys_out/q<N>.json` (new),
`nsys_out/basetip_q<N>.json` (reused captures), `nsys_out/tables.md`, `combined.md|json`.

Method notes / limits:
- Quent has no DF events and no row counts. Publication time = end of the last-ending task of the build site (the Publishing Task;
  it is also the longest task in 273 of 276 join-instances). Consumer scans = scans with a DYNAMIC_FILTER operator in the probe
  subtree of the join (plan edges); `*` = not on the direct probe chain. "Rows scanned unfiltered" is therefore answered by task
  start times, not rows.
- Scan work sits in the task's `Preparing` phase (compressed pin decode), so scan start = `Preparing`.
- Critical chain: from query end, take the task that ended last before t, jump to its `Created`, repeat. Unexplained gaps are 1-4 ms
  per query, so the chain covers the whole wall time.
- nsys attribution is same-thread: API calls issued on the range's thread inside the range; kernels and copies follow by correlation id.
  Async kernels run after the range returns (see `insert kernels end after contribute returns`).
- HOST-staged time (excluded) = `accum::finish` wall minus the wait for the task's own insert kernels (forward path) plus
  `replicate_devices` wall. Under the HOST transport every copy inside `accum::finish` is a staging leg; CUPTI labels part of the
  batch copies "PtoP". Assumption, not verified from logs: `replicate_devices` (exact filters) is also staged across the non-P2P pairs.
- "DF-site delta" = tip minus DF-off time of the same pipeline on the critical chain (pipelines aligned by operator-type sequence).
  For hash-path sites the DF-off scatter is not a perfect reference (q7 join 35: DF-off PARTITION 17.9 ms, tip scatter-only ~7.5 ms),
  so the subtraction has an error of several ms there (q7 comes out at -6.6 ms after removing the host loop).

## 1. Headline

1. **Execution is one serial chain of pipelines.** The critical chain explains all but 1-4 ms of every query. Every ms a DF build
   site spends is a ms of query wall time; nothing overlaps it.
2. **No probe scan ever starts before its filter is visible.** In all 276 join-instances (46 joins x 6 hot iterations) the first
   consumer scan task is created after the Publishing Task ends: 0 scan tasks, 0 input bytes scanned unfiltered. The scheduler
   creates the scan after the build site is done (after the build CONCAT in q3 and q21, straight after BUILD_FILTER in q4; gap
   publication -> scan start 0.3 ms to 626 ms), so publishing earlier inside the build (H09) buys nothing on this suite.
3. **DF build sites cost 645 ms of suite wall (4.2%)**; 600 ms of that (3.9%) is the HOST-staged loop (excluded). What remains is
   **~45 ms (0.30% of the suite)**, almost all of it the Bloom insert kernel on the critical path (46 ms by the direct estimate).
4. Outside the host bounce only **q21 (36 ms, 2.5%; 26-44 ms depending on method)** passes the per-query gate clearly; q2 (2.2 ms,
   1.6%) passes on paper but is inside its noise; q3 is 0.7-0.9%.
5. No reservation wait (max 0.05 ms), no second reservation attempt, no downgrade state, all build inputs on the GPU tier, in any
   build-site task.

## 2. Part A: Quent (tip, 6 hot iterations, medians)

### 2.1 Per join: build site and consumer scans
`med` / `last` = median task and last-ending task (Publishing Task) compute ms; `union` = wall covered by the site's tasks;
consumer column = first scan start minus publication (ms) / scan tasks started before publication / (ignore the MB field: it counts
pinned input batches registered before the tasks exist; q22 shows 6000 MB with 0 tasks started).

| q | wall | join | site | path | kinds | bloom MB | tasks | med ms | last ms | sum ms | union ms | resv max ms | pub @ms | consumer scan(s): first prep - pub ms / tasks before pub / out bytes before pub |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 139 | 7 | PARTITION(1) | fwd | small_in_list | 0 | 1 | 0.24 | 0.24 | 0.2 | 0.2 | 0.03 | 1 | GPU_SCAN(3): +0.4 / 0 of 1 / 0MB |
| 2 | 139 | 15 | PARTITION(9) | fwd | small_in_list | 0 | 1 | 0.22 | 0.22 | 0.2 | 0.2 | 0.03 | 3 | GPU_SCAN(11): +0.5 / 0 of 1 / 0MB |
| 2 | 139 | 28 | PARTITION(16) | fwd | hash_in_list | 0 | 1 | 3.24 | 3.24 | 3.2 | 3.2 | 0.02 | 31 | GPU_SCAN(21): +38.6 / 0 of 4 / 0MB |
| 2 | 139 | 25 | PARTITION(19) | fwd | hash_in_list | 0 | 3 | 0.05 | 1.57 | 1.7 | 1.6 | 0.02 | 67 | GPU_SCAN(21): +1.9 / 0 of 4 / 0MB |
| 2 | 139 | 42 | PARTITION(32) | hash | none+none | 0 | 4 | 4.78 | 4.68 | 20.2 | 6.6 | 0.02 | 125 |  |
| 3 | 656 | 8 | PARTITION(2) | fwd | bloom | 60 | 1 | 4.18 | 4.18 | 4.2 | 4.2 | 0.02 | 17 | GPU_SCAN(4): +11.5 / 0 of 8 / 0MB |
| 3 | 656 | 15 | PARTITION(9) | fwd | bloom | 294 | 4 | 5.51 | 57.12 | 72.9 | 57.2 | 0.03 | 184 | GPU_SCAN(11): +111.0 / 0 of 44 / 0MB |
| 4 | 316 | 9 | BUILD_FILTER(1) | hash | bloom | 115 | 10 | 0.64 | 20.97 | 26.4 | 21.3 | 0.05 | 63 | GPU_SCAN(4): +0.1 / 0 of 25 / 0MB |
| 5 | 681 | 10 | PARTITION(4) | fwd | small_in_list | 0 | 1 | 0.30 | 0.30 | 0.3 | 0.3 | 0.02 | 12 | GPU_SCAN(6): +0.3 / 0 of 1 / 0MB |
| 5 | 681 | 18 | PARTITION(12) | fwd | small_in_list | 0 | 1 | 0.28 | 0.28 | 0.3 | 0.3 | 0.02 | 14 | GPU_SCAN(14): +0.5 / 0 of 1 / 0MB |
| 5 | 681 | 25 | PARTITION(19) | fwd | bloom | 60 | 1 | 4.18 | 4.18 | 4.2 | 4.2 | 0.02 | 35 | GPU_SCAN(21): +49.0 / 0 of 7 / 0MB |
| 5 | 681 | 32 | PARTITION(26) | fwd | bloom | 91 | 4 | 0.53 | 16.73 | 18.1 | 16.8 | 0.03 | 199 | GPU_SCAN(28): +54.5 / 0 of 44 / 0MB |
| 7 | 738 | 9 | PARTITION(3) | fwd | small_in_list | 0 | 1 | 0.28 | 0.28 | 0.3 | 0.3 | 0.02 | 2 | GPU_SCAN(5): +0.5 / 0 of 1 / 0MB |
| 7 | 738 | 38 | PARTITION(10) | hash | bloom | 24 | 1 | 3.86 | 3.86 | 3.9 | 3.9 | 0.02 | 12 | GPU_SCAN(17)*: +6.0 / 0 of 1 / 0MB; GPU_SCAN(24)*: +10.9 / 0 of 50 / 0MB; GPU_SCAN(31): +626.0 / 0 of 5 / 0MB |
| 7 | 738 | 21 | PARTITION(15) | fwd | small_in_list | 0 | 1 | 0.31 | 0.31 | 0.3 | 0.3 | 0.02 | 18 | GPU_SCAN(17): +0.4 / 0 of 1 / 0MB |
| 7 | 738 | 28 | PARTITION(22) | fwd | hash_in_list | 0 | 1 | 1.54 | 1.54 | 1.5 | 1.5 | 0.02 | 22 | GPU_SCAN(24): +1.5 / 0 of 50 / 0MB |
| 7 | 738 | 35 | PARTITION(29) | hash | bloom | 292 | 4 | 12.30 | 63.24 | 99.6 | 63.3 | 0.02 | 579 | GPU_SCAN(31): +58.4 / 0 of 5 / 0MB |
| 8 | 925 | 44 | PARTITION(10) | fwd | hash_in_list | 0 | 2 | 1.20 | 2.35 | 2.4 | 2.4 | 0.02 | 17 | GPU_SCAN(15)*: +2.1 / 0 of 1 / 0MB; GPU_SCAN(23)*: +3.6 / 0 of 1 / 0MB; GPU_SCAN(30)*: +45.9 / 0 of 7 / 0MB; GPU_SCAN(37): +281.9 / 0 of 50 / 0MB |
| 8 | 925 | 19 | PARTITION(13) | fwd | small_in_list | 0 | 1 | 0.30 | 0.30 | 0.3 | 0.3 | 0.02 | 19 | GPU_SCAN(15): +0.3 / 0 of 1 / 0MB |
| 8 | 925 | 27 | PARTITION(21) | fwd | small_in_list | 0 | 1 | 0.28 | 0.28 | 0.3 | 0.3 | 0.01 | 20 | GPU_SCAN(23): +0.4 / 0 of 1 / 0MB |
| 8 | 925 | 34 | PARTITION(28) | fwd | bloom | 60 | 1 | 4.14 | 4.14 | 4.1 | 4.1 | 0.02 | 33 | GPU_SCAN(30): +30.0 / 0 of 7 / 0MB |
| 8 | 925 | 41 | PARTITION(35) | fwd | bloom | 182 | 4 | 2.77 | 35.51 | 42.8 | 35.6 | 0.02 | 198 | GPU_SCAN(37): +101.9 / 0 of 50 / 0MB |
| 9 | 1393 | 19 | PARTITION(13) | fwd | bloom | 22 | 3 | 0.15 | 3.58 | 3.8 | 3.7 | 0.02 | 348 | GPU_SCAN(15): +2.6 / 0 of 4 / 0MB |
| 9 | 1393 | 29 | PARTITION(23) | fwd | bloom+bloom | 174 | 4 | 1.15 | 31.86 | 34.6 | 31.9 | 0.02 | 435 | GPU_SCAN(25): +51.7 / 0 of 62 / 0MB |
| 10 | 840 | 10 | PARTITION(4) | hash | bloom | 115 | 7 | 2.23 | 22.53 | 33.3 | 22.7 | 0.02 | 107 | GPU_SCAN(6): +8.8 / 0 of 45 / 0MB |
| 10 | 840 | 21 | PARTITION(15) | hash | bloom | 89 | 4 | 1.56 | 17.37 | 22.0 | 17.4 | 0.03 | 437 | GPU_SCAN(17): +0.7 / 0 of 7 / 0MB |
| 11 | 170 | 7 | PARTITION(1) | fwd | small_in_list | 0 | 1 | 0.28 | 0.28 | 0.3 | 0.3 | 0.03 | 2 | GPU_SCAN(3): +0.4 / 0 of 1 / 0MB |
| 11 | 170 | 14 | PARTITION(8) | fwd | hash_in_list | 0 | 1 | 0.86 | 0.86 | 0.9 | 0.9 | 0.02 | 4 | GPU_SCAN(10): +0.7 / 0 of 5 / 0MB |
| 11 | 170 | 28 | PARTITION(22) | fwd | small_in_list | 0 | 1 | 0.22 | 0.22 | 0.2 | 0.2 | 0.02 | 63 | GPU_SCAN(24): +0.3 / 0 of 1 / 0MB |
| 11 | 170 | 35 | PARTITION(29) | fwd | hash_in_list | 0 | 1 | 0.87 | 0.87 | 0.9 | 0.9 | 0.03 | 65 | GPU_SCAN(31): +0.7 / 0 of 5 / 0MB |
| 12 | 693 | 8 | PARTITION(2) | fwd | bloom | 62 | 44 | 0.06 | 11.22 | 13.8 | 12.1 | 0.04 | 625 | GPU_SCAN(4): +30.9 / 0 of 8 / 0MB |
| 16 | 309 | 11 | PARTITION(5) | fwd | bloom | 59 | 3 | 0.30 | 8.49 | 9.0 | 8.5 | 0.04 | 64 | GPU_SCAN(7): +73.4 / 0 of 2 / 0MB |
| 17 | 485 | 7 | PARTITION(1) | fwd | hash_in_list | 0 | 2 | 0.37 | 0.70 | 0.7 | 0.7 | 0.02 | 10 | GPU_SCAN(3): +0.5 / 0 of 31 / 0MB |
| 18 | 1078 | 14 | PARTITION(8) | fwd | hash_in_list | 0 | 36 | 0.04 | 1.03 | 3.2 | 1.5 | 0.03 | 788 | GPU_SCAN(10): +2.5 / 0 of 10 / 0MB |
| 18 | 1078 | 24 | PARTITION(18) | fwd | hash_in_list | 0 | 4 | 0.05 | 0.57 | 0.7 | 0.6 | 0.02 | 941 | GPU_SCAN(20): +1.7 / 0 of 25 / 0MB |
| 19 | 879 | 9 | PARTITION(2) | fwd | hash_in_list | 0 | 2 | 0.63 | 1.20 | 1.3 | 1.2 | 0.02 | 54 | GPU_SCAN(4): +2.2 / 0 of 81 / 0MB |
| 20 | 708 | 7 | PARTITION(1) | fwd | small_in_list | 0 | 1 | 0.28 | 0.28 | 0.3 | 0.3 | 0.02 | 3 | GPU_SCAN(3): +0.4 / 0 of 1 / 0MB |
| 20 | 708 | 49 | BUILD_FILTER(8) | hash | hash_in_list | 0 | 1 | 0.91 | 0.91 | 0.9 | 0.9 | 0.02 | 20 | GPU_SCAN(14): +56.9 / 0 of 3 / 0MB; GPU_SCAN(27)*: +126.9 / 0 of 31 / 0MB |
| 20 | 708 | 18 | PARTITION(12) | fwd | bloom | 4 | 3 | 0.07 | 0.96 | 1.1 | 1.0 | 0.02 | 76 | GPU_SCAN(14): +0.9 / 0 of 3 / 0MB |
| 20 | 708 | 31 | PARTITION(25) | fwd | hash_in_list+hash_in_list | 0 | 1 | 1.42 | 1.42 | 1.4 | 1.4 | 0.02 | 146 | GPU_SCAN(27): +0.7 / 0 of 31 / 0MB |
| 20 | 708 | 45 | PARTITION(38) | fwd | none+none | 0 | 1 | 0.03 | 0.03 | 0.0 | 0.0 | 0.02 | 692 |  |
| 21 | 1437 | 7 | PARTITION(1) | fwd | small_in_list | 0 | 1 | 0.26 | 0.26 | 0.3 | 0.3 | 0.03 | 4 | GPU_SCAN(3): +0.3 / 0 of 1 / 0MB |
| 21 | 1437 | 21 | PARTITION(8) | fwd | hash_in_list | 0 | 1 | 0.93 | 0.93 | 0.9 | 0.9 | 0.02 | 21 | GPU_SCAN(13): +424.0 / 0 of 31 / 0MB |
| 21 | 1437 | 18 | PARTITION(11) | hash | bloom | 1462 | 5 | 31.80 | 317.30 | 474.3 | 317.3 | 0.03 | 369 | GPU_SCAN(13): +76.0 / 0 of 31 / 0MB |
| 21 | 1437 | 33 | PARTITION(27) | fwd | bloom | 139 | 4 | 1.58 | 26.09 | 30.8 | 26.1 | 0.03 | 929 | GPU_SCAN(29): +28.8 / 0 of 31 / 0MB |
| 22 | 332 | 28 | PARTITION(22) | fwd | bloom | 38 | 4 | 0.13 | 7.15 | 7.6 | 7.2 | 0.04 | 147 | GPU_SCAN(24): +4.1 / 0 of 2 / 6000MB |

Forward-path sites do only DF work, so `sum`/`union` are DF cost. Hash-path task time includes the scatter (split in section 3).
Publishing Task excess (last - median): q21 j18 285 ms, q7 j35 51, q3 j15 52, q8 j41 33, q9 j29 31, q10 20 + 16, q4 20, q5 j32 16.

### 2.2 DF build sites and probe scans on the critical chain, tip vs DF-off

| q | pipeline (tip) | role | tip ms on chain (min-max) | DF-off ms | delta ms |
|---|---|---|---|---|---|
| 2 | PARTITION(1) | DF site | 0.3 (0.3-0.3) | 0.1 | +0.2 |
| 2 | PARTITION(9) | DF site | 0.3 (0.3-0.3) | 0.1 | +0.2 |
| 2 | GPU_SCAN(11) -> DYNAMIC_FILTER(12) | probe scan+DF | 21.4 (21.4-26.5) | 19.3 | +2.1 |
| 2 | PARTITION(16) | DF site | 3.3 (3.3-3.3) | 0.1 | +3.2 |
| 2 | PARTITION(19) | DF site | 1.7 (1.6-1.7) | 0.1 | +1.5 |
| 2 | GPU_SCAN(21) -> DYNAMIC_FILTER(22) | probe scan+DF | 45.2 (45.2-45.3) | 44.0 | +1.2 |
| 2 | PARTITION(32) | DF site | 6.6 (4.6-9.2) | 4.9 | +1.8 |
| 3 | PARTITION(2) | DF site | 4.2 (4.2-4.3) | 0.1 | +4.1 |
| 3 | GPU_SCAN(4) -> DYNAMIC_FILTER(5) | probe scan+DF | 84.4 (84.3-84.6) | 88.5 | -4.0 |
| 3 | PARTITION(9) | DF site | 57.2 (57.1-57.3) | 0.2 | +57.0 |
| 3 | GPU_SCAN(11) -> DYNAMIC_FILTER(12) | probe scan+DF | 316.3 (315.9-316.6) | 327.2 | -11.0 |
| 4 | BUILD_FILTER(1) | DF site | 21.2 (21.1-24.0) | no match | +21.2 |
| 4 | GPU_SCAN(4) -> DYNAMIC_FILTER(5) -> FILT | probe scan+DF | 208.3 (208.0-208.5) | 219.9 | -11.6 |
| 5 | PARTITION(4) | DF site | 0.4 (0.3-0.4) | 0.1 | +0.3 |
| 5 | PARTITION(12) | DF site | 0.3 (0.3-0.4) | 0.1 | +0.3 |
| 5 | GPU_SCAN(14) -> DYNAMIC_FILTER(15) | probe scan+DF | 13.0 (6.4-13.2) | 9.6 | +3.4 |
| 5 | PARTITION(19) | DF site | 4.2 (4.2-4.3) | 0.1 | +4.2 |
| 5 | GPU_SCAN(21) -> DYNAMIC_FILTER(22) | probe scan+DF | 85.3 (85.1-103.7) | 83.2 | +2.1 |
| 5 | PARTITION(26) | DF site | 16.8 (16.8-16.9) | 0.2 | +16.6 |
| 5 | GPU_SCAN(28) -> DYNAMIC_FILTER(29) | probe scan+DF | 398.8 (398.6-399.0) | 369.0 | +29.8 |
| 7 | PARTITION(3) | DF site | 0.3 (0.3-0.4) | 0.1 | +0.3 |
| 7 | PARTITION(10) | DF site | 3.9 (3.9-4.0) | 2.1 | +1.9 |
| 7 | PARTITION(15) | DF site | 0.4 (0.3-0.4) | 0.1 | +0.3 |
| 7 | PARTITION(22) | DF site | 1.6 (1.6-1.6) | 0.1 | +1.5 |
| 7 | GPU_SCAN(24) -> DYNAMIC_FILTER(25) | probe scan+DF | 482.7 (482.3-483.3) | 481.7 | +1.0 |
| 7 | PARTITION(29) | DF site | 63.3 (63.1-64.1) | 17.9 | +45.4 |
| 8 | PARTITION(10) | DF site | 2.4 (2.4-2.5) | 0.1 | +2.3 |
| 8 | PARTITION(13) | DF site | 0.4 (0.3-0.4) | 0.1 | +0.3 |
| 8 | PARTITION(21) | DF site | 0.3 (0.3-0.3) | 0.1 | +0.3 |
| 8 | GPU_SCAN(23) -> DYNAMIC_FILTER(24) | probe scan+DF | 6.4 (6.4-6.4) | 3.6 | +2.8 |
| 8 | PARTITION(28) | DF site | 4.2 (4.2-4.2) | 0.1 | +4.1 |
| 8 | GPU_SCAN(30) -> DYNAMIC_FILTER(31) | probe scan+DF | 86.0 (85.9-86.5) | 83.7 | +2.3 |
| 8 | PARTITION(35) | DF site | 35.6 (35.4-35.7) | 0.1 | +35.4 |
| 8 | GPU_SCAN(37) -> DYNAMIC_FILTER(38) | probe scan+DF | 595.5 (593.3-596.0) | 608.1 | -12.7 |
| 9 | PARTITION(13) | DF site | 3.7 (3.6-3.8) | 0.1 | +3.6 |
| 9 | GPU_SCAN(15) -> DYNAMIC_FILTER(16) | probe scan+DF | 45.7 (45.6-45.9) | 44.0 | +1.6 |
| 9 | PARTITION(23) | DF site | 32.0 (31.9-32.0) | 0.1 | +31.8 |
| 9 | GPU_SCAN(25) -> DYNAMIC_FILTER(26) | probe scan+DF | 714.3 (125.5-726.1) | 677.8 | +36.6 |
| 10 | PARTITION(4) | DF site | 22.6 (22.5-22.7) | 1.4 | +21.2 |
| 10 | GPU_SCAN(6) -> DYNAMIC_FILTER(7) | probe scan+DF | 246.8 (245.7-247.6) | 252.4 | -5.6 |
| 10 | PARTITION(15) | DF site | 17.5 (17.3-17.6) | 1.2 | +16.2 |
| 10 | GPU_SCAN(17) -> DYNAMIC_FILTER(18) | probe scan+DF | 145.0 (140.2-147.2) | 146.1 | -1.2 |
| 11 | PARTITION(1) | DF site | 0.4 (0.3-0.5) | 0.1 | +0.3 |
| 11 | PARTITION(8) | DF site | 0.9 (0.9-1.0) | 0.1 | +0.9 |
| 11 | GPU_SCAN(10) -> DYNAMIC_FILTER(11) | probe scan+DF | 52.7 (52.4-54.8) | 49.9 | +2.8 |
| 11 | PARTITION(22) | DF site | 0.3 (0.3-0.3) | 0.1 | +0.2 |
| 11 | PARTITION(29) | DF site | 0.9 (0.9-1.0) | 0.1 | +0.9 |
| 11 | GPU_SCAN(31) -> DYNAMIC_FILTER(32) | probe scan+DF | 55.0 (53.4-56.6) | 51.2 | +3.8 |
| 12 | PARTITION(2) | DF site | 12.7 (12.3-12.8) | 1.1 | +11.6 |
| 12 | GPU_SCAN(4) -> DYNAMIC_FILTER(5) | probe scan+DF | 21.1 (19.1-22.2) | 33.2 | -12.1 |
| 16 | PARTITION(5) | DF site | 8.6 (8.6-8.6) | 0.1 | +8.5 |
| 16 | GPU_SCAN(7) -> DYNAMIC_FILTER(8) | probe scan+DF | 49.9 (49.8-50.0) | 41.5 | +8.4 |
| 17 | PARTITION(1) | DF site | 0.8 (0.7-0.8) | 0.1 | +0.7 |
| 17 | GPU_SCAN(3) -> DYNAMIC_FILTER(4) | probe scan+DF | 454.0 (453.4-462.2) | 452.8 | +1.2 |
| 18 | PARTITION(8) | DF site | 3.1 (1.5-4.8) | 0.6 | +2.5 |
| 18 | PARTITION(18) | DF site | 0.7 (0.6-0.9) | 0.1 | +0.6 |
| 18 | GPU_SCAN(20) -> DYNAMIC_FILTER(21) | probe scan+DF | 127.6 (126.8-127.8) | 126.2 | +1.4 |
| 19 | PARTITION(2) | DF site | 1.3 (1.2-1.3) | 0.1 | +1.2 |
| 19 | GPU_SCAN(4) -> DYNAMIC_FILTER(5) -> FILT | probe scan+DF | 813.6 (812.6-815.3) | 819.8 | -6.2 |
| 20 | PARTITION(1) | DF site | 0.3 (0.3-0.4) | 0.1 | +0.3 |
| 20 | GPU_SCAN(3) -> DYNAMIC_FILTER(4) | probe scan+DF | 13.2 (12.8-13.3) | 10.8 | +2.4 |
| 20 | BUILD_FILTER(8) | DF site | 1.0 (0.9-1.0) | no match | +1.0 |
| 20 | PARTITION(12) | DF site | 1.0 (1.0-1.2) | 0.1 | +0.9 |
| 20 | GPU_SCAN(14) -> DYNAMIC_FILTER(15) | probe scan+DF | 60.3 (49.0-65.7) | 53.3 | +7.0 |
| 20 | PARTITION(25) | DF site | 1.5 (1.4-1.5) | 0.1 | +1.4 |
| 20 | GPU_SCAN(27) -> DYNAMIC_FILTER(28) | probe scan+DF | 538.0 (537.8-543.0) | 536.6 | +1.4 |
| 20 | PARTITION(38) | DF site | 0.1 (0.1-0.1) | 0.6 | -0.5 |
| 21 | PARTITION(1) | DF site | 0.3 (0.3-0.4) | 0.1 | +0.2 |
| 21 | GPU_SCAN(3) -> DYNAMIC_FILTER(4) | probe scan+DF | 14.9 (14.7-15.0) | 9.3 | +5.6 |
| 21 | PARTITION(8) | DF site | 1.0 (1.0-1.0) | 0.1 | +0.9 |
| 21 | PARTITION(11) | DF site | 317.4 (316.7-317.6) | 12.1 | +305.2 |
| 21 | GPU_SCAN(13) -> DYNAMIC_FILTER(14) -> FI | probe scan+DF | 378.5 (374.7-379.2) | 346.4 | +32.1 |
| 21 | PARTITION(27) | DF site | 26.2 (26.1-26.2) | 0.1 | +26.1 |
| 21 | GPU_SCAN(29) -> DYNAMIC_FILTER(30) | probe scan+DF | 375.4 (374.7-380.0) | 364.7 | +10.6 |
| 22 | PARTITION(22) | DF site | 7.3 (7.2-7.4) | 0.2 | +7.1 |
| 22 | GPU_SCAN(24) -> DYNAMIC_FILTER(25) | probe scan+DF | 121.4 (121.3-121.4) | 109.3 | +12.2 |

Sums over the 17 DF queries: DF sites +645.2 ms (4.2% of the 15372 ms suite), probe scan pipelines +111.0 ms net
(positive deltas only: ~172 ms; negative ones are scans that got faster).
Probe-scan deltas are the scan + DF pipeline as a whole. They include probe work that pays back downstream
(q21: scan 13 +32 ms, but PARTITION 16 + CONCAT 17 go 472 -> 23 ms).

## 3. Part B: nsys

`dynfilter::*` ranges are present under nsys without `enable_nvtx` (HYPOTHESES C6 confirmed): `construction::begin`,
`accum::contribute`, `accum::finish`, `stage::contribute`, `stage::gather`, `replicate_devices`, `apply_output`, marks
`accum::final_commit` / `accum::visible`. **Absent in all 17 captured queries: `bounds::*`, `accum::late_wait`.**

### 3.1 Combined per query (Quent timing + nsys split; nsys values = mean over the hot iterations of all captures of the query)

| q | tip wall ms (Quent, 6 hot) | DF-off wall | DF-site delta on chain ms (tip-off) | % of q | nsys captures (hot iters) | begin ms | contribute host ms | insert kernel GPU ms (sum over GPUs) | insert kernel critical-path ms (max task per site) | accum::finish ms | stage::gather ms | replicate_devices ms | finish-chain syncs | HOST-staged (excluded) ms [min-max] | HOST-staged copy MB | DF-site delta minus HOST-staged ms | % of q | probe-scan delta ms (tip-off) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 139 | 131 | +7.0 | 5.0 | 4 | 1.09 | 0.44 | 0.00 | 0.02 | 0.0 | 0.50 | 4.73 | 41 | 4.7 [4.7-4.8] | 214 | +2.2 | 1.62 | +3.5 |
| 3 | 656 | 657 | +61.1 | 9.3 | 4 | 0.38 | 0.19 | 21.58 | 6.06 | 62.4 | 0.00 | 0.00 | 404 | 56.3 [56.3-56.4] | 3034 | +4.8 | 0.73 | -15.0 |
| 4 | 316 | 674 | +21.2 | 6.7 | 2 | 0.23 | 0.88 | 3.75 | 0.42 | 21.7 | 0.00 | 0.00 | 146 | 20.7 [20.6-20.7] | 1094 | +0.6 | 0.18 | -11.6 |
| 5 | 681 | 693 | +21.3 | 3.1 | 4 | 1.24 | 0.27 | 2.47 | 1.20 | 21.1 | 0.18 | 0.35 | 172 | 20.3 [20.2-20.5] | 1097 | +1.0 | 0.15 | +35.6 |
| 7 | 738 | 941 | +49.3 | 6.7 | 2 | 0.87 | 0.34 | 18.36 | 5.04 | 54.2 | 0.25 | 1.73 | 421 | 55.9 [55.8-56.0] | 2935 | -6.6 | -0.89 | +2.9 |
| 8 | 925 | 935 | +42.5 | 4.6 | 4 | 1.73 | 0.29 | 11.17 | 4.05 | 40.3 | 0.32 | 2.55 | 292 | 39.3 [38.8-40.5] | 2069 | +3.2 | 0.34 | -7.4 |
| 9 | 1393 | 1542 | +35.5 | 2.5 | 4 | 0.44 | 0.26 | 3.92 | 1.21 | 36.1 | 0.00 | 0.00 | 296 | 34.9 [34.9-35.0] | 1825 | +0.5 | 0.04 | +38.2 |
| 10 | 840 | 1136 | +37.5 | 4.5 | 2 | 0.41 | 0.47 | 5.39 | 1.05 | 37.2 | 0.00 | 0.00 | 262 | 37.2 [37.2-37.3] | 1943 | +0.3 | 0.03 | -6.8 |
| 11 | 170 | 169 | +2.2 | 1.3 | 2 | 0.94 | 0.19 | 0.00 | 0.01 | 0.0 | 0.37 | 1.77 | 36 | 1.8 [1.7-1.8] | 61 | +0.4 | 0.25 | +7.3 |
| 12 | 693 | 731 | +11.6 | 1.7 | 4 | 0.21 | 2.30 | 0.79 | 0.02 | 11.4 | 0.00 | 0.00 | 86 | 11.4 [11.4-11.5] | 594 | +0.2 | 0.02 | -12.1 |
| 16 | 309 | 303 | +8.5 | 2.7 | 2 | 0.33 | 0.13 | 0.59 | 0.26 | 8.7 | 0.00 | 0.00 | 70 | 8.4 [8.4-8.4] | 453 | +0.1 | 0.02 | +8.4 |
| 17 | 485 | 518 | +0.7 | 0.1 | 2 | 0.49 | 0.15 | 0.00 | 0.00 | 0.0 | 0.14 | 0.48 | 11 | 0.5 [0.5-0.5] | 15 | +0.2 | 0.04 | +1.2 |
| 18 | 1078 | 1095 | +3.1 | 0.3 | 2 | 0.46 | 1.62 | 0.00 | 0.00 | 0.0 | 1.02 | 0.67 | 64 | 0.7 [0.7-0.7] | 19 | +2.4 | 0.23 | +1.4 |
| 19 | 879 | 881 | +1.2 | 0.1 | 2 | 0.39 | 0.17 | 0.00 | 0.00 | 0.0 | 0.16 | 0.94 | 11 | 0.9 [0.9-1.0] | 37 | +0.2 | 0.02 | -6.2 |
| 20 | 708 | 704 | +3.0 | 0.4 | 2 | 1.90 | 0.29 | 0.05 | 0.03 | 1.0 | 0.28 | 2.25 | 64 | 3.2 [3.2-3.3] | 117 | -0.2 | -0.03 | +11.4 |
| 21 | 1437 | 1550 | +332.5 | 23.1 | 4 | 0.73 | 0.43 | 129.26 | 26.32 | 296.9 | 0.18 | 0.93 | 1950 | 296.3 [292.6-306.9] | 15298 | +36.2 | 2.52 | +48.4 |
| 22 | 332 | 379 | +7.1 | 2.1 | 2 | 0.20 | 0.13 | 0.38 | 0.10 | 7.3 | 0.00 | 0.00 | 86 | 7.2 [7.2-7.2] | 364 | -0.1 | -0.03 | +12.2 |

Suite sums (17 DF queries): DF-site delta 645 ms; HOST-staged 600 ms; remainder 45 ms; insert kernel 198 GPU-ms summed over GPUs,
46 ms on the critical path (max task per site; lower bound when two tasks of a site share a GPU, as in q21).
Variance: Quent site times agree within 0.1-0.3 ms over 6 hot iterations (exceptions: q2 j42 4.6-9.2, q18 j14 1.5-4.8). nsys hot
iterations agree within 0.1 ms on HOST-staged time, except q21 (292.6-306.9 across 2 captures).

### 3.2 Per-query totals, new captures only

Caveat for "DF % of all partition::execute": on the forward path `execute` returns in 0.03 ms and the task then waits for the
async insert kernel, so the range understates DF time; use task wall (3.4) or section 2.

| q | nsys wall ms | DF ranges wall ms (top-level sum) | % wall | DF-site tasks: n, task wall ms | DF wall inside those tasks ms | all partition::execute ms (n) | DF % of all partition::execute | insert kernels GPU ms (add_if_n) | finish-chain wall ms | of which HOST-staged loop (excl.) ms | DF wall minus HOST-staged ms | % wall |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 21 | 1466 [1469.0, 1463.2] | 295.6 [295.7, 295.5] | 20.2 | 11, 512.3 | 294.9 | 700.3 (102) | 42 | 129.25 | 294.4 | 266.6 | 29.0 | 1.98 |
| 3 | 662 [659.7, 663.4] | 62.9 [63.0, 62.9] | 9.5 | 5, 78.3 | 62.6 | 66.4 (61) | 94 | 21.59 | 62.4 | 56.3 | 6.6 | 0.99 |
| 7 | 762 [773.8, 750.0] | 57.4 [57.4, 57.3] | 7.5 | 8, 107.6 | 56.5 | 115.9 (73) | 49 | 18.36 | 56.1 | 49.3 | 8.0 | 1.05 |
| 9 | 1423 [1428.4, 1417.1] | 36.8 [36.9, 36.8] | 2.6 | 7, 39.2 | 36.4 | 325.3 (113) | 11 | 3.92 | 36.1 | 35.0 | 1.9 | 0.13 |
| 10 | 854 [854.9, 853.8] | 38.1 [38.2, 38.0] | 4.5 | 11, 57.2 | 37.7 | 327.0 (84) | 12 | 5.39 | 37.2 | 35.9 | 2.2 | 0.25 |
| 8 | 979 [1020.3, 937.8] | 45.2 [46.1, 44.4] | 4.6 | 9, 51.0 | 43.5 | 48.6 (83) | 89 | 11.17 | 43.2 | 36.3 | 8.9 | 0.91 |
| 5 | 711 [700.8, 720.7] | 22.6 [22.6, 22.6] | 3.2 | 7, 23.4 | 21.9 | 22.1 (69) | 99 | 2.47 | 21.6 | 19.9 | 2.7 | 0.38 |
| 2 | 163 [180.5, 145.1] | 6.7 [6.7, 6.7] | 4.1 | 10, 34.7 | 5.6 | 35.0 (25) | 16 | 0.00 | 5.2 | 0.0 | 6.7 | 4.14 |
| 4 | 325 [323.3, 327.1] | 22.8 [23.0, 22.7] | 7.0 | 10, 27.3 | 22.6 | 68.6 (39) | 0 | 3.75 | 21.7 | 20.7 | 2.2 | 0.66 |
| 12 | 699 [701.4, 697.5] | 13.9 [14.0, 13.8] | 2.0 | 44, 15.0 | 13.7 | 14.0 (56) | 98 | 0.79 | 11.5 | 11.5 | 2.4 | 0.35 |

### 3.3 Range sums per hot iteration (new captures)
API calls per contribution: 8 for one Bloom or exact key (2 `cudaStreamWaitEvent`, `cudaStreamGetDevice`, event create / record /
destroy, 1 kernel launch + nsys's `cuKernelGetName`), 10 for two keys. No sync, no alloc, no copy in any contribute range.
`construction::begin`: 20-31 API calls per construction, no sync; memset is queued, not waited.
All memcpy inside `accum::finish` and `replicate_devices` is cross-GPU staging (excluded). No other memcpy is issued by DF ranges
except `stage::gather` (<= 4 MB per query).


| q | range | n | wall ms | max ms | CUDA API calls (per range) | sync calls / ms | kernels: name n GPU-ms | memset MB | memcpy MB by kind (GPU copy ms) | top APIs |
|---|---|---|---|---|---|---|---|---|---|---|
| 21 | accum::contribute | 9 | 0.33 | 0.06 | 72 (8.0) | 0 / 0.00 | add_if_n 9 129.25 | 0 | - | StreamWaitEvent 18, StreamGetDevice 9, EventCreateWithFlags 9, cuKernelGetName 9, LaunchKernel 9 |
| 21 | accum::finish | 2 | 293.33 | 266.87 | 5014 (2507.0) | 1932 / 274.94 | sirius::op:: 576 2.88 | 0 | DtoH 1527 (28.1); HtoD 1660 (57.5); PtoP 12081 (255.3) | StreamSynchronize 1932, MemcpyBatchAsync 1920, cuKernelGetName 576, LaunchKernel 576, MemPoolGetAccess 4 |
| 21 | construction::begin | 4 | 0.71 | 0.21 | 96 (24.0) | 0 / 0.00 | - | 6113 | - | MallocFromPoolAsync 24, MemsetAsync 24, StreamCreateWithFlags 16, EventCreateWithFlags 16, EventRecord 16 |
| 21 | replicate_devices | 4 | 0.92 | 0.78 | 48 (12.0) | 14 / 0.65 | static_kernel 3 0.01 | 0 | DtoH 0 (0.0); HtoD 3 (0.1); PtoP 27 (0.5) | StreamSynchronize 14, MallocFromPoolAsync 6, StreamGetDevice 6, MemPoolGetAccess 6, MemcpyAsync 4 |
| 21 | stage::contribute | 2 | 0.10 | 0.07 | 16 (8.0) | 0 / 0.00 | static_kernel 2 0.00 | 0 | - | StreamWaitEvent 4, StreamGetDevice 2, cuKernelGetName 2, LaunchKernel 2, EventCreateWithFlags 2 |
| 21 | stage::gather | 2 | 0.18 | 0.10 | 52 (26.0) | 4 / 0.03 | - | 0 | DtoH 0 (0.0); PtoP 0 (0.0) | StreamWaitEvent 10, EventRecord 10, MemcpyAsync 8, EventCreateWithFlags 8, EventDestroy 8 |
| 3 | accum::contribute | 5 | 0.18 | 0.08 | 40 (8.0) | 0 / 0.00 | add_if_n 5 21.59 | 0 | - | StreamWaitEvent 10, StreamGetDevice 5, EventCreateWithFlags 5, cuKernelGetName 5, LaunchKernel 5 |
| 3 | accum::finish | 2 | 62.37 | 58.27 | 1020 (510.0) | 404 / 58.45 | sirius::op:: 108 0.52 | 0 | DtoH 280 (5.2); HtoD 618 (12.3); PtoP 2135 (51.5) | StreamSynchronize 404, MemcpyBatchAsync 392, cuKernelGetName 108, LaunchKernel 108, MemPoolGetAccess 4 |
| 3 | construction::begin | 2 | 0.36 | 0.23 | 40 (20.0) | 0 / 0.00 | - | 1351 | - | StreamCreateWithFlags 8, MallocFromPoolAsync 8, MemsetAsync 8, EventCreateWithFlags 8, EventRecord 8 |
| 3 | replicate_devices | 2 | 0.00 | 0.00 | 0 (0.0) | 0 / 0.00 | - | 0 | - |  |
| 7 | accum::contribute | 5 | 0.22 | 0.09 | 40 (8.0) | 0 / 0.00 | add_if_n 5 18.36 | 0 | - | StreamWaitEvent 10, StreamGetDevice 5, EventCreateWithFlags 5, cuKernelGetName 5, LaunchKernel 5 |
| 7 | accum::finish | 2 | 54.16 | 52.60 | 994 (497.0) | 394 / 50.39 | sirius::op:: 105 0.55 | 0 | DtoH 290 (5.3); HtoD 568 (13.5); PtoP 2016 (45.8) | StreamSynchronize 394, MemcpyBatchAsync 382, cuKernelGetName 105, LaunchKernel 105, MemPoolGetAccess 4 |
| 7 | construction::begin | 5 | 0.87 | 0.19 | 124 (24.8) | 0 / 0.00 | - | 1217 | - | MallocFromPoolAsync 32, MemsetAsync 32, StreamCreateWithFlags 20, EventCreateWithFlags 20, EventRecord 20 |
| 7 | replicate_devices | 5 | 1.73 | 1.40 | 69 (13.8) | 21 / 1.37 | static_kernel 3 0.01 | 0 | DtoH 0 (0.0); HtoD 6 (0.1); PtoP 55 (1.0) | StreamSynchronize 21, MallocFromPoolAsync 9, StreamGetDevice 9, MemPoolGetAccess 9, MemcpyAsync 8 |
| 7 | stage::contribute | 3 | 0.13 | 0.09 | 24 (8.0) | 0 / 0.00 | static_kernel 3 0.00 | 0 | - | StreamWaitEvent 6, StreamGetDevice 3, cuKernelGetName 3, LaunchKernel 3, EventCreateWithFlags 3 |
| 7 | stage::gather | 3 | 0.25 | 0.09 | 78 (26.0) | 6 / 0.04 | - | 0 | DtoH 0 (0.0); PtoP 0 (0.0) | StreamWaitEvent 15, EventRecord 15, MemcpyAsync 12, EventCreateWithFlags 12, EventDestroy 12 |
| 9 | accum::contribute | 7 | 0.26 | 0.04 | 64 (9.1) | 0 / 0.00 | add_if_n 11 3.92 | 0 | - | StreamWaitEvent 14, cuKernelGetName 11, LaunchKernel 11, StreamGetDevice 7, EventCreateWithFlags 7 |
| 9 | accum::finish | 2 | 36.13 | 32.45 | 754 (377.0) | 296 / 33.42 | sirius::op:: 82 0.31 | 0 | DtoH 176 (3.2); HtoD 363 (7.1); PtoP 1285 (30.5) | StreamSynchronize 296, MemcpyBatchAsync 284, cuKernelGetName 82, LaunchKernel 82, MemPoolGetAccess 4 |
| 9 | construction::begin | 2 | 0.43 | 0.28 | 48 (24.0) | 0 / 0.00 | - | 746 | - | MallocFromPoolAsync 12, MemsetAsync 12, StreamCreateWithFlags 8, EventCreateWithFlags 8, EventRecord 8 |
| 9 | replicate_devices | 2 | 0.00 | 0.00 | 0 (0.0) | 0 / 0.00 | - | 0 | - |  |
| 10 | accum::contribute | 11 | 0.47 | 0.08 | 88 (8.0) | 0 / 0.00 | add_if_n 11 5.39 | 0 | - | StreamWaitEvent 22, StreamGetDevice 11, EventCreateWithFlags 11, cuKernelGetName 11, LaunchKernel 11 |
| 10 | accum::finish | 2 | 37.20 | 20.86 | 672 (336.0) | 262 / 34.45 | sirius::op:: 75 0.40 | 0 | DtoH 194 (3.6); HtoD 389 (7.4); PtoP 1360 (32.3) | StreamSynchronize 262, MemcpyBatchAsync 250, cuKernelGetName 75, LaunchKernel 75, MemPoolGetAccess 4 |
| 10 | construction::begin | 2 | 0.41 | 0.24 | 40 (20.0) | 0 / 0.00 | - | 777 | - | StreamCreateWithFlags 8, MallocFromPoolAsync 8, MemsetAsync 8, EventCreateWithFlags 8, EventRecord 8 |
| 10 | replicate_devices | 2 | 0.00 | 0.00 | 0 (0.0) | 0 / 0.00 | - | 0 | - |  |
| 8 | accum::contribute | 5 | 0.14 | 0.03 | 40 (8.0) | 0 / 0.00 | add_if_n 5 11.17 | 0 | - | StreamWaitEvent 10, StreamGetDevice 5, EventCreateWithFlags 5, cuKernelGetName 5, LaunchKernel 5 |
| 8 | accum::finish | 2 | 40.37 | 36.23 | 656 (328.0) | 264 / 37.74 | sirius::op:: 66 0.30 | 0 | DtoH 174 (3.2); HtoD 579 (12.0); PtoP 1215 (29.9) | StreamSynchronize 264, MemcpyBatchAsync 252, cuKernelGetName 66, LaunchKernel 66, MemPoolGetAccess 4 |
| 8 | construction::begin | 5 | 1.71 | 1.84 | 124 (24.8) | 0 / 0.00 | - | 945 | - | MallocFromPoolAsync 32, MemsetAsync 32, StreamCreateWithFlags 20, EventCreateWithFlags 20, EventRecord 20 |
| 8 | replicate_devices | 5 | 2.53 | 2.17 | 69 (13.8) | 21 / 2.17 | static_kernel 3 0.02 | 0 | DtoH 0 (0.0); HtoD 20 (0.4); PtoP 81 (1.5) | StreamSynchronize 21, MallocFromPoolAsync 9, StreamGetDevice 9, MemPoolGetAccess 9, MemcpyAsync 8 |
| 8 | stage::contribute | 4 | 0.14 | 0.04 | 32 (8.0) | 0 / 0.00 | static_kernel 4 0.01 | 0 | - | StreamWaitEvent 8, StreamGetDevice 4, cuKernelGetName 4, LaunchKernel 4, EventCreateWithFlags 4 |
| 8 | stage::gather | 3 | 0.32 | 0.15 | 82 (27.3) | 7 / 0.08 | - | 0 | DtoH 0 (0.0); PtoP 4 (0.1) | StreamWaitEvent 15, EventRecord 15, MemcpyAsync 12, EventCreateWithFlags 12, EventDestroy 12 |
| 5 | accum::contribute | 5 | 0.17 | 0.06 | 40 (8.0) | 0 / 0.00 | add_if_n 5 2.47 | 0 | - | StreamWaitEvent 10, StreamGetDevice 5, EventCreateWithFlags 5, cuKernelGetName 5, LaunchKernel 5 |
| 5 | accum::finish | 2 | 21.09 | 17.02 | 370 (185.0) | 154 / 19.58 | sirius::op:: 33 0.13 | 0 | DtoH 87 (1.6); HtoD 144 (4.8); PtoP 866 (19.3) | StreamSynchronize 154, MemcpyBatchAsync 142, cuKernelGetName 33, LaunchKernel 33, MemPoolGetAccess 4 |
| 5 | construction::begin | 4 | 0.71 | 0.23 | 96 (24.0) | 0 / 0.00 | - | 576 | - | MallocFromPoolAsync 24, MemsetAsync 24, StreamCreateWithFlags 16, EventCreateWithFlags 16, EventRecord 16 |
| 5 | replicate_devices | 4 | 0.35 | 0.18 | 42 (10.5) | 14 / 0.16 | - | 0 | DtoH 0 (0.0); HtoD 0 (0.0); PtoP 0 (0.0) | StreamSynchronize 14, MemcpyAsync 8, MallocFromPoolAsync 6, StreamGetDevice 6, MemPoolGetAccess 6 |
| 5 | stage::contribute | 2 | 0.09 | 0.07 | 16 (8.0) | 0 / 0.00 | static_kernel 2 0.00 | 0 | - | StreamWaitEvent 4, StreamGetDevice 2, cuKernelGetName 2, LaunchKernel 2, EventCreateWithFlags 2 |
| 5 | stage::gather | 2 | 0.18 | 0.10 | 52 (26.0) | 4 / 0.03 | - | 0 | DtoH 0 (0.0); PtoP 0 (0.0) | StreamWaitEvent 10, EventRecord 10, MemcpyAsync 8, EventCreateWithFlags 8, EventDestroy 8 |
| 2 | construction::begin | 5 | 1.11 | 0.39 | 156 (31.2) | 0 / 0.00 | - | 72 | - | MallocFromPoolAsync 48, MemsetAsync 48, StreamCreateWithFlags 20, EventCreateWithFlags 20, EventRecord 20 |
| 2 | replicate_devices | 4 | 4.72 | 3.10 | 96 (24.0) | 28 / 4.11 | static_kernel 6 0.04 | 0 | DtoH 0 (0.0); HtoD 27 (0.5); PtoP 186 (3.5) | StreamSynchronize 28, MallocFromPoolAsync 12, StreamGetDevice 12, MemPoolGetAccess 12, MemcpyBatchAsync 8 |
| 2 | stage::contribute | 10 | 0.42 | 0.06 | 88 (8.8) | 0 / 0.00 | static_kernel 14 0.03 | 0 | - | StreamWaitEvent 20, cuKernelGetName 14, LaunchKernel 14, StreamGetDevice 10, EventCreateWithFlags 10 |
| 2 | stage::gather | 4 | 0.49 | 0.27 | 116 (29.1) | 13 / 0.16 | - | 0 | DtoH 0 (0.0); PtoP 3 (0.1) | StreamWaitEvent 20, EventRecord 20, MemcpyAsync 17, EventCreateWithFlags 16, EventDestroy 16 |
| 4 | accum::contribute | 10 | 0.88 | 0.13 | 80 (8.0) | 0 / 0.00 | add_if_n 10 3.75 | 0 | - | StreamWaitEvent 20, StreamGetDevice 10, EventCreateWithFlags 10, cuKernelGetName 10, LaunchKernel 10 |
| 4 | accum::finish | 1 | 21.70 | 21.72 | 375 (375.0) | 146 / 20.20 | sirius::op:: 42 0.16 | 0 | DtoH 109 (2.0); HtoD 219 (4.2); PtoP 766 (18.2) | StreamSynchronize 146, MemcpyBatchAsync 140, cuKernelGetName 42, LaunchKernel 42, MemPoolGetAccess 2 |
| 4 | construction::begin | 2 | 0.23 | 0.24 | 20 (10.0) | 0 / 0.00 | - | 438 | - | StreamCreateWithFlags 4, MallocFromPoolAsync 4, MemsetAsync 4, EventCreateWithFlags 4, EventRecord 4 |
| 4 | replicate_devices | 1 | 0.00 | 0.00 | 0 (0.0) | 0 / 0.00 | - | 0 | - |  |
| 12 | accum::contribute | 44 | 2.20 | 0.18 | 352 (8.0) | 0 / 0.00 | add_if_n 44 0.79 | 0 | - | StreamWaitEvent 88, StreamGetDevice 44, EventCreateWithFlags 44, cuKernelGetName 44, LaunchKernel 44 |
| 12 | accum::finish | 1 | 11.48 | 11.48 | 219 (219.0) | 86 / 10.53 | sirius::op:: 24 0.09 | 0 | DtoH 59 (1.1); HtoD 59 (1.7); PtoP 476 (10.5) | StreamSynchronize 86, MemcpyBatchAsync 80, cuKernelGetName 24, LaunchKernel 24, MemPoolGetAccess 2 |
| 12 | construction::begin | 1 | 0.23 | 0.25 | 20 (20.0) | 0 / 0.00 | - | 238 | - | StreamCreateWithFlags 4, MallocFromPoolAsync 4, MemsetAsync 4, EventCreateWithFlags 4, EventRecord 4 |
| 12 | replicate_devices | 1 | 0.00 | 0.00 | 0 (0.0) | 0 / 0.00 | - | 0 | - |  |

### DF-site tasks (iteration 2 of each capture; tasks with task wall >= 1 ms or publishing)

| q | task | op | publishing | task wall ms | execute range ms | DF ranges ms | contribute ranges ms (API calls) | insert kernel GPU ms | insert kernels end after contribute returns ms | pre-finish ms (hash path: scatter) | finish chain ms | syncs (ms) in task | HOST/Peer copy MB | post-finish ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|

### 3.4 DF-site tasks (hot iteration 2 of each new capture)
Full table: `$R/runs/profile/nsys_out/tables.md` (third table). Key rows:

| q / join | path | task | task wall ms | insert kernel ms | pre-finish ms | finish chain ms | syncs |
|---|---|---|---|---|---|---|---|
| 21 j18 | hash | non-publishing (x4) | 31.7 | 24.7 | - | - | 4 |
| 21 j18 | hash | Publishing | 323.0 | 24.3 | 56.2 | 266.8 | 1761 |
| 21 j33 | fwd | non-publishing (x3) | 1.56 | 1.53 | - | - | 0 |
| 21 j33 | fwd | Publishing | 26.5 | 1.53 | 0.05 | 26.5 | 177 |
| 3 j15 | fwd | non-publishing (x3) | 4.7-5.5 | 4.6-5.5 | - | - | 0 |
| 3 j15 | fwd | Publishing | 58.3 | 5.4 | 0.04 | 58.2 | 367 |
| 7 j35 | hash | non-publishing (x3) | 11.9-12.8 | 4.4-4.8 | - | - | 25 |
| 7 j35 | hash | Publishing | 64.4 | 4.4 | 11.9 | 52.5 | 382 |
| 8 j41 | fwd | Publishing | 36.3 | 3.4 | 0.03 | 36.2 | 227 |
| 9 j29 | fwd | Publishing | 32.5 | 1.1 | 0.05 | 32.4 | 227 |
| 10 j10 / j21 | hash | Publishing | 23.2 / 17.7 | 0.6 / 0.4 | 2.3 / 1.3 | 20.8 / 16.3 | 151 / 121 |
| 4 j9 | BUILD_FILTER | Publishing | 21.9 | 0.4 | 0.1 | 21.7 | 147 |
| 12 j8 | fwd, 44 batches | Publishing | 11.5 | 0.02 | 0.03 | 11.5 | 87 |
| any 1-5 row IN-list | fwd | Publishing | 0.31-0.38 | 0 | 0.04 | 0.23-0.28 | 10 |

- **H04 confirmed in the engine.** Hash path: the insert kernel runs on the task stream ahead of the scatter and the task waits for
  both (q21 j18: 24.7 of 31.7 ms per task; q7 j35: 4.4 of 12 ms). Forward path: task wall = insert kernel (q3 j15 5.5 ms).
  q21 j18 Publishing Task needs 56 ms before `finish` starts while the DF-off site takes 12.1 ms in total: 5 tasks on 4 GPUs, so
  two inserts queue on one GPU.
- **H06 / H09 hold before deposit**: the Publishing Task holds for the whole finish chain (table), then returns in 0.01-0.02 ms.
  With Bloom keys the chain is the HOST-staged loop (about 2 syncs per chunk leg; q21 1932 syncs, q3 404). Without Bloom keys it is
  0.23-0.28 ms and 10 syncs per join. Hash-path "publish after scatter" delay = pre-finish column (q21 56 ms, q7 12 ms), but no
  consumer can use an earlier filter (section 1.2).

## 4. HOST-staged bounce (excluded; report only)

600 ms over the 17 DF queries = 3.9% of the suite; 93% of all DF build-site time. Per query (ms / % of query): q21 296 / 20.6,
q3 56 / 8.6, q7 56 / 7.6, q8 39 / 4.2, q10 37 / 4.4, q9 35 / 2.5, q4 21 / 6.5, q5 20 / 3.0, q12 11 / 1.6, q16 8.4 / 2.7,
q22 7.2 / 2.2, q2 4.7 / 3.4, q20 3.2, q11 1.8, q19 0.9, q18 0.7, q17 0.5. Copy volume 31 GB per suite pass (q21 15.3 GB).
It is on the critical path in every case (section 1.1). Rate 0.19-0.20 ms per MB of Bloom with 3 sources.
For scale only (model, not measured here): MB4 on the PEER pair gives 20 ms per GB per target, i.e. about 30 ms for q21, 6.6 ms for
q3, 64-70 ms for the suite's 3.19 GB if a 4-GPU P2P machine behaves like the 2-GPU microbenchmark.

## 5. Part C: measured ranking (host bounce excluded)

Gate: >= 1% of a query or >= 0.5% of the suite (15372 ms).

| Rank | Hyp. | Measured cost, host bounce excluded | Gate | Queries |
|---|---|---|---|---|
| 1 | H04 insert kernel on the critical path | 46 ms suite (0.30%) lower bound. q21 26 ms (1.8%) lower bound, 36-44 ms (2.5-3.0%) from tip vs DF-off; q3 6.1 (0.9%), q7 5.0 (0.7%), q8 4.1 (0.4%), q9 1.2, q5 1.2, q10 1.1 | **pass (query)** | q21; q3 just below |
| 2 | H05 filters built but not useful | Probe side (pre-existing mechanism, upper bounds): q9 scan 25 +36.6 ms (2.6%, both keys of j29, one keeps 98.9%); queries with DF net <= 0: q16 +8.4 (2.7%), q20 +11.4 (1.6%), q11 +7.3 (4.3%), q2 +3.5 (2.5%); together ~67 ms (0.44%). Build side without host bounce: < 1.5 ms per filter (q9 key 2 ~0.6 ms, q8 j41 3.4 ms) | **pass (query)**, probe cost only | q9, q16, q20, q11, q2; q5 (+29.8 ms at scan 28, benefit not separated) |
| 3 | H02 Bloom bytes | Nothing measurable here outside the host bounce: memset queued, not waited (q21 6.1 GB); insert speed flat above ~768 MiB (MB_BLOOM). H02a: no saving (baseline). PEER model only: q21 ~30 ms (2.1%), q3 6.6 (1.0%), suite 64-70 ms (0.4%); 8 bits/key would halve that | model only: q21, q3 | not measured |
| 4 | H03 forward path / fixed cost per join | 0.2-0.3 ms per tiny IN-list join (10 syncs) + begin 0.2-0.4 ms. q2 2.2 ms (1.6%, inside its +-2% noise), q18 2.4 ms (0.2%), q11 0.4 ms. Suite < 10 ms (< 0.07%) | marginal (query) | q2 |
| 5 | H07 eager leases | begin 0.2-0.4 ms per construction, 12 ms over 17 queries (0.08%); q2 1.1 ms (0.8%), q20 1.9 ms (0.3%). No reservation wait, retry or downgrade | fail | - |
| 6 | H08 many small batches | 8-10 API calls, 0.03-0.05 ms per contribution. q12 (44 batches) 2.2 ms summed over threads (0.3%); q18 (36) 1.6 ms + gather 1.0 ms (0.2%) | fail | - |
| 7 | H06 serial finish chain | Non-Bloom chain 0.23-0.28 ms and 10 syncs per join: < 11 ms suite (< 0.07%). Bloom chain is the excluded loop | fail | - |
| 8 | H14 mutex across begin | begin max 0.39 ms (one 1.84 ms outlier in q8) | fail | - |
| - | H01 declined / skipped | not reached (no decline). q2 j42 skipped attempt: +1.8 ms on its site, inside its 4.6-9.2 ms run-to-run range | - | - |
| - | H09 publish after scatter | not reached: no consumer starts before the build site is done. Delay itself: q21 56 ms, q7 12 ms | - | - |
| - | H10 scratch with history | not reached: all filters published, no OOM retry, no reservation retry | - | - |
| - | H11 dead attempt retention | not reached (only q2 j42 / q20 j45, tiny) | - | - |
| - | H12 chunk size | only inside the excluded loop here; killed on PEER by MB4 | - | - |
| - | H13 bounds | not reached: no `bounds::*` range in 17 queries | - | - |

Bottom line: with the host bounce excluded, DF construction mechanics cost about 0.3% of the suite. The only construction item
above the gate is the insert kernel in q21 (H04). The larger measured DF costs are the probe-side cost of low-value filters (H05,
pre-existing consumer mechanism) and the excluded host bounce (3.9% of the suite).

## 6. Unverified / caveats

- Critical-path insert for q21 is a range (26-44 ms): task-to-GPU placement was not read out; the upper figure assumes two inserts
  serialize on one GPU.
- H05 probe deltas are whole scan pipelines, tip vs DF-off; per-filter probe cost and per-filter benefit are not separated, and the
  consumer gate state is not logged.
- Transport of `replicate_devices` copies not confirmed as HOST-staged from logs (classified as excluded per HYPOTHESES section 1).
  If it is not, add at most 4.7 ms (q2), 2.6 ms (q8), 2.3 ms (q20) to the non-excluded side.
- nsys: 2 hot iterations per capture, 1-2 captures per query; q4, q7, q10 have one capture. q11, q16-q20, q22 only from the reused
  captures. q1, q6, q13-q15 have no DF.
- PEER figures are a model from the 2-GPU microbenchmark, not a measurement on a 4-GPU P2P machine.
- Not DF, seen in passing: CONCAT after a broadcast PARTITION spends 60-110 ms in `Preparing` (q3 CONCAT 10: 110 ms; q21 CONCAT 12:
  76 ms tip, 78 ms DF-off) with `convert:gpu_to_gpu:host_staging` ranges. Same at DF-off.
