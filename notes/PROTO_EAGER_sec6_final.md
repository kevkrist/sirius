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
