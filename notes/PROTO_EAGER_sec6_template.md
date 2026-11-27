SUPERSEDED 2026-11-27 by PROTO_EAGER.md section 6 (final). Kept as the previous author's draft; placeholders below were never filled here.

## 6. Throttle, option 2, S6 release condition, contention, final table (re-run 2026-11-19/20 after the wipe)

All raw data of sections 1-5 was lost in the wipe; everything in this section was measured again. Both worktrees were
rebuilt (`wt-eager` f05cfd12 = the recovered option 1 commit; `wt-eager2` = `proto/eager-opt2`, now with 3 more commits:
ca0c150b gate variants + Memory Admission placeholder + scan-only throttle, cf26812b and @@HEAD@@ the cuCascade transfer
patch `proto/cucascade-batch-staging.patch`). Tip = `$R/repo` binary, untouched. Config file untouched.

### 6.0 What ran

- Protocol: fresh process per query, 6 iterations (1 warm + 5 hot), first hot dropped, medians of 8 hot samples per arm
  (2 processes x 4); arms in ABBA order (list once, then mirrored); every process under `flock $R/machine.lock`, one hold =
  6 to 12 processes (2 to 4.5 min). A process costs about 22 s. Deltas are always against the tip processes of the same
  holds. Deviation: the mirrored half of a query sometimes ran in a later hold (other agents in between).
- Processes: @@PROCS@@. All rc=0.
- Tags (dirs `runs/eager/<tag>_<arm><n>/`, Quent `telemetry/eager_<tag>_<arm><n>/q<N>/`):
  `v1` tip vs `pro` (wt-eager); `b2` q3 q8 q21 with tip o1 k1 k2 k3 o2; `c3` batched staging mode 1 (q3, q8);
  `c4` calculus (plans `calc2`, `calc3`); `fs2` full suite (plans `suite2-4`); `gt` S6-literal gate; nsys `runs/profile/eg_*`.
- Arms (all on `wt-eager2` except tip and pro; option 1 = publication inside PARTITION, activation at PARTITION finalize):

| arm | switches |
|---|---|
| tip | `$R/repo` |
| pro | `wt-eager`, `SIRIUS_EAGER_PROBE=1` (recovered option 1 commit) |
| o1 | `SIRIUS_EAGER_PROBE=1` (option 1, joins with a filter plan, no gate beyond the own session) |
| g1 / g2 | o1 + `SIRIUS_EAGER_GATE=1` (S6 literal: all Relevant Producers Terminal) / `=2` (channels of the scans this activation starts are Terminal) |
| f1 | `SIRIUS_EAGER_PROBE=2` (every BUILD_PROBE join) + gate 2 + `SIRIUS_EAGER_ADMIT_GIB=16` = the S6 shape |
| k1 / k2 / k3 | o1 + `SIRIUS_EAGER_SCAN_THROTTLE=K`: at most K scan tasks per GPU in `Preparing` while an eagerly overlapped build move is pending |
| o2 | option 2: `SIRIUS_DF_DEFER_PUBLISH=1` (OP1B deferred publication) + activation callback when the publication is done |
| bs / bs2 | o1 + cuCascade `CUCASCADE_PROTO_BATCH_STAGING=1 / 2` (6.3); `b02` = mode 2 with eager off; `bs2k` = bs2 + k1; `bf2` = f1 + mode 2 |
| adm | o1 + gate 2 + `SIRIUS_EAGER_ADMIT_GIB=1000` (Memory Admission never granted) |

- Correction of the first throttle (section 6 was never written, the partial numbers were in the recovery notes): the old
  hook counted every task, the build CONCAT's own tasks included (`pipeline->get_source()` is the first operator of any
  pipeline). So the old "K=1" was "nothing overlaps the build move" (= tip, +4 ms) and the old "K=2" was "CONCAT + 1 scan
  task". The throttle now counts scan-sourced tasks only; new K=1 is the old K=2 (`SIRIUS_EAGER_THROTTLE_ALL=1` restores
  the old behaviour).

### 6.1 Option 1 re-verified (step 1)

`v1`, tip vs `pro` (wt-eager), n = 8 hot per arm: q3 -31.7 ms (661.8 -> 630.0), q8 -39.2 (935.5 -> 896.3), q21 -50.8
(1438.7 -> 1387.9), q16 -22.8. Sum -145 ms (lost run: -167). The same code on `wt-eager2` (`o1`) gives q3 -43.7, q8 -38.7,
q21 -64.1, q16 -21.6 = -168, and gate 2 in the suite run (`g2`) -47.9 / -42.4 / -57.5 / -23.3 = -171. Confirmed.

New observation: the unthrottled eager arms are bimodal at q3. One hot iteration in 4 of 9 processes takes 693 ms (30 ms
*slower* than tip) instead of 613. Quent (`iterpipes.py`, `b2_o11` iterations 4 vs 5): the build CONCAT `Preparing`
takes 447 ms instead of 372 and the build lands 136 ms after the scan end instead of 58. Same mechanism as 6.3, worse
draw. k1 and bs2 do not show it (spread 594..603 and 584..595).

### 6.2 The calculus (step 2): option 1 vs throttle K vs option 2 vs un-starved build move

Delta vs tip in ms, n = 8 hot per arm and query (`final_tables.py calc`, raw per-arm tables `out/calc_*_arms.md`).
`-` = not run. q4 and q6 were dropped from this batch to save lock time (no BUILD_PROBE DF site; they are in 6.5).

@@CALC@@

Sums over the 8 slack queries: **option 1 -244 ms, option 1 + scan throttle K=1 -322 ms, option 2 -246 ms, option 1 +
two-phase staging (bs2) -316 ms.** K=2 and K=3 (q3, q8, q21 only): -147 and -140 against -147 for option 1 on the same
three, i.e. no effect: with 4 pipeline threads per GPU the CONCAT task already holds one, so K=3 is option 1, and K=2
still leaves two queued scan trains in front of every build buffer.

Reading:

- Option 1 (o1) reproduces the lost run (q3 -44, q8 -39, q21 -64, q5 -45, q16 -22, q7 -22, q12 -7, q9 -3).
- K=1 is the best pure-scheduling variant: q3 -61, q8 -72, q9 -30, q5 -49, flat elsewhere. It works because the build
  then lands before the scan ends (no tail), at the price of a slower scan (q3 scan chain 370 ms vs 337 unthrottled vs
  316 at tip).
- Option 2 is not better than option 1 on the suite (-246 vs -244) and is the most invasive (OP1B's deferred
  publication, 12 files). Per query it trades: better where the scan is short (q12 -15 vs -7, q7 -30 vs -22, q9 -19 vs
  -3), worse where the finish is long (q21 -35 vs -64, q3 -31 vs -44). Mechanism (Quent, q3 j15): the deferred finish now
  overlaps the build move instead of the scan, and the two host-staged transfers starve each other: publication at 258 ms
  instead of 182, i.e. F grows from 57 to 135 ms and the scan starts that much later.
- bs2 (a transfer change, not a scheduling change) gets the same total as K=1 without slowing any scan on purpose.
  bs2 + K=1 is not additive (q3 -65, q8 -72, q21 -57).
- Controls: q10 +0.9 (k1) / 0.0 (o2); q2 -26 / -27 (j28, as in the lost run).

### 6.3 Contention between the eager scan and the build CONCAT move (step 4)

Evidence: nsys captures of q3 (`runs/profile/eg_{tip,o1,k1,bs2}`, 3 iterations, last hot window; `nsys_move.py`,
`nsys_seg.py`; nsys times are inflated, used for mechanism only) and Quent per site (`esites.py` + `sites_compact.py`,
`out/sites_calc.md`, medians of 8 hot iterations).

**Which resource.** The target GPU's host-to-device copy queue (the ingress link). Nothing else:

| candidate | finding (q3 j15, CONCAT(10) vs GPU_SCAN(11)) |
|---|---|
| host-to-GPU link | During the overlap every GPU's host-to-device path is 98% busy: scan 72-77% + move 7-9% of the 405 ms move window (the scan covers 84% of it). The scan alone saturates it: scan throughput per GPU is the same with 1, 3 or 4 concurrent scan tasks (task duration 32.5 / 88 / 113 ms = 0.031 / 0.034 / 0.035 tasks per ms). |
| GPU-to-host (egress) | not contended: the move's device-to-host halves start at once (6-9% busy, move only) |
| peer copies (pair 0-1, 2-3) | start at once; 2x slower under the scan (546 MiB: 11 -> 25 ms) |
| pinned host pool | `allocate_multiple_blocks` 0.0 ms inside the move tasks; capacity not an issue |
| pipeline threads | CONCAT tasks start 0.1 ms after publication in every arm; they then hold 1 of 4 threads per GPU, blocked in `cudaStreamSynchronize` for 346-382 of 382-405 ms. The scan does not miss the thread (link-bound). |
| GPU compute | not involved (copy time of the move 61-67 ms at tip, 70-88 ms under the scan) |

**Why the move starves instead of sharing.** It is head-of-line blocking, not bandwidth sharing. The move
(`cucascade/src/cudf/representation_converter_builtins.cpp`, `peer_copy_into_async`) handles one buffer at a time:
device-to-host train, host-to-device train, stream sync. Each scan task queues its whole chunk as one train of 1 MiB
host-to-device copies (about 28 ms of link time). The driver serves the link first-come-first-served by trains, and the
move's host-to-device train enters the queue only when its device-to-host half is done, behind everything already queued:

| arm | wait of each host-staged buffer before its host-to-device half starts | buffers finished while the scan runs | move task duration |
|---|---|---|---|
| tip (no scan) | 0 | - | 105-120 ms |
| o1 (3 scan tasks per GPU) | 82-85 ms = 3 queued scan trains | 4 of 10 segments in 341 ms; the other 6 take 45 ms once the scan has ended | 382-405 ms |
| k1 (1 scan task per GPU) | 25-30 ms = 1 train | all 10 | 314-348 ms |
| bs2 (two-phase staging) | 60-67 ms, but only once per source batch (2 per task) | all | 199-290 ms |

So the move's own link time is small (1.1 GB per GPU = about 35 ms); what it loses is one full round of the scan's queue
per buffer. That is the "build tail": the build lands after the scan has ended and the join waits for it.

**Size of the loss** (8 slack queries, sites with a CONCAT window >= 20 ms, Quent medians; full table `out/sites_calc.md`):

| arm | sum of build tails (build lands after scan end) | sum of site gains (max(CONCAT end, scan end) - publication, tip minus arm) | wall, 8 queries |
|---|---|---|---|
| o1 | 289 ms | 266 ms | -244 ms |
| k1 | 119 ms | 299 ms | -322 ms |
| bs2 | 110 ms | 334 ms | -316 ms |

The 332 ms tail of section 5 is reproduced (289 ms here).

**Does throttling the scan recover it?** Partly: K=1 removes 170 of 289 ms of tail and gains 78 ms more than option 1,
but it slows every overlapped scan (q3 +54 ms scan chain vs tip, against +20 unthrottled) and only K=1 works. It is a
knob that depends on the thread count and on the transfer being slow; not a design.

**Does giving the build move priority recover it?** Strict priority (nothing overlaps the move) is the tip: measured +4
ms in the lost run (old K=1). Both sides are bound by the same link, so serial order gains nothing. What recovers the
tail is to stop paying one queue round per buffer:

- `CUCASCADE_PROTO_BATCH_STAGING=1` (drop the per-buffer sync, same copy order): no gain (q3 -32, q8 -31 with eager;
  +6 / +20 with eager off). The per-buffer wait is not caused by the host thread's sync but by the device-to-host ->
  host-to-device hop of each buffer.
- `CUCASCADE_PROTO_BATCH_STAGING=2` (all device-to-host copies of a table first, then all host-to-device copies as one
  train, one sync per table; 60 lines in `peer_copy_into_async` / `convert_gpu_to_gpu`): **q3 -74 (o1 -44), q8 -74 (-39),
  q5 -55 (-45)**; no change at q16, q12, q9, slightly worse at q7 (-16 vs -22) and q21 (-59 vs -64). With eager off it
  costs +4 (q3) and +7 (q8), so it is not free at the tip.
- Not good enough as it stands: in the full suite (6.5, `bf2` vs `f1`) it loses at q9 (-47 vs -86), q2 (-8 vs -23) and
  @@BF2LOSS@@. Cause not investigated (the two-phase order delays every host-to-device byte until the last
  device-to-host byte of the table is out; small and hash-partitioned moves pay for it). It proves the mechanism; the
  landable shape (pipelined two-stream staging, or a bounded in-flight window on the scan side) belongs to the transfer
  PR.

What a better transfer can still recover on top of the S6 shape, measured upper end: the tails that remain
(o1 289 ms; bs2 still leaves 110 ms), i.e. roughly 100-180 ms per suite pass on this machine. On a machine with full
peer-to-peer the move has no host hop and this loss should not exist in this form (not measurable here).

### 6.4 Release condition: prototype vs the user's rule vs S6 (step 3)

S6 (#1825): `normal_build_deposit_ready || (layout_stable && target_producers_terminal && memory_admitted)`.
GLOSSARY: *Relevant Producers* = every Producer registered on a Channel the join publishes to, including Producers held
by other joins.

| term | first prototype (sections 1-5) | S6 | match | now in `wt-eager2` |
|---|---|---|---|---|
| ordinary activation kept | yes: the join hint is unchanged | required | yes | unchanged |
| layout_stable | finalize listener of the build PARTITION | explicit build-PARTITION completion source | yes in effect; S6 wants the notification outside locks (the listener runs under the pipeline's status mutex) | unchanged (prototype) |
| target_producers_terminal | own session Terminal only; other producers Terminal "by induction" (a scan is reachable only through the joins on its probe path) | every Relevant Producer Terminal, re-evaluated on the last producer's terminal notification | **no** | `SIRIUS_EAGER_GATE=1`: every channel in the join's publish plan must be Terminal (`sirius_dynamic_filter_set::snapshot().terminal()`), else the join is held and ordinary activation applies. No re-evaluation on a later terminal transition (see below). |
| user's rule: a scan targeted by several filters waits for all of them | by induction only, checked after the fact in Quent (0 scan tasks before any publication) | implied by the term above | implicit | `SIRIUS_EAGER_GATE=2`: explicit check at the activation: every scan this activation starts (static walk from the probe head; at a lower hash join only its build side) has a Terminal channel |
| memory_admitted | none | required; "retain normal scheduling when safe admission cannot be demonstrated" | **no** | placeholder `SIRIUS_EAGER_ADMIT_GIB=N`: every GPU must have N GiB available at the activation, else held. Not a reservation: it demonstrates the fallback path, not the rule. |
| a filter need not exist | switch 1 only joins with a filter plan; switch 2 all BUILD_PROBE joins | condition does not mention filters; "a terminal no-filter result does not itself justify early work" (admission must) | switch 2 = S6 shape | `f1` = switch 2 + gate 2 + admit |

Measured (INFO line `[eager] join N probe activated|held ...` of every activation, `gatelog.py`, `out/gatelog_f1.md`):

@@GATE@@

Verdict on the release condition:

1. **The user's rule holds, and is now checked instead of argued**: 0 pending channels among the scans started by any
   activation (gate 2 never holds a join). Every multi-filter scan (q2 scan 21, q21 scan 13, the non-direct targets of
   q7 j38 / q8 j44 / q21 j21) started after all its filters were published.
2. **The first prototype did not match S6's `target_producers_terminal`.** With the literal rule (gate 1) the joins that
   share a channel with a lower join of their own probe subtree are held, because that lower join cannot become Terminal
   before this join releases its probe side: @@G1JOINS@@. They fall back to ordinary activation; no hang, results
   identical. Cost: @@G1COST@@. So the literal S6 rule is safe and almost free here, but it can never open for stacked
   probes; the rule that keeps the gain and still enforces the user's rule is gate 2 ("the channels of the scans this
   activation starts are Terminal"), which is also cheaper to wire: no subscription to other joins' sessions is needed
   on any shape seen (it never held). Recommendation for S6: define `target_producers_terminal` over the endpoints the
   activation starts, not over the join's own publish plan; keep the literal rule as the documented fallback for shapes
   where the walk cannot be computed.
3. Re-evaluation on "last producer terminal": not implemented. On TPC-H it would never fire (the only pending Relevant
   Producers are probe-subtree joins, which need this join's release first). Needed in S6 for unrelated producers
   (union / delim / CTE shapes), untested here.
4. **Memory Admission**: the lowest GPU availability at any activation in the suite was @@MINAVAIL@@ GiB of 92; the
   16 GiB placeholder always passes. `adm` (never admitted): 12 held lines, q3 -3.7 ms vs tip, results identical, i.e.
   the hold path costs nothing and falls back cleanly. A real rule (reserve the build move + hash tables before letting
   probe output queue) was not built and cannot be validated with this config.
5. **Switch 2 on the full suite** (S6 does not require a filter): see 6.5. @@SW2@@

### 6.5 Final table: the recommended shape (step 5)

Recommended shape = `f1`: option 1 (finish inside PARTITION, then schedule the probe side at PARTITION finalize), every
BUILD_PROBE join, release gate on the started scans' channels, Memory Admission placeholder. `g2` = the same restricted
to joins with a filter plan (switch 1). `bf2` = `f1` + two-phase staging (transfer-PR lever, not part of the shape).
`fs2`, n = 8 hot per arm, spread = max - min. On the 10 queries without an eager site (q1 q4 q6 q11 q13 q15 q17 q19
q20 q22) `g2` was not run: it takes the same code path as `f1` there; its sum uses the `f1` value.

@@SUITE@@

@@SUITEREAD@@

### 6.6 Verdict

@@VERDICT@@

### 6.7 What the stack must provide, what belongs to S6, what belongs to the transfer PR

**#2569 (S5B1)**: nothing to change for S6, and S6 cannot sit on #2569 alone: at `pr2569` the observer finalizes the
session only for eligible layouts (`hash_join_partition_observer::finalize_input`: `if (eligible())`), the other
layouts still publish one-shot at the CONCAT delivery. It already provides two things S6 reuses: `post_unlock_actions`
(decided under the session mutex, dispatched after it) and the per-endpoint terminal state
(`dynamic_filter_snapshot::terminal()`, producers registered before `freeze_registration()`).

**#2570 (S5B2) must provide / keep** (small, all inside what it already owns):

1. The contract **"build PARTITION finalize => the join's publication session is Terminal, for every layout and every
   outcome (published, skipped, failed, cancelled, plan dropped)"**. True at the tip (commits e69f84ca, 5ccdcfcd) but
   only a `D_ASSERT` (`hash_join_partition_observer.cpp:60-65`). Make it a documented guarantee in `dynamic-filters.md`
   with one test per outcome; S6's whole correctness argument is this line. Option 1 needs nothing else from the
   publication path: no deferred publication, no callback from the session.
2. Keep the publication **synchronous inside the Publishing Task, before its deposit and before PARTITION finalize**.
   Any later move of the finish off that task (OP1 / option 2 style) breaks contract 1 and turns S6 into option 2.
3. A cheap **`terminal()` on the channel** (today `snapshot()` copies the entry vector to answer it) and, if S6 keeps
   the literal Relevant-Producers rule, a **terminal notification carried by `post_unlock_actions`** (today it carries
   only log lines). With the started-scans gate the notification is not needed on any plan shape seen.
4. Nothing in the session API has to change for the early start itself; the prototype reads `plan().enabled()`,
   `plan().probe_targets()[i].filter_set` and `terminal()`.

**S6 PR (#1825)**:

- The activation: build-PARTITION completion source that fires **outside** the pipeline status mutex (today's
  `add_finalize_listener` runs under it), weak query/task targets, coalescing through the task-request mechanism
  (`task_creator::schedule` is what the prototype calls), nomination of the build CONCAT before the probe head.
- The release gate (6.4) and its re-evaluation, the internal ablation switch, observability (the `[eager]` line),
  docs (`pipeline-execution.md` "filter-agnostic", `task-creator.md`, GLOSSARY).
- Memory Admission for buffered early probe output (the prototype has a placeholder only).
- Joins without a filter plan (switch 2): same hook; gain is as large as the DF part (6.5).
- Tests the prototype does not have: probe pipeline finishing before any build batch lands (q12, q16, q7 do this in
  every run, results identical), both notification orders, zero-filter terminals, teardown with a queued request, MARK /
  right-family / STANDARD / MIXED / delim joins (excluded in the prototype), memory pressure.
- Optional, as an internal knob only: the scan throttle (K=1) as a stop-gap until the transfer PR lands. Not recommended
  as the design.

**Transfer PR [xfer-PR]** (not #2569 / #2570 / S6): the build move must stop paying one ingress-queue round per buffer
when a scan overlaps it (6.3): pipelined host staging on two streams or one train per table, and/or a bounded in-flight
copy window in the scan. Measured headroom on top of S6: q3 -27, q8 -38, q5 -11 ms with the crude two-phase prototype,
which is not landable as is (regressions at q9, q2). The DF finish (F, 600 ms per suite pass, unchanged by anything
here) remains the other transfer item; with option 2 ruled out it stays on the critical path 1:1.

### 6.8 Limits, unverified

- One machine, one config, no memory pressure; P2P only inside pairs. On a full-P2P machine the build move has no host
  hop: the CONCAT window is shorter (smaller bound) and the starvation mechanism of 6.3 should not apply as measured.
- nsys: one hot iteration per arm, q3 only. The queueing discipline of the driver is inferred from those timelines.
- Two-phase staging: prototype quality, no correctness test beyond identical results in @@BSPROCS@@ processes; its
  regressions at q9 / q2 are not explained.
- Gate 1 / gate 2 were exercised on TPC-H shapes only; gate 2's static walk treats every non-hash-join operator as
  transparent (conservative) and was never the reason for a hold.
- The Memory Admission placeholder is a threshold, not a reservation.
- ABBA halves of a query were sometimes separated by other agents' jobs (minutes apart).
- No unit tests were run on the prototype builds; no TRACE log pass was repeated (the lost run's log pass found equal
  filters, section LOGPASS).
- The bimodal iteration of the unthrottled arms (6.1) was seen at q3 only; other queries were not searched for it.
