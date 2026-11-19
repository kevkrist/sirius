# VERIFY_PROTO_OP1B: adversarial verification of OP1 design B (`proto/op1-minimal`, `$R/wt-op1b`, notes PROTO_OP1B.md)

2026-11-18/19. Verifier only: prototype worktree and binaries not modified (read-only use of `$R/wt-op1b` build `20:27` =
HEAD `e6cec1c3`, tree clean; tip = `$R/repo` `ae939824`). Scripts `$R/runs/vop1b/bin`, raw output `$R/runs/vop1b/<label>`,
Quent `$R/telemetry/vop1b_<label>/`. Every GPU process under `flock $R/machine.lock` (one hold per query: 4 fresh
processes back to back). Config file untouched; only env vars (`SIRIUS_DF_DEFER_PUBLISH`, `SIRIUS_PROTO_NUM_GPUS`,
`CUDA_VISIBLE_DEVICES`, `SIRIUS_LOG_LEVEL`).

## 0. Verdict

| claim | verdict | corrected number |
|---|---|---|
| Gain on this machine (4 GPUs, HOST-staged): q3 -26, q8 -27, q7 -29, q9 -34, q21 -24 | **confirmed** | q3 -25.7, q8 -27.7, q7 -26.3, q9 -36.9 (+-10), q21 -25.0 (my block); pooled with theirs: -26.0 / -27.5 / -28.0 / -30 +-8 / -23.7 |
| Controls flat (q4, q10) | **confirmed** | q4 -0.5; q10 -5.9 (mine), +2.2 (theirs), pooled 0 +- 8 |
| Suite -168 ms (13 queries) = -1.1% | **confirmed for the 7 queries re-run** (-148 ms vs their -138 on the same 7); other 6 queries (-30 ms) and the 9 never-run queries: see section 2.3 | suite about -1.1% stands, still partial |
| Bounce excluded (PEER pair): no gain | **confirmed, slightly worse**: no gain, and q3 is consistently slower | q3 +8.9 (mine), pooled +4.5 (4 of 4 proto processes above all 4 reference processes); q21 -7.9 (mine), pooled -3 +- 5 |
| "All of the gain is bounce time" | **confirmed, by a new control**: same 2-GPU plan on the non-P2P pair (GPUs 0,2: HOST staging) gains q3 -17.2 (2.7%), q21 -57.5 (3.6%); on the P2P pair (GPUs 0,1) nothing | section 3 |
| Results identical, filters identical, no scan before its filter | **confirmed** with one wording correction (Quent flags 2 scan tasks 6-10 us before the Publishing Task's *end event*; not a lost filter, section 4.3). 5 more queries covered (q17 18 19 20 22) | |
| Unit tests: 671/673 with deferral off, 660/673 with deferral on, 11 failures "encode the old order" | **confirmed, but weakened in meaning**: the 11 are exactly the tests that exercise the failure paths (OOM retry, optional scratch OOM, error drain during publication). Under the new order those paths have **zero** test coverage, not "explained failures" | section 4.4 |
| Recommendation: drop for the stack | **agree** | |

## 1. What ran (all under the lock)

| label (`$R/runs/vop1b/`) | what | arms, order | queries |
|---|---|---|---|
| `v1_*` | timing, 4 GPUs, 1 warm + 5 hot, first hot dropped, 8 hot per arm | tip, pro, pro, tip (q3 7 21 4) and pro, tip, tip, pro (q8 9 10) | 3 8 7 9 21 + controls 4 10 |
| `x1_g2*` | timing, GPUs 0-1 only (P2P pair, PEER transport: 0 "HOST staging" lines in 12 / 24 publications) | mode 1, mode 0, mode 0, mode 1 (q3); reversed (q21) | 3 21 |
| `x1_h2*` | **new control**: `CUDA_VISIBLE_DEVICES=0,2` + 2 GPUs (non-P2P pair: every Bloom publication "through HOST staging", 12 of 12 / 12 of 12) | mode 0, mode 1, mode 1, mode 0 (q3); reversed (q21) | 3 21 |
| `logpass` | prototype (mode 1) TRACE pass, 2 iterations, compared with the tip pass `runs/baseline/logpass` | pro | 3 7 8 9 10 21 + never-run 17 18 19 20 22 |
| `unit_m1.txt` | `sirius_unittest "[dynamic_filter]"`, prototype build, mode 1 | | |
| `edge`, `v2_*` | **not run**: edge-path smoke (q3 / q7 x20, interrupt sweep, empty / tiny / LIMIT / semi joins; `bin/edge.sh`, `bin/edge.py`) and timing of q5 q12 q2 were queued at 00:43 behind 84 jobs (about 3 h at 2 min per job) and cancelled unserved | | |

Reference arm of the 2-GPU runs is the prototype binary in mode 0 (their `m0` block shows mode 0 = tip within 2.4 ms;
not re-run). Lock: the queue is FIFO and was 45-95 deep; first job served 113 min after submission.

## 2. Timing, this machine (4 GPUs, HOST-staged finish)

### 2.1 My block (`bin/times.py v1_tip1,v1_tip2 v1_pro1,v1_pro2 ...`)

| q | tip med ms (n, min..max) | per process | proto med ms (n, min..max) | per process | delta | their delta |
|---|---|---|---|---|---|---|
| 3 | 659.5 (8, 657..672) | 659/661 | 633.9 (8, 629..700) | 636/632 | **-25.7** (3.9%) | -26.2 |
| 8 | 936.1 (8, 928..940) | 933/937 | 908.5 (8, 900..911) | 906/908 | **-27.7** (3.0%) | -26.9 |
| 7 | 745.5 (8, 740..751) | 746/744 | 719.2 (8, 716..725) | 719/720 | **-26.3** (3.5%) | -28.8 |
| 9 | 1429.8 (8, 1401..1444) | 1419/1430 | 1392.9 (8, 1374..1413) | 1409/1387 | **-36.9** (2.6%) | -34.3 |
| 21 | 1441.5 (8, 1430..1458) | 1446/1440 | 1416.5 (8, 1396..1430) | 1420/1414 | **-25.0** (1.7%) | -24.4 |
| 4 (control) | 326.5 (8, 322..333) | 325/328 | 326.0 (8, 323..328) | 326/326 | -0.5 | -0.1 |
| 10 (control) | 845.0 (8, 840..876) | 844/856 | 839.1 (8, 830..862) | 835/860 | -5.9 | +2.2 |
| 7 queries | 6384 | | 6236 | | **-148** (2.3%) | -138.5 |

### 2.2 Pooled with the prototype agent's raw data (recomputed from their CSVs; their tables reproduce exactly)

| q | tip (n hot; process medians) | proto | delta of medians | delta of process-median means |
|---|---|---|---|---|
| 3 | 660.2 (24; 663 660 663 660 659 661) | 634.1 (24; 635 635 636 634 636 632) | -26.0 | -26.5 |
| 8 | 936.1 (16; 934 938 933 937) | 908.6 (16; 911 906 906 908) | -27.5 | -27.9 |
| 7 | 747.2 (16; 749 748 746 744) | 719.2 (16; 720 719 719 720) | -28.0 | -27.1 |
| 9 | 1429.8 (16; 1436 1415 1419 1430) | 1394.2 (16; 1388 1394 1409 1387) | -35.6 | -30.4 (process sd 8-9: **-30 +- 8**) |
| 21 | 1440.2 (16; 1438 1441 1446 1440) | 1416.5 (16; 1416 1415 1420 1414) | -23.7 | -24.9 |
| 4 | 325.8 (16) | 325.8 (16) | +0.1 | -0.3 |
| 10 | 849.4 (16; 861 848 844 856) | 854.7 (16; 856 860 835 860) | +5.3 | +0.5 (process sd 7-10: flat) |

Every effect on the five claim queries is 3-15x the process-to-process spread, in both orders (ABBA and BAAB) and in two
independent sessions 4 hours apart. q9: the site-level change explains -23.5 ms (j29 -21.9, j19 -1.6, Quent); the
remaining 7-13 ms is inside q9's process spread. q10: site j10 -5.7 ms (Quent) is plausible but not resolved in wall time.

Per-site (Quent, my runs, median of 8 hot; span = first task of the build site starts -> first primary consumer scan
created): q3 j15 166.6 -> 142.2 (-24.4), j8 -2.7; q8 j41 135.9 -> 111.9 (-24.0), j34 -2.4; q7 j35 119.7 -> 93.4
(-26.3); q9 j29 82.9 -> 61.0 (-21.9); q21 j18 393.5 -> 376.1 (-17.4), j33 52.0 -> 45.8 (-6.2); q10 j10 -5.7, j21 -0.4;
q4 j9 -0.2. Same as PROTO_OP1B 5.2 within 2 ms. Deferred publication durations and join-point waits (prototype log,
`bin/op1b_log.py`): q3 j15 136.5 ms (waited 15.0 in 7 of 8), q8 j41 74.8, q7 j35 81.5 (waited 19.7), q9 j29 57.9,
q21 j18 318.8 (waited **233.2** in 8 of 8), j33 44.1 (waited 14.4), q10 j10 23.5 (waited 12.5), j21 15.9 (waited 15.3).

### 2.3 Suite

- Re-measured: 7 queries, -148 ms. Their figure for the other 6 measured queries (q5 -13.3, q12 -8.1, q2 -4.2, q16 -1.8,
  q11 -1.6, q6 -0.6 = -30 ms) was **not re-run** by me (queued, never served).
- The 9 queries never run on the prototype (q1 13 14 15 17 18 19 20 22): still no timing. I ran the five that have DF
  sites (q17 18 19 20 22) in the TRACE pass: results and filters identical (section 4), no timing value.
- So: **-1.1% of the suite (about -170 +- 15 ms on the 13 measured queries) stands as a partial figure**; nothing I
  measured contradicts it.

## 3. Bounce time vs non-bounce mechanics

### 3.1 P2P pair (GPUs 0-1, PEER transport): deferral off vs on, prototype binary

| q (2 GPUs) | mode 0 med (n; process medians) | mode 1 | delta | their delta | pooled (process-median means) |
|---|---|---|---|---|---|
| 3 | 910.2 (8; 910 910) | 919.1 (8; 915 919) | **+8.9** | +0.4 | **+4.5** (mode 0: 910 910 910 910; mode 1: 911 914 915 919) |
| 21 | 2018.3 (8; 2022 2017) | 2010.4 (8; 2007 2012) | -7.9 | +4.5 | -2.9 (sd 3-5: noise) |

- No gain. q3 is slower in 4 of 4 prototype processes (mode 0 is 910 ms to the millisecond in all four): **+2 to +9 ms
  (0.2-1.0%) regression on the PEER path**, not "no change". q21 is noise around 0: the outcome depends on which task
  becomes the Publishing Task (my hot iteration: build CONCAT created 163.1 instead of 170.7, scan 201.3 instead of 209.2;
  theirs: created 139.6, scan 209.0 = 209.1).
- Potential that was there to win on the pair: finish 13 ms (q3 j15), 30 ms (q21 j18), CONCAT slack 25-38 ms. Measured 0.

### 3.2 New control: the non-P2P pair (GPUs 0,2), same 2-GPU plan, HOST-staged transport

| q (2 GPUs, `CUDA_VISIBLE_DEVICES=0,2`) | mode 0 med (n; process medians) | mode 1 | delta |
|---|---|---|---|
| 3 | 644.1 (8; 644 644) | 626.9 (8; 626 627) | **-17.2 (2.7%)** |
| 21 | 1596.0 (8; 1596 1599) | 1538.4 (8; 1537 1539) | **-57.5 (3.6%)** |

q21 j18 hot iteration (Quent, ms from query start):

| | HOST pair, mode 0 | HOST pair, mode 1 | PEER pair, mode 0 | PEER pair, mode 1 |
|---|---|---|---|---|
| last non-publishing build task ends | 135.7 | 135.3 | 139.6 | 163.0 (later than in mode 0) |
| publication done | 250.0 (114 ms) | 292.0 (157 ms, overlapped) | 170.7 (31 ms) | <= 163.0 (42.5 ms by the log) |
| build CONCAT created -> last end | 250.0 -> 337.1 (87 ms) | 135.3 -> 237.1 (102 ms) | 170.7 -> 209.2 (38 ms) | 163.1 -> 201.3 (38 ms) |
| first probe scan created | 337.1 | 292.0 | 209.2 | 201.3 |

- Same GPU count, same plan shape, same binary: the gain appears when the cross-GPU transfers are HOST-staged and
  disappears when they are PEER. On the HOST pair finish + CONCAT = 201 ms serial becomes 157 ms overlapped (the staged
  loop leaves capacity idle that the CONCAT fills); on the PEER pair 31 + 34 = 65 ms serial stays 65 ms overlapped
  (existing prototype telemetry, `op1b_g2_*`: CONCAT ends at 205.0 / 209.1 ms in both modes, to 0.1 ms).
- This removes the main objection to their 2-GPU measurement ("a 2-GPU plan has no slack to hide the finish in"): it has,
  and it is used as soon as the transport is the bounce.
- Independent corroboration: PROTO_OP1A 5a-5b (other design, own microbench MB8) found the same byte-conserving exchange
  on the pair (+0.5 / -0.1 / +3.6 ms on q8 / q3 / q7).
- Side fact (not about the prototype): the HOST pair runs q3 and q21 **faster** than the P2P pair in absolute terms
  (644 vs 910 ms, 1596 vs 2018 ms). GPUs 0-1 sit behind one switch and share one host uplink; scans are host-to-GPU copy
  bound. A "full-P2P" machine built the same way has the same shared-uplink property.

### 3.3 Split

| | this machine | bounce excluded / P2P-equivalent |
|---|---|---|
| q3 | -26 (3.9%) | +2..+9 (measured, pair) |
| q8 | -27.5 (2.9%) | -2.4 (theirs; not re-run) |
| q7 | -28 (3.7%) | +1.7 (theirs; not re-run) |
| q9 | -30 +- 8 (2.1%) | not measured |
| q21 | -24 (1.7%) | -3 +- 5 (measured, pair) |
| suite (13 q) | about -170 (1.1%), partial | **0** (4 queries on the pair: sum +4 theirs; q3 + q21 mine: +1) |

All of the gain is bounce time moved into the build CONCAT's shadow. Non-bounce mechanics: 0, with a small loss on q3.

### 3.4 Would it survive on a machine with full P2P?

No, as far as this machine can tell:
- Measured (pair, two labs, two prototypes, one microbench): finish copies and CONCAT moves are byte-conserving on a
  shared link. Overlapping moves the wait, it does not remove it.
- 4 GPUs, all P2P (reasoning, not data): the root GPU's port carries all 3 merge inflows and 3 replica outflows of the
  finish plus its own 3 + 3 CONCAT flows. The root's CONCAT is the last to finish with or without the overlap, and the
  probe starts after the last CONCAT. Expected gain about 0; an upper bound would be the share of the CONCAT's `Preparing`
  that is not link-bound on the root, which the pair measurement puts at about 0 (65.2 vs 65.5 ms).
- Not measurable here: 3 PEER targets, 12 directed links, any topology where finish and CONCAT flows use disjoint links.

## 4. Correctness

### 4.1 Results
- Byte-identical `result.txt` tip vs prototype, every run: q3 8 7 9 21 4 (4 files each), q10 (4), 2-GPU arms q3 q21
  (8 files each, equal to the 4-GPU md5). `bin/cmp_results.py`.
- TRACE pass (prototype) vs the tip log pass: q3 7 8 9 10 21 and the never-run **q17 18 19 20 22**: identical md5.
- Still never run on the prototype: q1 13 14 15 (no DF site). q11's tie order (known) not re-checked.

### 4.2 Filters built / published / probed (`out/cmp_df.txt`)
- `parse_df.py` on my prototype TRACE pass vs `runs/baseline/logpass/df_summary.json`, `cmp_df.py`: q3 7 8 9 10 17 18 19
  20 21 22: **11 same, 0 differ**; 32 filters pushed at both; key lines (kind, size, pushed, reason), HOST-staging
  counts, compressed-scan probe lines (chunks, probes ran / of, kept, total) and DYNAMIC_FILTER rows in / out equal.
  E.g. q7 `1/1/true` 18 chunks 60,396,902 of 754,690,081; q21 17 chunks + 31 chunks `1/2/false`, as at tip.
- No fallback / error / reschedule / OOM line in any prototype run of mine (timing, 2-GPU, log pass); warning lines are
  the same three kinds as on the tip.

### 4.3 No consumer scan before its filter is visible
- Quent, `runs/profile/bin/quent_df.py` + `qdf_sum.py` on my timing runs: 168 join-instances per build; tip 0 scan tasks
  started before the publication; **prototype 2** (q7 j35, GPU_SCAN(31), one task in each of two iterations, 5.7 and
  9.6 us before). In addition the first scan task is *created* up to 41 us before that end event in 38 of 168
  site-instances (tip: 0 of 168).
- This is the instrument, not a lost filter: the script uses the Publishing Task's end event as "publication". The join
  point releases on the session's TERMINAL transition, which follows the fan-out (`publish`: `fan_out` -> `end_attempt`
  -> `~active_operation` -> `settle` -> `complete`, all filters pushed before) and precedes the task's end event by the
  log line, `release_retired_storage`, the memory-history record and `stream.sync()`. The probe counts of 4.2 (every chunk
  ran its probe) agree.
- Correction to PROTO_OP1B 5.4: "0 consumer scan tasks started before publication" holds for the Terminal instant by
  construction, but Quent as used can no longer prove it; a production change needs a mark at Terminal.
- The gate is per join: it covers the scans activated through this join's hint only (section 6, item 3).

### 4.4 Unit tests (`unit_m1.txt`, mode 1 on the prototype build)
- 673 cases: 660 pass, 13 fail: the same 13 `file:line` as their run (2 `fault::available()`, pre-existing on the tip
  build per their `unit_tip.txt`; not re-run by me). Mode 0 not re-run.
- What the 11 are (their description is accurate, the consequence is understated):
  - `test_in_task_publication.cpp:612` "mandatory scatter OOM retries the elected identity through the executor":
    fails at `retry_deposit_ready` (filter Terminal at the retry's deposit). The REQUIREs behind it (4 deposits, 1
    duplicate contribution, 1 publication, 2 filters pushed) are **not evaluated** in mode 1.
  - `:670` "a forward-layout OOM after contribution retries and publishes once" and `:788` "scheduler error drain joins
    collecting and publishing PARTITION tasks before reuse" (`during_publication`): the injection keys on the old order,
    never fires, the test times out. So "error drain while a publication runs" - the exact hazard of early completion -
    is not exercised.
  - `:852` "optional publication scratch OOM deposits without rescheduling the PARTITION task": injection never fires.
  - `:541`, `:578` (visible at the first deposit) and six `test_physical_partition.cpp` cases that call
    `partition.execute()` without a task (no `after_sink`, so the session never publishes: `0 == 2` contributions).
- **Net: with deferral on, no existing test covers OOM retry of the Publishing Task, an optional-scratch OOM in the
  publication, or an error / cancel drain during the publication.** "Failures explained" is true; "behaviour on those
  paths verified" is not.

### 4.5 Edge / failure paths
- **Nothing was executed on these paths.** The edge smoke (repeat x20, `con.interrupt()` sweep across the deferred
  publication, empty / tiny / LIMIT / semi joins) is written (`bin/edge.sh`, `bin/edge.py`, adapted from the OP1A
  verifier) but never got the lock. With 4.4 this means: cancel, error drain and OOM during the deferred tail are
  verified by nobody, neither by a test nor by a run. What is known is code reading only (sections 5 and 6).
- Races: 16 timed hot iterations per claim query plus the TRACE pass show the same filters and probes every time; the
  prototype agent's TRACE pass and DEBUG smoke agree. No stress run beyond that.

## 5. Code review of the prototype (read-only)

Session side is sound for the paths TPC-H takes:
- `claim_publication` = the head of `publish_if_final` (same predicate, `pending_publication.reset()`,
  `++active_operations`), `run_claimed_publication` = its tail under an `active_operation{claimed}` guard. The session is
  already `PUBLISHING` (set at the final commit), so `finish_input()` from the early pipeline close neither ends the
  attempt as `incomplete` nor abandons it (`pending_publication` already taken).
- Join point: `deferred` is set under the session mutex before `complete_task_early()`; the hint cannot name the probe
  producer before the build CONCAT pushed, which needs the pipeline closed, which needs the claim. No window in which the
  gate is passed before `deferred` is set.
- TERMINAL is reached on every exit of `publish` (RAII guard), including cancellation and recoverable failure, so the
  waiter cannot hang as long as the task thread runs to the end of `after_sink`.

## 6. What a production implementation must solve

1. **Early completion bends the pipeline contract.** `report_completion_early` closes the pipeline and runs
   `finalize_operator()` on every operator of the build pipeline while the task still executes and still holds its
   reservation, stream, input pins and outputs. Anything keyed on `tasks_created == tasks_completed` (query-terminal
   detection, per-query drains, the "task completed after the pipeline was considered done" check) no longer sees that
   task; only the worker's query lease keeps teardown away from it.
2. **The tail must be non-retryable and non-throwing by contract.** The executor's OOM-reschedule path would re-run a
   task whose completion was already counted (double deposit, counters off). Today nothing after
   `complete_task_early()` throws a recoverable OOM (`publish` swallows it), but that is a property of the callee, not
   enforced.
3. **The join point is per join and hint-only.** Consumers not behind this join's probe activation (scan reached through
   an intervening join, transitive / cross-scheduled targets, lookahead scheduling, non-`BUILD_PROBE` consumers) are not
   gated. At the tip they are already "opportunistic" (docs `dynamic-filters.md:315`), but the window in which they miss
   the filter grows from about 0 to the whole tail (up to 376 ms on q21 j18). A scan targeted by filters of several
   joins waits only for the join whose hint activates it (user decision: must wait for all). None in TPC-H (0 of 168).
4. **Blocking wait in a task-creator thread** (`get_next_task_hint`; 233 ms on q21 j18 in 8 of 8 iterations, 12-20 ms on
   q3 q7 q10): 1 of 4 creator threads, shared by all queries; under the lookahead strategy the call sits under
   `lookahead_mutex`. The hint returned after the wait was computed before it. Needs "not ready" + re-nomination from the
   Terminal transition.
5. **Failure-path coverage is gone** (section 4.4): rewrite the in-task publication tests for the new order, add tests
   for error drain / cancel / OOM during the tail, restore a Terminal invariant in place of the removed
   `D_ASSERT(terminal)` in `finalize_input` (now only the hash join's finalizer backstop).
6. **Memory peak**: the Publishing Task keeps reservation + scratch + pins for the whole tail while the build CONCAT
   allocates on the same GPU (q21 j18: 376 ms instead of 317 ms, now overlapped with a 1 GiB-class CONCAT). Not measured
   by either of us; no OOM / retry line in any run.
7. **The publication becomes the critical path where it outlasts the CONCAT** (q21 j18, q3 j15, q7 j35: the join point
   waits). The deferred HOST-staged finish runs 1.2-2.5x longer under the CONCAT (q3 57 -> 137 ms, q8 35.5 -> 75 ms,
   q21 j18 267 -> 319 ms): realized 25-80% of `min(finish, slack)`.
8. **Transport gate**: on PEER the change gains nothing and costs q3 +2..+9 ms. If it ever lands it must be conditional
   on HOST staging, i.e. it is a bounce optimization: by the user decisions in OPPORTUNITIES.md that belongs to the
   separate transfer PR, and it consumes the slack that eager scans want to reclaim.
9. Telemetry: Quent's Publishing Task duration now includes the tail and the pipeline range ends before its last task;
   instruments that equate "task end" with "publication" (quent_df.py) need a Terminal mark.
10. BUILD_FILTER sites (q4, q20) are untouched: no gain there (no slack either).

## 7. Unverified / not done

- Edge-path smoke and the timing of q5 q12 q2 (q16 q11 q6): not run (lock queue). The suite figure therefore rests on my
  7 queries plus their 6; the 9 remaining queries have no timing from anyone.
- Failure paths under the new order (cancel, error drain, OOM retry, optional scratch OOM during the tail): no test, no
  run (4.4, 4.5).
- Unit tests in mode 0 and on the tip binary: their runs taken as given; I re-ran mode 1 only.
- "Mode 0 of the prototype binary = tip" (their `m0` block, q3 +2.4, q8 -0.1): not re-run; my 2-GPU reference arms rely
  on it.
- OP1b (mode 2, insert-wait absorption): their "no gain" (q3 634.9 vs 634.4, q8 909.0 vs 909.5) not re-run.
- Bounce-excluded figures for q8, q7 (theirs only) and q9 (nobody). 4 GPUs all-P2P: not measurable here.
- Memory peak on the publisher's GPU during the overlap: not measured.
- Why q3 is 2-9 ms slower on the PEER pair with the deferral: not diagnosed.
- Housekeeping: nothing written to the root disk; every Quent session of my processes moved to
  `$R/telemetry/vop1b_*` (13 label dirs); `$R/wt-op1b` and `$R/repo` untouched (`git status` clean); no build, no push.
