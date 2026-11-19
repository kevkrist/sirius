# PROTO_EAGER: eager probe-side scheduling when the build PARTITION has finished (option 1), measured

2026-11-18/19. Worktree `/opt/dlami/nvme/dfperf/wt-eager`, branch `proto/eager-probe` (from pr2570 `ae939824`), commit `092a0e3a`.
Second worktree `wt-eager2`, branch `proto/eager-opt2` (= `proto/op1-minimal` + the same patch + option 2 + scan throttle),
commits `49e651f6`, `0cd75dcb`, `2abda103`. Local commits only, nothing pushed.
Scripts `$R/runs/eager/bin/`, raw runs `$R/runs/eager/<label>/`, Quent `$R/telemetry/eager_<label>/q<N>/`, derived tables
`$R/runs/eager/out/`. Every build and GPU process ran under `flock $R/machine.lock`. Config file untouched.
Throwaway prototype: no docs, no tests.

STATUS: sections 1-5 are final for option 1 (batch `a1`). Section 6 (scan throttle, option 2; batch `b1`) is filled in
at the end of the file once those runs finish; if it says PENDING, they did not finish.

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

## 6. Throttle, option 2, S6 gate, contention (2026-11-19 re-run after the wipe) - DRAFT, replaced by the final text below when done

WORK IN PROGRESS (handoff state, written while batches run; final tables replace this block).
- Rebuilt `wt-eager` (f05cfd12) and `wt-eager2` (now ca0c150b + cf26812b + mode-2 commit): gate variants
  `SIRIUS_EAGER_GATE` 0/1/2, Memory Admission placeholder `SIRIUS_EAGER_ADMIT_GIB`, scan-only throttle
  (`SIRIUS_EAGER_THROTTLE_ALL=1` = first version, which also counted the CONCAT's own tasks),
  cuCascade patch `proto/cucascade-batch-staging.patch` (`CUCASCADE_PROTO_BATCH_STAGING` 1/2).
- Scripts: `runs/eager/bin/{unit2,plan,batch2,nsys3}.sh`, `{arms,raw,gatelog,iterpipes,nsys_move,nsys_seg}.py`, plans
  `runs/eager/plans/*.plan` (copy in `notes/scripts_eager/`). Tags: `v1` (tip vs pro), `b2` (q3 q8 q21: tip o1 k1 k2 k3 o2),
  `c3` (q3 q8: batched staging mode 1), `fs` (q3 only), `c4` = calc2.plan, `fs2` = suite2.plan (running).
- Results so far (ms vs tip, n = 8 hot per arm): v1 pro q3 -32, q8 -39, q21 -51; b2 q3 o1 -44 / k1 -61 / k2 -34 / k3 -44 /
  o2 -31; q8 -39 / -72 / -51 / -33 / -52; q21 -64 / -62 / -62 / -64 / -35. Batched staging mode 1: no gain (q3 -32, q8 -31;
  with eager off +6 / +20).
- Mechanism (nsys q3, `runs/profile/eg_{tip,o1,k1}`): the build move's host-to-device half waits one whole round of the
  scan's queued copy trains per buffer (about 84 ms with 3 scan tasks per GPU, 28 ms with 1), 10 buffers per task;
  device-to-host and peer halves are not delayed. See `nsys_seg.py`.

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
