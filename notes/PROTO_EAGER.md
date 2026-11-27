# PROTO_EAGER: eager probe-side scheduling when the build PARTITION has finished (option 1), measured

2026-11-18/19. Worktree `/opt/dlami/nvme/dfperf/wt-eager`, branch `proto/eager-probe` (from pr2570 `ae939824`), commit `092a0e3a`.
Second worktree `wt-eager2`, branch `proto/eager-opt2` (= `proto/op1-minimal` + the same patch + option 2 + scan throttle),
commits `49e651f6`, `0cd75dcb`, `2abda103`. Local commits only, nothing pushed.
Scripts `$R/runs/eager/bin/`, raw runs `$R/runs/eager/<label>/`, Quent `$R/telemetry/eager_<label>/q<N>/`, derived tables
`$R/runs/eager/out/`. Every build and GPU process ran under `flock $R/machine.lock`. Config file untouched.
Throwaway prototype: no docs, no tests.

STATUS (2026-11-27): FINAL. Sections 1-5 = first run of option 1 (batch `a1`, raw data lost). Section 6 = final tables of
the 11-19/20 re-run and the 11-27 close-out + the recommendation (6.8). Sections 7-8 are the first author's lists; where
they differ from 6.8, 6.8 holds. Checkpoint log at the end of the file.

## 1. What was built (option 1)

3 files, +67 lines (`git -C $R/wt-eager show --stat 092a0e3a`):

- `src/op/partition/consumer_partition_policy.cpp` `bind_partition_inputs`: unless the probe drives,
  `build.add_finalize_listener([&join, build_concat] { join.activate_probe_after_build_partition(build_concat); })`.
  The listener runs right after `hash_join_partition_observer::finalize_input()`, i.e. when the build PARTITION pipeline has
  finished and the join's publication session is terminal.
- `src/op/sirius_physical_hash_join.{hpp,cpp}` `activate_probe_after_build_partition(build_concat) noexcept`: one shot;
  BUILD_PROBE only, not MARK, not right-family; refuses (ERROR log) if a filter plan is enabled and the session is not
  terminal; then `task_creator->schedule(build CONCAT)` and `task_creator->schedule(probe head)`
  (`get_port("default")->src_pipeline->get_operators()[0]`). INFO line `[eager] join N probe activated ...`.
- Nothing else changes: hints, session, observer, CONCAT, join. "Build not ready yet" is handled by what the join already
  does: probe batches wait in the probe CONCAT / the join's probe port (ordinary spillable repositories) and
  `select_build_probe_action` answers `wait_for_build` until each partition's build batch has arrived.
- Switch: `SIRIUS_EAGER_PROBE` 0 / unset = tip behaviour, 1 = joins with an enabled filter plan, 2 = every BUILD_PROBE join.
  `SIRIUS_EAGER_CONCAT_FIRST` (default 1) = nominate the build CONCAT before the probe head (0 made no difference).

## 2. What ran

Protocol: fresh process per query, 1 warm + 5 hot, first hot dropped (4 hot per process), arms back to back in one lock hold
per query, order tip, eager, eager, tip; 2 processes per arm = 8 hot samples per arm; medians. Tip = `$R/repo` binary.

| label | what | queries |
|---|---|---|
| `smoke_dbg` | DEBUG log, 3 iterations, eager=1; automatic GO check (`smoke_check.txt`) | q3, q12, q16 |
| `a1_tip1/2`, `a1_pro1/2` | timing ABBA, tip vs eager=1 | all 22 |
| `a1_pros1/2` | eager=1 with the scan nominated before the CONCAT | q3, q16 |
| `a1_pro21/22` | eager=2 (joins without DF as well) | q14, q18, q2 |
| `logpass` | TRACE log pass, eager=1, 2 iterations | see section 4 |
| `b1_*` | scan throttle and option 2 (section 6) | see section 6 |

98 processes in `a1`, all rc=0.

## 3. Timing (a)

`$R/runs/eager/out/a1_times.md` (n = 8 hot per arm, min..max in the file).

| q | tip ms | eager ms | delta ms | ratio | note |
|---|---|---|---|---|---|
| 21 | 1439.5 | 1377.6 | -62.0 | 0.957 | slack sites j18, j33 |
| 3 | 661.8 | 615.5 | -46.4 | 0.930 | j15, j8 |
| 5 | 692.8 | 646.7 | -46.1 | 0.933 | j32, j25 |
| 8 | 933.8 | 896.4 | -37.4 | 0.960 | j41, j34 |
| 2 | 148.8 | 123.2 | -25.6 | 0.828 | j28 (hash IN-list), was not on the slack list |
| 16 | 312.9 | 291.5 | -21.4 | 0.932 | j11 |
| 7 | 746.8 | 725.9 | -20.9 | 0.972 | j35 |
| 9 | 1426.8 | 1416.8 | -9.9 | 0.993 | j29: the join used to overlap the scan, see 5.3 |
| 12 | 700.0 | 692.7 | -7.3 | 0.990 | j8 |
| 10 | 851.1 | 843.8 | -7.3 | 0.991 | j10 (9 ms window) |
| 20 | 717.1 | 711.6 | -5.5 | 0.992 | inside noise |
| 13 | 1307.9 | 1302.9 | -5.0 | 0.996 | inside noise (one tip process 1378) |
| 22 | 339.3 | 336.0 | -3.3 | 0.990 | inside noise |
| 11 | 173.3 | 171.7 | -1.6 | 0.991 | control |
| 15, 6, 18, 19, 1, 4, 17 | | | -0.9 .. +0.4 | 1.000 | controls, flat |
| 14 | 687.3 | 693.0 | +5.7 | 1.008 | inside noise (676..707 both arms); no eager site at switch 1 |
| **sum of 22** | **15314** | **15019** | **-295** | **0.9808** | |

- Suite: eager is **295 ms (1.9%) faster than the tip** in the same blocks. Against the reference totals of the campaign
  (base 15225 ms, tip 15372 ms, other protocol: median of 6 hot including the first): tip - 295 = ~15077 ms, i.e.
  **~150 ms (1.0%) below base** with the finish and the bounce unchanged. Direct: 15019 vs 15225 = -206 ms (-1.35%), but
  my tip arm is also 58 ms below the reference tip, so the first figure is the fair one.
- Slack queries (q3 q8 q21 q16 q7 q5 q9 q12): -251 ms. Their bound (sum of min(C, S), EAGER_SCOUT) was 648 ms: 39% realized.
- Variants: scan nominated before the CONCAT (`pros`): q3 620.2 vs 615.5, q16 291.6 vs 291.5: no difference.
- Switch 2 (joins without DF too): q14 632.5 (-54.7 ms vs tip), q18 1007.7 (-81.2), q2 123.6 (same as switch 1).
  Extrapolated suite with switch 2: about -430 ms (2.8%); only these three queries were run at switch 2.

## 4. Correctness (c)

- Results: `sirius/q<N>/result.txt` byte-identical between all tip and all prototype processes (switch 1, switch 2,
  scan-first) for 21 queries; q11 differs between any two processes including tip vs tip (tie order, known); sorted, the
  q11 files are identical (`out/a1_results.txt`).
- Filter visibility (Quent, `quent_df.py` + `qdf_sum.py`, `out/qdf_pro.md`): 368 join-instances (46 joins x 8 hot
  iterations), **0 consumer scan tasks created or started before the publication**, including the non-direct consumers
  (q7 j38 -> scans 17, 24; q8 j44 -> scans 15, 23, 30; q21 j21 -> scan 13). First consumer scan is created 0.0 - 0.2 ms
  after the Publishing Task ends (was 11 - 112 ms). Same for switch 2 and scan-first.
- Engine side: 258 `[eager] join ... probe activated` lines per process set, every one with `session_terminal=true`;
  0 refusals.
- Smoke at DEBUG (q3, q12, q16): every compressed-scan chunk ran all its probes (`N of M probe(s) ran`, 60 / 24 / 6
  chunks, 0 partial).
- Log scan of all `a1` logs: 0 OOM retries, 0 errors, 0 fallbacks; the downgrade monitor moved 0 batches in every request
  (tip and eager alike). Quent: no second reservation attempt, no non-GPU target tier, no failed task, reservation wait
  max 0.04 ms (tip 0.13 ms).
- TRACE log pass diff (filters built / published / pushed, DYNAMIC_FILTER rows in / out, chunks probed): see the end of
  this file (LOGPASS).

## 5. Quent per site (b) and the calculus (d)

Tables: `out/sites_a1.md` (all columns), `out/calc_a1.md`. Medians of 8 hot iterations per arm. C = build CONCAT window
(Publishing Task end -> last CONCAT task end), S = probe scan chain, F = Publishing Task (the DF finish runs inside it).

### 5.1 Sites

| q | join | F ms | C tip | S tip | bound min(C,S) | CONCAT `Preparing` med tip -> eager | scan stretch S'-S | build lands after scan end | site gain | wall gain |
|---|---|---|---|---|---|---|---|---|---|---|
| 3 | 15 | 57.2 | 110.2 | 316.3 | 110.2 | 98 -> 377 | +20.6 | +54.7 | 33.9 | 46.4 (with j8) |
| 3 | 8 | 4.2 | 11.8 | 84.4 | 11.8 | 5.5 -> 51 | +2.2 | -2.9 | 9.2 | |
| 8 | 41 | 35.6 | 100.1 | 594.1 | 100.1 | 87 -> 641 | +24.4 | +54.3 | 21.5 | 37.4 (with j34) |
| 8 | 34 | 4.2 | 28.7 | 86.0 | 28.7 | 16 -> 61 | +5.9 | +9.2 | 13.6 | |
| 21 | 18 | 317.2 | 76.9 | 378.6 | 76.9 | 61 -> 372 | +30.3 | +5.0 | 39.1 | 62.0 (with j33) |
| 21 | 33 | 26.1 | 25.5 | 377.2 | 25.5 | 22 -> 294 | +15.1 | -48.6 | 16.1 | |
| 5 | 32 | 16.7 | 57.5 | 398.7 | 57.5 | 47 -> 422 | +10.7 | +22.2 | 24.4 | 46.1 (with j25) |
| 5 | 25 | 4.2 | 49.1 | 95.0 | 49.1 | 27 -> 72 | +5.8 | +24.8 | 21.1 | |
| 7 | 35 | 63.2 | 54.7 | 73.1 | 54.7 | 48 -> 98 | +10.5 | +31.0 | 13.8 | 20.9 |
| 9 | 29 | 31.9 | 51.3 | 718.9 | 51.3 | 39 -> 693 | -18.3 | +10.5 | (58.9) | 9.9 |
| 16 | 11 | 8.5 | 72.5 | 49.9 | 49.9 | 60 -> 73 | +4.4 | +46.5 | 21.4 | 21.4 |
| 12 | 8 | 11.2 | 30.5 | 21.0 | 21.0 | 25 -> 40 | +0.9 | +23.1 | 6.1 | 7.3 |
| 10 | 10 | 21.9 | 9.0 | 246.9 | 9.0 | 7.7 -> 243 | +3.4 | -1.3 | 5.7 | 7.3 |
| 2 | 28 | 3.2 | 27.3 | 7.1 | - | 15 -> 27 | +2.0 | - | - | 25.6 |

Activated sites with a window >= 5 ms: bound 653 ms, site gain 259 ms (40%), F on the path 605 ms, build landing after the
scan end 332 ms (sum of the positive values), scan stretch 118 ms. "Site gain" = max(C end, S end) - publication, tip minus
eager; it misses what happens after the scan (q9) and what else overlaps (q2), so the wall figures are the verdict.

### 5.2 Contention findings

1. **The build move is starved by the overlapping scan.** The build CONCAT task starts 0.1 ms after the publication in
   both arms, but its `Preparing` phase (the cross-GPU move of the build, HOST-staged on this machine) stretches from C to
   roughly the whole scan: q8 j41 87 -> 641 ms, q9 j29 39 -> 693, q3 j15 98 -> 377, q5 j32 47 -> 422, q21 j18 61 -> 372,
   q10 j10 8 -> 243. The build lands 5 - 55 ms **after** the scan has ended at the large sites, and the join waits for it.
   Mechanism (code read, `cucascade/src/cudf/representation_converter_builtins.cpp:596-710`): each build buffer is moved
   as a train of 1 MiB device-to-host copies, then 1 MiB host-to-device copies, on one stream with a sync per buffer; it
   competes with 4 scan tasks per GPU that each keep a deep queue of 1 MiB host-to-device copies on a link that the scan
   alone already saturates (EAGER_SCOUT: 98% copy duty, 28 GB/s per GPU).
2. **The scan pays little:** +1 to +30 ms per site (+118 ms summed), i.e. about the CONCAT's own copy time. Scan task
   medians drop (q3 114 -> 88 ms, q8 148 -> 106) because one executor thread per GPU is held by the CONCAT task and three
   scan tasks share the same link.
3. **Realized gain = C - scan stretch - build tail.** That is 21 - 63% of the bound per site, 40% summed.
4. No memory effect: no reservation wait, no retry, no downgrade, no spill (section 4).
5. Probe wait for the build: the join's first task is created 4 - 37 ms before the last CONCAT task ends at the large
   sites (partitions whose build has landed start), 15 - 32 ms after the scan end at q3 j15 / q8 j41 / q16 (tip: 1 ms).

### 5.3 q9: the one site where queueing at the probe costs

At q9 j29 the tip's join already overlaps the scan (first join task 109 ms before the scan ends: the probe output passes
the 1 GB CONCAT threshold). With eager the build lands 10 ms after the scan end, so that overlap is gone and the wall gain
is 10 ms instead of ~51. Same starvation, different symptom. q21 j33 is the other site of this kind and keeps its overlap
(build lands 49 ms before the scan end).

### 5.4 The calculus

Critical path per site: tip `F + C + S`; eager option 1 `F + max(C', S')` with `C'` ~ `S' + tail` under starvation.

- Eager start, as measured: -295 ms per suite pass (switch 1), 40% of the 653 ms bound.
- F stays on the path 1:1 in both designs and nothing hides it any more: **605 ms per suite pass here** (Publishing Task
  medians of the activated sites; 600 ms in the campaign ledger, ~215 ms P2P-equivalent). Per site: q21 j18 317, q7 j35 63,
  q3 j15 57, q8 j41 36, q9 j29 32, q21 j33 26, q10 j10 22, q5 j32 17, q12 11, q16 8.5.
- What a shorter finish still buys once scans are eager (model on the ledger's split, not measured):
  - this machine: up to 600 ms (the whole finish), of which ~385 ms is the bounce excess that a direct / well pipelined
    transfer removes;
  - bounce excluded: the P2P-equivalent finish ~215 ms is the ceiling for byte-reduction levers (fewer filters, smaller
    filters), plus the insert kernel ~65 ms.
- What the CONCAT window still costs once scans are eager: at long-scan sites the CONCAT is off the path only if it lands
  before the scan ends. Today it does not (332 ms of build tail). So the build move needs either priority on the copy
  path or a cheaper transfer; this is the second place where the transfer work pays, and it is not in the finish.
- Suite arithmetic (tip reference 15372, base 15225):
  - eager alone, as measured: ~15077 (-1.0% vs base);
  - eager + build tail recovered (upper bound +332): ~14745;
  - eager + bounce-free finish (-385, model): ~14690; both: ~14360 (-5.7% vs base). The last two lines are models.

## 6. Throttle, option 2, S6 release condition, staging modes, final tables (FINAL, 2026-11-27)

Measured in two sessions. **11-19/20** (previous author, after the first wipe; tags `v1 b2 c3 c4 fs fs2 gt`, 342
processes, all rc=0) and **11-27** (close-out after the second wipe; tag `r7`, 118 processes, all rc=0, plus a TRACE tip
log pass of 22 processes). Every delta below is against the tip processes of the same tag and the same lock holds; sums
that mix tags say so. Nothing from 11-19/20 was rerun except where an arm was needed as same-session reference.
Raw data: `runs/eager/<tag>_<arm><n>/` (timings `csv/runtimes.csv`, logs, results), Quent of r7 in
`telemetry/eager_r7_<arm><n>/`, derived tables `runs/eager/out/`. Quent / nsys captures of 11-19/20 are lost (wipe); the
mechanism figures of 6.3 are the record of that session (`out/sites_calc.md` survives).

Code: `wt-eager` f05cfd12 (`proto/eager-probe`, option 1, 3 files +67); `wt-eager2` fdd2caec (`proto/eager-opt2`: option 1 +
option 2 + gate variants + Memory Admission placeholder + scan throttle, 16 source files +560; cuCascade patch
`proto/cucascade-batch-staging.patch`, applied uncommitted in the submodule). Tip = `$R/repo` ae939824. Config untouched.

### 6.0 Protocol and arms

Fresh process per query, 6 iterations (1 warm + 5 hot), first hot dropped, 2 processes per arm in ABBA order = 8 hot
samples per arm, medians; every process under `flock $R/machine.lock`, one hold = 3 to 12 processes. Noise +-3% / +-10 ms
per query; suite gate 0.5% = 77 ms. Scripts `runs/eager/bin/{unit2,plan}.sh`, plans `runs/eager/plans/*.plan`.

| arm | switches (all on `wt-eager2` except tip, pro) |
|---|---|
| tip | `$R/repo` |
| pro | `wt-eager`, `SIRIUS_EAGER_PROBE=1` |
| o1 | `SIRIUS_EAGER_PROBE=1`: option 1 = publication inside PARTITION, probe side activated at build PARTITION finalize; joins with a filter plan only; gate 0 |
| g1 / g2 | o1 + `SIRIUS_EAGER_GATE=1` (S6 literal: every Relevant Producer of the join's channels Terminal) / `=2` (the channel of every scan this activation starts is Terminal) |
| f1 | `SIRIUS_EAGER_PROBE=2` (every BUILD_PROBE join) + gate 2 + `SIRIUS_EAGER_ADMIT_GIB=16` = the S6 shape |
| k1 / k2 / k3 | o1 + `SIRIUS_EAGER_SCAN_THROTTLE=K`: at most K scan tasks per GPU in `Preparing` while an eagerly overlapped build move is pending |
| fk1 | f1 + `SIRIUS_EAGER_SCAN_THROTTLE=1` (new 11-27) |
| o2 | option 2: `SIRIUS_DF_DEFER_PUBLISH=1` (deferred publication) + activation callback at its end |
| bs / bs2 | o1 + `CUCASCADE_PROTO_BATCH_STAGING=1 / 2`; `b0` / `b02` = same with eager off; `bsk` / `bs2k` = + K=1; `bf2` = f1 + mode 2 |
| adm | o1 + gate 2 + `SIRIUS_EAGER_ADMIT_GIB=1000` (Memory Admission never granted) |

Throttle note: its first version counted every task, the build CONCAT's own included, so the old "K=1" was "nothing
overlaps the build move" (= tip, +4 ms). Since ca0c150b it counts scan-sourced tasks only (`SIRIUS_EAGER_THROTTLE_ALL=1`
restores the old behaviour). With 4 pipeline threads per GPU and the CONCAT task holding one, K=3 is no throttle.

### 6.1 Inventory (what was complete, what was run on 11-27)

| tag | date | content | processes rc=0 | state |
|---|---|---|---|---|
| v1 | 11-19 | tip, pro: q3 q8 q21 q16 | 16 / 16 | complete |
| b2 | 11-19 | tip o1 k1 k2 k3 o2: q3 q8 q21 | 36 / 36 | complete |
| c3 | 11-19 | tip b0 bs bsk: q3 q8 | 16 / 16 | complete |
| c4 | 11-19 | tip o1 k1 o2 bs2 (8 slack queries), k1 o2 on q10 q2, bs2k b02 adm | 86 / 86 | complete |
| fs | 11-19 | q3 smoke of g2 f1 bf | 8 / 8 | complete |
| fs2 | 11-19/20 | full suite: tip f1 bf2 on 22 queries, g2 on the 12 with a slack / switch-2 site | 156 / 156 | complete; `g2 = f1` was assumed on the other 10 |
| gt | 11-20 | tip g1 g2: q7 q8 q21, q2 | 24 ran (23 status lines, one q2 g2 dir lost in the backup) | complete |
| r7 | 11-27 | tip f1 fk1 on the 18 queries that have an eager activation; g2 k1 on q22; k1 on q18; f1 x1 on q1 q4 q6 q13 | 118 / 118 | new |

What r7 fills: (1) option 1 at switch 1 on the queries where 6.5 of the draft assumed `g2 = f1` (wrong at q22: j33 has no
filter plan); (2) the throttle on the full suite, in the recommended shape; (3) result files of q11 q16 q20 and all Quent
(lost in the wipe) for the visibility check. Arms that are the tip code path by construction were not run: any arm on
q1 q4 q6 q13 (0 `[eager]` lines in the f1 logs: no BUILD_PROBE join is activated), g2 / k1 on q14 q15 (no join with a
filter plan). Where every activated join has a filter plan (q11 q17 q19 q20) switch 1 = switch 2, so g2 = f1 and k1 = fk1.

### 6.2 (a) Full-suite gain of option 1

`out/r7_suite_table.md` (`bin/r7_tables.py`); cells = delta vs tip in ms (n hot samples, spread max-min, tag).

| q | tip 11-19 | tip 11-27 | o1/g2 (switch 1) | k1 (switch 1) | f1 11-19 | f1 11-27 | fk1 11-27 | bf2 11-19 |
|---|---|---|---|---|---|---|---|---|
| 1 | 470.3 | - | tip path | tip path | -0.9 | tip path | tip path | +2.2 |
| 2 | 148.1 | 147.4 | -25.1 (fs2) | -26.4 (c4) | -23.4 | -24.3 | -23.0 | -7.8 |
| 3 | 664.4 | 664.3 | -47.9 (fs2) | -61.2 (b2) | -47.1 | -41.7 | -66.4 | -74.1 |
| 4 | 324.2 | - | tip path | tip path | +0.7 | tip path | tip path | +1.9 |
| 5 | 692.7 | 691.7 | -46.6 (fs2) | -49.0 (c4) | -49.3 | -49.3 | -54.0 | -60.5 |
| 6 | 380.9 | - | tip path | tip path | -0.7 | tip path | tip path | -0.6 |
| 7 | 747.1 | 746.6 | -23.5 (fs2) | -20.3 (c4) | -22.1 | -23.2 | -19.6 | -18.7 |
| 8 | 936.6 | 935.0 | -42.4 (fs2) | -71.6 (b2) | -41.0 | -36.4 | -73.7 | -78.9 |
| 9 | 1409.6 | 1418.0 | -13.8 (fs2) | -30.0 (c4) | -86.4 | -79.5 | -109.7 | -47.3 |
| 10 | 855.0 | 845.5 | -15.6 (fs2) | +0.9 (c4) | -9.5 | +2.0 | +6.4 | +9.8 |
| 11 | 176.7 | 173.1 | = f1 | = fk1 | -4.6 | -1.4 | -1.3 | -5.2 |
| 12 | 702.6 | 701.1 | -8.7 (fs2) | -8.3 (c4) | -10.3 | -7.2 | -8.4 | -9.8 |
| 13 | 1287.6 | - | tip path | tip path | +17.6 (spread 395) | tip path | tip path | +4.2 |
| 14 | 692.8 | 687.5 | -5.0 (fs2; tip path, noise) | tip path | -52.4 | -54.0 | -62.4 | -65.8 |
| 15 | 529.1 | 527.7 | tip path | tip path | -1.6 | -0.6 | +1.2 | -4.5 |
| 16 | 313.3 | 311.2 | -23.3 (fs2) | -19.8 (c4) | -24.2 | -20.7 | -20.5 | -25.6 |
| 17 | 497.0 | 497.8 | = f1 | = fk1 | +0.3 | -0.6 | -0.8 | +0.4 |
| 18 | 1087.0 | 1084.5 | -6.2 (fs2) | +8.5 (r7) | -82.6 | -84.0 | -83.1 | -60.3 |
| 19 | 880.2 | 881.5 | = f1 | = fk1 | -0.0 | -1.5 | -1.5 | +0.5 |
| 20 | 713.3 | 716.7 | = f1 | = fk1 | +1.1 | -2.9 | -1.0 | +0.1 |
| 21 | 1441.8 | 1448.1 | -57.5 (fs2) | -61.9 (b2) | -58.6 | -56.8 | -63.0 | -52.4 |
| 22 | 339.5 | 338.2 | -2.7 (r7) | -2.7 (r7) | -16.5 | -15.2 | -15.7 | -14.8 |
| **sum** | **15290** | 12816 (18 q) | **-324** (mixed tags) | **-347** (mixed tags) | **-512** (22 q) | **-497** (18 q) | **-597** (18 q) | **-507** (22 q) |

- **Option 1 as the user stated it, joins with a dynamic filter only (switch 1): -324 ms = -2.1% of the suite**
  (13 measured queries -318, the 4 `= f1` queries -6, tip-path queries 0; tags fs2 + r7). The lost first run gave -295,
  the same code as `o1` in c4 gave -244 on the 8 slack queries against -251 in the lost run: reproduced three times.
- **Option 1 on every BUILD_PROBE join (switch 2, `f1`, the S6 shape): -512 ms = -3.35%** (11-19, all 22 queries in one
  tag) and **-497 ms = -3.25%** (11-27, the 18 queries that have an activation; the same 18 gave -529 on 11-19). The
  extra ~190 ms over switch 1 comes from joins without a filter plan: q9 -80..-86 (switch 1: -14), q18 -83 (-6),
  q14 -53 (0), q22 -15..-16 (-3).
- Regressions: none outside noise. Largest positive cells: q10 +2.0 (11-27; -9.5 on 11-19), q13 +17.6 on 11-19 with a
  tip spread of 395 ms on a query that has no activation (tip code path).
- Against base (15225 at the campaign reference where tip = 15372): tip - 500 = about 14870, i.e. **~2.3% below base**.

### 6.3 (b) Scan throttle

Same-tag comparisons only:

| comparison | queries | without throttle | K=1 | K=2 | K=3 |
|---|---|---|---|---|---|
| switch 1, tag b2 (11-19) | q3 q8 q21 | o1 -147 | -195 | -147 | -140 |
| switch 1, tags c4 + b2 (11-19) | 8 slack queries | o1 -244 | **-322** | - | - |
| switch 2, tag r7 (11-27) | 18 queries | f1 -497 | **fk1 -597** | - | - |

- **K=1 helps: -78 ms (switch 1, 8 queries) and -100 ms (S6 shape, full suite) on top of the unthrottled shape**,
  i.e. 0.5 - 0.65% of the suite, above the suite gate. Per query (fk1 - f1, 11-27): q8 -37, q9 -30, q3 -25, q14 -8,
  q21 -6, q5 -5; flat elsewhere; worst q10 +4.4 and q7 +3.6 (inside noise). At switch 1 the throttle is flat on the
  queries added on 11-27 (q22 0.0, q18 +8.5 with a 35 ms spread).
- **K=2 and K=3 do nothing** (-147 / -140 against -147): K=3 is no throttle (the CONCAT task holds the 4th thread), K=2
  still leaves two queued scan trains in front of every build buffer.
- K=1 also removes the bimodal iterations of the unthrottled arms (one hot iteration in four at q3 / q8 runs 60 - 80 ms
  slower, at the tip's speed or worse): spread at q3 8 ms (f1 22), q8 5 ms (f1 81).
- Why it works (6.5): with one scan task per GPU the build move waits one queued scan train per buffer instead of
  three, so the build lands before the scan ends; price = a slower scan (q3 scan chain 370 ms vs 337 unthrottled vs 316
  at the tip, Quent 11-19).
- Why it is not the design: it is tied to this config (4 pipeline threads per GPU), to the build move being host-staged
  (no full P2P here), it blocks an executor thread in a process-global condition variable, and only K=1 works. bs2 (a
  transfer change) recovers the same loss at q3 q8 q5 without touching the scan (6.4).

### 6.4 (d) cuCascade batched staging

| arm | tag | q3 | q8 | q21 | q16 | q7 | q5 | q9 | q12 | sum |
|---|---|---|---|---|---|---|---|---|---|---|
| o1 | c4 / b2 | -43.7 | -38.7 | -64.1 | -21.6 | -21.7 | -44.7 | -2.6 | -7.0 | -244 |
| bs (mode 1 + o1) | c3 | -32.0 | -30.9 | - | - | - | - | - | - | -63 (o1 same queries -82) |
| b0 (mode 1, eager off) | c3 | +6 | +20 | | | | | | | +26 |
| bs2 (mode 2 + o1) | c4 | -74.1 | -73.8 | -58.9 | -23.6 | -15.9 | -54.9 | -5.6 | -9.1 | **-316** |
| b02 (mode 2, eager off) | c4 | +4.0 | +6.8 | | | | | | | +11 |
| bs2k (mode 2 + K=1) | c4 | -65.2 | -71.5 | -56.5 | | | | | | -193 (bs2 -207, k1 -195) |
| bf2 (mode 2 + f1), full suite | fs2 | -74.1 | -78.9 | -52.4 | -25.6 | -18.7 | -60.5 | -47.3 | -9.8 | **-507 on 22 q (f1 -512)** |

- **Mode 1** (drop the per-buffer sync, same copy order): **no gain**, slightly negative (-63 vs -82 with eager, +26 with
  eager off).
- **Mode 2** (all device-to-host copies of a table first, then one host-to-device train; 60 lines in
  `peer_copy_into_async` / `convert_gpu_to_gpu`): **gain on the 8 slack queries (-316 vs -244, i.e. -72), no gain on
  the full suite (-507 vs -512)**: it wins at q3 (-27 vs f1), q8 (-38), q14 (-13), q5 (-11) and loses at q9 (+39),
  q18 (+22), q10 (+19), q2 (+16). Without eager scans it costs +4 / +7 ms. Not additive with K=1.
- Verdict: mode 1 no, mode 2 not landable (regressions unexplained; it delays every host-to-device byte until the last
  device-to-host byte of the table is out). It proves the mechanism of 6.5; the landable transfer shape belongs to
  the transfer PR.

### 6.5 Contention between the eager scan and the build move (record of 11-19; nsys / Quent raw data lost)

The resource is the target GPU's host-to-device copy queue. The scan alone saturates it (98% busy; scan throughput per
GPU is the same with 1, 3 or 4 concurrent scan tasks). The build move
(`cucascade/src/cudf/representation_converter_builtins.cpp`, `peer_copy_into_async`) handles one buffer at a time
(device-to-host train, host-to-device train, stream sync); its host-to-device train enters the queue behind every scan
train already queued. Egress, peer copies, the pinned pool, pipeline threads and GPU compute are not the bottleneck.

| arm | wait of each host-staged buffer before its host-to-device half | move task duration (q3 j15) | sum of build tails, 8 slack queries | wall, 8 queries |
|---|---|---|---|---|
| tip (no overlap) | 0 | 105-120 ms | - | 0 |
| o1 (3 scan tasks per GPU) | 82-85 ms = 3 queued scan trains | 382-405 ms | 289 ms | -244 |
| k1 (1 scan task per GPU) | 25-30 ms = 1 train | 314-348 ms | 119 ms | -322 |
| bs2 (two-phase staging) | 60-67 ms, once per source batch | 199-290 ms | 110 ms | -316 |

Strict priority for the build move (nothing overlaps it) is the tip (+4 ms): both sides are bound by the same link.
What recovers the tail is not paying one queue round per buffer. Option 2 (`o2`) is not better than option 1 (-246 vs
-244 on the 8 queries; q21 -35 vs -64, q3 -31 vs -44, q12 -15 vs -7, q9 -19 vs -3) and needs the deferred publication
(12 more files): its deferred finish overlaps the build move and the two host-staged transfers starve each other
(q3 j15: Publishing Task 57 -> 135 ms). Not pursued.

### 6.6 (c) Release condition: gate 0 / 1 / 2

S6 (#1825): `normal_build_deposit_ready || (layout_stable && target_producers_terminal && memory_admitted)`.

| gate | rule at the activation (besides the join's own session Terminal, always required) | timing | held joins | results |
|---|---|---|---|---|
| 0 | none (other producers Terminal by induction) | o1: -244 on 8 slack queries (c4) | 0 | identical |
| 1 | S6 literal: every channel in the join's publish plan Terminal | q2 -2.1, q7 -15.2, q8 -40.1, q21 -47.5 (gt) | q2 j28, q7 j38, q8 j44, q21 j21: held in every iteration (48 held lines), fall back to ordinary activation | identical |
| 2 | the channel of every scan this activation starts is Terminal | q2 -23.5, q7 -21.4, q8 -42.9, q21 -52.3 (gt); full suite as g2 / f1 in 6.2 | 0 of the 56 joins activated across the suite, in every process (fs2, r7) | identical |

- **Gate 2 = gate 0 in time and behaviour** (it never holds: `started_pending=0` for all 56 activated joins in all f1 / fk1
  processes, `out/gatelog_f1.md`, `out/gatelog_r7_f11.md`), but it checks the user's rule instead of arguing it.
- **Gate 1 costs 35 ms** on the four queries where it holds a join (q2 21, q7 6, q8 3, q21 5) and nothing elsewhere
  (no other join has a pending relevant channel). The held joins publish to a scan that sits under a lower join of
  their own probe subtree; that lower join is the other producer on the channel and cannot become Terminal before this
  join releases its probe side. So the literal rule can never open for stacked probes: it is a safe fallback, not the
  rule. No hang, no result difference.
- No correctness difference between the three: result files identical to tip in every process of every gate (6.7).
- Memory Admission placeholder (`SIRIUS_EAGER_ADMIT_GIB=16`): always passes; lowest free GPU memory at any activation
  74.7 GiB (q9; 74.9 on 11-27) of 95.6. `adm` (never admitted): all joins held, q3 -3.7 ms vs tip, results identical:
  the hold path falls back cleanly. A real admission rule was not built and cannot be validated on this config.
- Not implemented: re-evaluation on a later terminal notification of another producer. On TPC-H it would never fire.

### 6.7 (e) Correctness

- **Results: 22 of 22 queries identical to tip** across 425 result files of all tags (v1 b2 c3 c4 fs fs2 gt r7; every arm:
  pro o1 g1 g2 f1 fk1 k1 k2 k3 o2 adm bs bs2 bs2k bf2 b0 b02), byte-identical for 21 queries, q11 identical after sorting
  (tie order, differs tip vs tip). q11 q16 q20: r7 files only (tip, f1, fk1; older files lost). Tip result files of
  11-19 and 11-27 have the same md5 (regenerated data). Not checked against DuckDB CPU (campaign-wide open item).
- **No scan starts before all its filters are visible.** Quent, r7 (`runs/profile/bin/quent_df.py` +
  `runs/op1b/bin/qdf_sum.py`; `out/qdf_r7_{f1,fk1,tip}.md`, json next to them): f1 364 join-instances (46 DF-building
  joins x 4 hot x 2 processes, q13 once), fk1 360: **0 consumer scan tasks created or started before the publication of
  any filter targeting the scan**; smallest gap publication -> first consumer task created +0.03 ms (median 0.2 ms; tip
  2.6 ms). The 11 scans targeted by more than one join (q2 scan 21; q7 scans 17, 24, 31; q8 scans 15, 23, 30, 37; q20
  scans 14, 27; q21 scan 13) are covered pair by pair: 0 tasks before any of their publications.
- Engine side: every `[eager] ... activated` line has `session_terminal=true` and `started_pending=0` (4147 lines on
  11-19/20; all 56 activated joins in each r7 f1 / fk1 process set); 0 refusals.
- Logs of all 460 timing processes: 0 fallback lines, 0 error / OOM lines, 0 OOM retries.
- Not repeated on 11-27: TRACE log pass of the prototype (section LOGPASS, first run: equal filters, rows, chunks), unit
  tests (never run on prototype builds), memory pressure.

### 6.8 Recommendation (the S6 design to productionize)

**Productionize `f1` without the throttle: option 1 on every BUILD_PROBE inner / left-family join, released by
`layout_stable && started_scans_terminal && memory_admitted`, ordinary activation kept as the alternative.** Measured
-497..-512 ms (-3.3%) per suite pass, results identical on 22 of 22, no scan before its filters. Release condition:
(1) `layout_stable` = build PARTITION finalize, at which the join's own publication session is Terminal by the #2570
contract (publication stays synchronous inside the Publishing Task; option 2 / deferred publication is rejected: no gain,
12 more files); (2) `target_producers_terminal` defined over the endpoints the activation starts (gate 2: the channel of
every scan reachable from the probe head, following only the build side of lower hash joins, is Terminal) - this is the
user's "wait for ALL filters" rule, it never held a join on TPC-H, whereas the S6-literal Relevant-Producers rule
(gate 1) can never open for stacked probes and costs 35 ms; keep the literal rule as the fallback where the walk cannot
be computed, and add the level-triggered re-evaluation on the last producer's terminal notification for shapes TPC-H
does not have; (3) `memory_admitted` = a real reservation-based rule (the prototype has a 16 GiB threshold that always
passes; when admission cannot be shown the join keeps ordinary activation, which the `adm` arm shows is free).
**Throttle: not in S6's design.** K=1 is worth a further -100 ms (0.65%) here and only K=1 works; ship it at most as an
internal, default-off ablation switch, because the loss it recovers (the build move queueing behind the scan's copy
trains) is a transfer problem whose fix belongs to the transfer PR. Batched staging mode 1: drop; mode 2: evidence only.
Files and owners: **#2570** - document and test the contract "build PARTITION finalize => session Terminal for every
layout and outcome" (`src/op/dynamic_filter/hash_join_partition_observer.cpp:60-65`, today a `D_ASSERT`;
`docs/super-sirius/dynamic-filters.md`) and a cheap `terminal()` on the channel
(`dynamic_filter_publisher.{hpp,cpp}`); **#2569** - nothing; **S6 PR (#1825)** - the activation
(`src/op/partition/consumer_partition_policy.cpp` `bind_partition_inputs`, fired outside the pipeline status mutex with
weak targets and coalesced through `task_creator::schedule`; `src/op/sirius_physical_hash_join.{hpp,cpp}`
`activate_probe_after_build_partition` with the started-scans walk and the gate; build CONCAT nominated before the probe
head), Memory Admission (`src/creator/task_creator.hpp` + reservation path), the internal ablation switch and the
`[eager]` observability line, docs (`pipeline-execution.md`, `task-creator.md`, `dynamic-filters.md`, GLOSSARY), tests
(both notification orders, probe finishing before any build lands, zero-filter terminals, teardown with a queued
request, MARK / right-family / STANDARD / MIXED / delim exclusions, memory pressure); joins without a filter plan are
the same hook and half of the gain (keep as a separate commit); **transfer PR [xfer-PR]** - the build move must not pay
one ingress-queue round per buffer under an overlapping scan
(`cucascade/src/cudf/representation_converter_builtins.cpp` `peer_copy_into_async` / `convert_gpu_to_gpu`: pipelined
staging or one train per table without mode 2's regressions, and / or a bounded in-flight copy window on the scan side
in place of `src/op/eager_probe_throttle.hpp` + the hook in `src/pipeline/gpu_pipeline_task.cpp`); headroom on top of S6
about 100 ms here.

### 6.9 Limits, unverified

- One machine, one config, no memory pressure, P2P only inside pairs: the starvation mechanism and the throttle gain
  are specific to a host-staged build move.
- Sums of the switch-1 columns in 6.2 mix tags (sessions); the f1 / fk1 / bf2 sums do not.
- q9 fk1 - f1 = -30 ms is inside the per-query noise band (3% = 42 ms) but repeats the switch-1 result of 11-19 (-27).
- The f1 vs fk1 comparison was not combined with the shadow rule or the transfer prototypes (combined branch: other
  track, `runs/combined`).
- Gate 1 / 2 exercised on TPC-H shapes only; gate 2's walk treats every non-hash-join operator as transparent.
- Mode 2 regressions (q9 q18 q10 q2) are unexplained. The bimodal iteration of the unthrottled arms was seen at q3 / q8.
- ABBA halves were sometimes minutes apart (two other agents shared the lock on 11-27).

## 7. Files changed and owning PR

| piece | files | owner |
|---|---|---|
| Contract "build PARTITION finalize implies the join's session is terminal" (already true at the tip; today a `D_ASSERT`). State it in `dynamic-filters.md`, keep one test | `src/op/dynamic_filter/hash_join_partition_observer.cpp:60-65`, docs | **#2570** (it owns the session and the call site) |
| Eager activation (option 1): listener in `bind_partition_inputs`, `activate_probe_after_build_partition`, knob, admission rule, tests, docs (`pipeline-execution.md` "the scheduler is filter-agnostic", `task-creator.md` activation rules, `dynamic-filters.md` ordering of the immediate probe, GLOSSARY "Early Probe Activation") | `src/op/partition/consumer_partition_policy.cpp`, `src/op/sirius_physical_hash_join.{hpp,cpp}` | **follow-up PR on top of the stack** (S6 / #1825) |
| Eager activation for joins without a filter plan (switch 2) | same function | same follow-up, separate commit (general scheduler change, no DF dependency) |
| Priority / pacing of the build move against an overlapping scan (section 6) | task executor or cuCascade converter | follow-up, or the transfer PR ([xfer-PR]) if done in the copy path |
| Option 2 (deferred publication + activation at its end) | OP1B's 12 files + 4 | not proposed for the stack; see section 6 |
| #2569 | - | nothing |

## 8. Production issues to solve (the prototype ignores them)

1. **Build move starvation** (section 5.2): the single largest loss. Needs a real answer: copy priority for the build
   CONCAT's move, pacing of the overlapped scan, or a transfer that does not queue behind the scan's copies.
2. **Memory admission.** The probe output now queues before any build has landed and while the build is still being
   replicated. Here it fits (no reservation wait, no downgrade), on this config only. The S6 plan's "Memory Admission"
   rule and a knob are needed for tighter machines; test under memory pressure (downgrade of queued probe batches,
   OOM retry of a CONCAT task that lost the race for memory to scan tasks).
3. **Listener context.** The listener runs under the PARTITION pipeline's status lock and calls `task_creator::schedule`
   (enqueue only, same call as `notify_downstream_pipelines` makes outside the lock). Move the nomination out of the lock
   (e.g. into the consumer notification) in a landable version.
4. **Join types and modes.** Prototype covers BUILD_PROBE inner / left-family joins. Not covered: MARK, right-family
   (already eager through BUILD_FILTER), STANDARD / MIXED_JOIN, nested-loop, delim joins. Empty build never enters
   BUILD_PROBE, so it is unaffected; a zero-row non-empty build was not tested.
5. **Probe side finishing before the build lands** (q12, q16, q7, q3 j15, q8 j41, q5): works (results identical, the join
   starts when the build arrives), but no unit test exercises it.
6. **Multi-filter scans** wait for the last join on their probe path by construction (the scan is reachable only through
   those joins' activations). Verified on q2, q7, q8, q21 (0 scan tasks before any of their publications). Needs a test
   for a plan where a filter's target sits in the build subtree of a lower join.
7. **Failure / cancel paths**: activation after a failed or cancelled publication (session terminal without filters)
   scans unfiltered, as today. Query teardown while the eager request is queued relies on the creator's submission gate
   (not tested).
8. **Priorities.** Scan tasks of the probe pipeline and the build CONCAT now coexist in the executor queues; the CONCAT
   is not delayed at dispatch (starts 0.1 ms after publication) but one executor thread per GPU is held by a starved
   CONCAT task for the whole scan.
9. **Switch 2** (joins without DF) removes the only throttle on unfiltered probe output of such joins; here that output
   is already fully buffered before the join starts, elsewhere it may not be.

## LOGPASS (completes section 4)

TRACE pass of the prototype (`runs/eager/logpass`, eager=1, 22 queries, all rc=0), hot iteration, against the tip pass
`runs/baseline/logpass`. Tools: `runs/baseline/bin/parse_df.py` + `bin/cmp_df.py` (`out/logpass_cmp.txt`) and
`bin/cmp_decode.py` (`out/logpass_decode_cmp.txt`).

- Filters: 47 pushed at the tip, 47 in the prototype; per query the key list (join, condition, kind, size, pushed, reason),
  constructions, Bloom bytes and HOST staging are equal in 22 of 22 queries. DYNAMIC_FILTER operator rows in / out: equal.
- Per-chunk compressed-scan decode lines (`decode APPLIED ... join_filters=N survivors=a/b`): 21 of 22 queries have the
  same number of chunks, the same number of chunks with filters attached and the same survivor / row totals
  (q3: 52 of 53 chunks with 1 filter, 181,951,153 of 7,499,989,709 rows, both builds).
- Difference 1 (8 queries): the *measurement* line `join filters kept ... N of M probe(s) ran` is printed 2 - 4 times less
  often (q3 24 -> 20 chunks, q21 17 -> 13). It is printed only for chunks that carry a usefulness measurement, and those
  are the chunks in flight before the gate has its first observation: with the build CONCAT holding one executor thread
  per GPU, 3 instead of 4 scan tasks start together. Every line still reads `N of N probe(s) ran`.
- Difference 2 (q9): the lineitem scan's chunks that try the 2-filter pushdown before the gate turns it off (keep ratio
  0.99) are 12 instead of 16, same cause. Applied chunks and survivors are equal.
- So: same filters built, published and attached; every chunk that probes runs all its probes; the count of *measured*
  chunks moves with scan concurrency. Not a coverage loss.

## CHECKPOINT LOG (chronological; newest last)

- 2026-11-27 (close-out session, new author). Inventory of the 2026-11-19/20 re-run from `runs/eager/*/status.txt`
  (`bin/checks.sh`): v1 16/16, b2 36/36, c3 16/16, c4 86/86, fs 8/8, fs2 156/156, gt 23/24 processes rc=0 (gt q2: all 6 ran,
  one g2 dir and its status line lost in the backup). Derived tables survive in `runs/eager/out/` (calc_table.md,
  suite_table.md, gate_arms.md, gatelog_f1.md). Lost in the wipe: all Quent telemetry, `runs/baseline/logpass/df_summary.json`,
  result files of q11 q16 q20 (> 512 KiB).
- Missing for the task questions -> new batch tag `r7` (plan `runs/eager/plans/r7.plan`, 176 processes, queue script
  `runs/eager/bin/queue_r7.sh`, progress `runs/eager/logs/r7_q*.out`): (1) g2 (option 1, switch 1) and k1 on the 10
  queries where section 6.5 assumed "g2 = f1"; k1 on q14 q18; (2) new arm `fk1` = f1 + scan throttle K=1 (added to
  `bin/unit2.sh`) with tip and f1 on all 22 queries; (3) Quent + result files of f1 on all 22 (visibility check, results).
  Before it: TRACE tip log pass `runs/baseline/logpass` (regenerates df_summary.json for `runs/profile/bin/quent_df.py`).
- `wt-eager2` recreated from `proto/eager-opt2` fdd2caec + `proto/cucascade-batch-staging.patch` applied in the cucascade
  submodule (uncommitted there, as before); build log `logs/eager2_build_r7.log`.
- 2026-11-27 19:44 UTC. Build of `wt-eager2` rc=0 (422 s). TRACE tip log pass done (22/22 rc=0, 46 DF-constructing joins;
  `runs/baseline/logpass/df_summary.json`, tables `df_tables.md`). It took 76 min of wall time: two other agents share the
  lock. Plan r7 therefore trimmed before it started to 118 processes in 12 holds (`plans/r7.plan`): arms that are the tip
  code path by construction are not run (g2/k1 where no join has a filter plan: q14 q15; any arm on q1 q4 q6 q13, which
  have no eager activation: one f1 process each for results and the log check); where every activated join has a filter
  plan (q11 q17 q19 q20) switch 1 = switch 2, so g2 = f1 and k1 = fk1 by construction.
  Started 19:44; progress in the task output of `bin/queue_r7.sh` and `runs/eager/logs/r7_q*.out`, per-process rc in
  `runs/eager/r7_*/status.txt`.
- 2026-11-27 21:05 UTC. r7 done: 118 / 118 rc=0 (`runs/eager/r7_*/status.txt`). Checks run (CPU): results 22 / 22 identical
  over 425 files; 0 fallback / error / retry lines; gate log `out/gatelog_r7_f11.md`; Quent visibility
  `out/qdf_r7_{f1,fk1,tip}.md` (0 scan tasks before any publication). Table `out/r7_suite_table.md` (`bin/r7_tables.py`).
- 2026-11-27 21:07 UTC. Section 6 replaced by the final text (source copy `notes/PROTO_EAGER_sec6_final.md`;
  `PROTO_EAGER_sec6_template.md` is the previous author's draft, superseded). Nothing pending for this track. Not done,
  out of scope here: independent verification of the prototype, production code (architect -> developer -> reviewer).
  `wt-eager2` (fdd2caec + cuCascade patch applied uncommitted in the submodule) is built; `wt-eager` was not recreated.
