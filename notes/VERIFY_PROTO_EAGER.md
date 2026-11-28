# VERIFY_PROTO_EAGER: independent, adversarial verification of notes/PROTO_EAGER.md section 6 (S6 eager scans)

2026-11-27/28. Verifier (not the author). Prototype worktrees untouched (`wt-eager2` fdd2caec + cuCascade patch, built by the
author; tip = `$R/repo` ae939824). Own scripts `$R/runs/veager/bin/`, raw runs `$R/runs/veager/<tag>_<arm><n>/`, Quent
`$R/telemetry/veager_<label>/`, derived tables `$R/runs/veager/out/`. Every GPU process under `flock $R/machine.lock`
(100 timing / log processes + 9 shape processes, all rc=0). Config untouched, env switches only. Nothing built, nothing committed
to a code branch (no code was written).

STATUS: FINAL (2026-11-28 01:10 UTC).

## 0. Verdicts

| # | claim (PROTO_EAGER 6.x) | verdict | evidence |
|---|---|---|---|
| a1 | f1 (every BUILD_PROBE join) gains -497..-512 ms per suite pass, no regression | **CONFIRMED** | own ABBA, 7 largest contributors: -419.7 ms (author same seven: -401.7 on 11-27, -417.4 on 11-19); controls q4 +0.2, q19 -0.6, q10 -10.0 (noise); author's sums recomputed from raw csv: identical; binary control (prototype build, switch off) -4.4 ms over 5 queries |
| a2 | switch 1 (joins with a filter only) -324 ms | **NOT RE-MEASURED** (sum mixes sessions, as the author says); spot check o1: q2 -26.8, q7 -21.5 (author -25.1, -23.5) | t3 |
| b1 | throttle K=1 adds about -100 ms on top of f1 (q8 -37, q9 -30, q3 -25) and removes the slow outlier iterations | **CONFIRMED** on the three queries that carry it: q8 -31.7, q9 -29.0, q3 -18.8 = -79.5 (author -92.2 on the same three); spread q8 67 -> 4 ms, q3 21 -> 8 ms | t1 |
| b2 | K=2, K=3 do nothing | **NOT RE-MEASURED** (11-19 record only) | - |
| c1 | gate 2 never holds any of the 56 activated joins | **CONFIRMED** | 56 distinct joins, 664 activation lines in 5 process sets, 0 held, 0 with `started_pending>0`, 0 with `session_terminal=false`; also 0 held in 82 activations on 37 non-TPC-H shapes |
| c2 | gate 1 holds q2 j28, q7 j38, q8 j44, q21 j21 in every iteration; costs 35 ms | **CONFIRMED** (holds: exactly those four, every iteration; cost re-timed on q2 23.1 ms [author 21] and q7 2.6 ms [author 6, inside noise]; q8 / q21 cost not re-timed) | lp g1, t3 |
| c3 | results identical under all gates | **CONFIRMED** | TPC-H: 12 queries byte-identical over tip / f1 / fk1 / off / g1 / o1, 7 more f1 = tip log pass (q11 after sorting); 37 shapes identical on tip / f1 / fk1 / gate 1 / gate 0 and equal to DuckDB CPU |
| 2 | no scan starts before all its filters; no scan loses a filter | **CONFIRMED on what can be observed** (not refuted) | section 2 |
| 3 | memory: "lowest free GPU memory at any activation 74.7 GiB" | number reproduced (74.7 / 74.9) but **WEAKENED as evidence**: it is sampled before the overlap. Real peaks (section 3): resident batches +2.1 GiB (max +3.5) on the worst GPU at q18, nothing elsewhere; no pressure on this machine. Memory pressure and a real admission rule remain untested | section 3 |
| 4 | no hang / deadlock | **NOT REFUTED**: 0 hangs in 4 eager arms x (37 shapes x 2 + 15 interrupts); code-level risks listed in section 4 are untriggered, not disproved | section 4 |
| 5 | the proposed production design (6.8) meets the S6 acceptance criteria | **WEAKENED**: direction matches, but 4 of 8 criteria (level-triggered re-evaluation, notification outside locks with weak targets, real memory admission, the validation list) exist only as text; the measured number belongs to a prototype that does not meet them, and about 190 of the 500 ms come from joins without a filter, which criterion 6 allows only with demonstrated admission | section 5 |

Bottom line: the measurement claims hold. Nothing was refuted. What is weaker than the summary suggests is the design claim:
the S6-compliant part that is demonstrated today is "eager activation is correct and worth about 500 ms here when admission
always passes"; the admission rule, the notification plumbing and the re-evaluation are unbuilt and unmeasured.

## 1. Suite gain and throttle (own ABBA, tag t1; binary control, tag t2; gates, tag t3)

Protocol = BASELINE_SUITE section 0: fresh process per query, 6 iterations, iterations 0 and 1 dropped, 2 processes per arm in
ABBA order inside one lock hold, 8 hot samples per arm, medians. Cells: delta vs tip in ms (%; n, spread; per-process deltas).
Plans `runs/veager/plans/t{1,2,3}.plan`, table script `bin/times.py <tag> [arms]`.

| q | tip med (n, spread; tip1 / tip2) | f1 delta ms (%; n, spread; per-process) | fk1 delta ms (%; n, spread; per-process) |
|---|---|---|---|
| 3 | 663.4 (8, 89; 663.4 / 665.1) | -46.6 (-7.0%; 8, 21; -46.0 / -48.2) | -65.4 (-9.9%; 8, 8; -65.2 / -66.4) |
| 4 | 325.2 (8, 5; 325.9 / 325.2) | +0.2 (+0.1%; 8, 7; -0.2 / +0.7) | - |
| 5 | 691.5 (8, 33; 691.5 / 692.4) | -48.8 (-7.1%; 8, 19; -48.8 / -49.7) | - |
| 8 | 937.1 (8, 4; 937.1 / 937.4) | -44.9 (-4.8%; 8, 67; -40.4 / -46.0) | -76.6 (-8.2%; 8, 4; -75.6 / -76.8) |
| 9 | 1420.2 (8, 32; 1425.5 / 1416.6) | -87.1 (-6.1%; 8, 52; -84.7 / -88.6) | -116.1 (-8.2%; 8, 46; -117.2 / -112.3) |
| 10 | 857.9 (8, 17; 860.3 / 857.2) | -10.0 (-1.2%; 8, 31; -0.7 / -13.5) | - |
| 14 | 686.4 (8, 77; 696.6 / 681.9) | -52.1 (-7.6%; 8, 53; -51.8 / -52.6) | - |
| 18 | 1088.2 (8, 32; 1082.9 / 1092.3) | -87.7 (-8.1%; 8, 17; -85.7 / -91.8) | - |
| 19 | 881.0 (8, 17; 879.7 / 881.4) | -0.6 (-0.1%; 8, 14; -0.6 / -0.8) | - |
| 21 | 1444.6 (8, 26; 1444.6 / 1445.7) | -52.5 (-3.6%; 8, 30; -55.4 / -52.5) | - |
| sum | | -430.1 over tip 8995 | -258.2 over tip 3021 |

- Sum of the 7 largest contributors (q3 q5 q8 q9 q14 q18 q21): f1 **-419.7 ms**; the author has -401.7 (11-27) and -417.4
  (11-19) on the same seven. Every query agrees within 9 ms. Controls: q4 (no activation) +0.2, q19 (activation, no slack)
  -0.6, q10 (author's worst cell, +2.0 / -9.5) -10.0 with per-process -0.7 / -13.5: noise, no regression.
- The 11 queries not re-timed carry -85 ms of the author's -497; extrapolating my seven with them unchanged gives about
  -505 ms (-3.3% of 15290). Not re-measured: the 22-query sum in one session (exists only for 11-19).
- Throttle (fk1 - f1, same holds): q3 -18.8, q8 -31.7, q9 -29.0 = -79.5 ms. The unthrottled f1 has the bimodal iteration at
  q8 (spread 67 ms) and fk1 does not (4 ms).
- Recomputation of the author's raw data with my script (`out/recalc_r7.md`): r7 f1 -497.2, fk1 -596.7 (18 queries), fs2 f1
  -511.5 (22 queries): identical to 6.2.

Binary control: the eager arms run a different binary (`wt-eager2`, 16 files changed) than the tip arm. Same binary with the
switch off (`SIRIUS_EAGER_PROBE=0 SIRIUS_DF_DEFER_PUBLISH=0`) against the tip, the author did not run this:

| q | tip med (n, spread; tip1 / tip2) | off delta ms (%; n, spread; per-process) |
|---|---|---|
| 3 | 661.5 (8, 14; 661.5 / 662.5) | +1.8 (+0.3%; 8, 14; +1.8 / +1.0) |
| 9 | 1416.4 (8, 35; 1417.5 / 1416.4) | -9.8 (-0.7%; 8, 38; +3.1 / -14.4) |
| 14 | 692.3 (8, 32; 692.1 / 692.8) | -4.7 (-0.7%; 8, 30; +0.7 / -10.1) |
| 18 | 1086.5 (8, 22; 1091.7 / 1083.7) | +3.4 (+0.3%; 8, 31; +5.8 / +0.4) |
| 21 | 1443.5 (8, 26; 1443.5 / 1445.6) | +4.8 (+0.3%; 8, 26; -2.5 / +6.9) |
| sum | | -4.4 over tip 5300 |

No build effect (sum -4.4 ms over 5300): the gain is the switch.

Gate 1 against gate 0 (both switch 1):

| q | tip med (n, spread; tip1 / tip2) | g1 delta ms (%; n, spread; per-process) | o1 delta ms (%; n, spread; per-process) |
|---|---|---|---|
| 2 | 149.8 (8, 6; 150.1 / 148.1) | -3.7 (-2.5%; 8, 4; -4.1 / -3.4) | -26.8 (-17.9%; 8, 4; -26.0 / -27.0) |
| 7 | 745.6 (8, 16; 750.5 / 744.1) | -18.9 (-2.5%; 8, 10; -17.8 / -18.9) | -21.5 (-2.9%; 8, 6; -21.5 / -20.6) |
| sum | | -22.6 over tip 895 | -48.3 over tip 895 |

## 2. Ordering: filters before scans, no lost filter

Method (independent of the author's Quent tool): TRACE log pass of f1 on the 18 activated queries (`runs/veager/lp_f11`, 2
iterations, hot one compared) against the tip TRACE pass of the same day (`runs/baseline/logpass`), script `bin/cmp_logs.py`,
table `out/cmp_logs_f1.md` (g1 on q2 q7 q8 q21: `out/cmp_logs_g1.md`). Per query, hot iteration:

- `[dynamic_filter_key]` set (join, condition, rows, kind, size, pushed, reason): **equal in 18 of 18** (49 keys).
- Rows into and out of every DYNAMIC_FILTER operator (= rows the scan delivered after pushdown, and rows after the filters):
  **equal for all 43 operators in 18 of 18 queries**. A scan that started without one of its filters, or never got it,
  would deliver more rows.
- Rows produced by every operator: equal in 16 of 18; the two differences are not filter related (q9 partial group-by output
  2800 vs 2100 groups, batching; q11 sort sample 234719 vs 234355). Every scan, filter and join row count is equal.
- Compressed-scan lines `join filters kept X/Y rows (N of M probe(s) ran, complete=..)`: the same (N, M, complete) classes in
  both arms; for the scans targeted by two filters the number of `M = 2` lines is equal at q2 (4 / 4), q7 (5 / 5), q8
  (50 / 50), q20 (34 / 34), q21 (31 / 31) and lower at q9 (16 vs 12: fewer chunks measured, rows equal). No `M = 1` line
  appears in the eager arm that the tip does not have (counts are equal or lower), i.e. no chunk of a two-filter scan was
  decoded while only one filter was attached.
- Engine side: 664 `[eager] ... activated` lines (lp_f11, t1_f11, t1_f12, t1_fk11, t1_fk12): all `session_terminal=true`,
  all `started_pending=0`.
- Non-TPC-H shapes (unpinned parquet scans; section 4), TRACE, `bin/cmp_shapes_tr.py`, `out/cmp_shapes_tr_{f1,s2g0}.md`:
  72 statement windows, 56 with a DYNAMIC_FILTER operator: key sets equal in all; rows equal in 69; the 3 differences are
  tip-side nondeterminism (the filter of `declined_domain` passes 1499704589 or 1499823855 rows in two tip iterations of
  the same statement; the join under a LIMIT produces 234 - 300 M rows depending on when the limit lands). Gate 0 gives
  the same picture as gate 2.

Limits: row equality cannot see a filter that arrives late but would have removed nothing; log timestamps are 1 ms. The
author's Quent check (0 consumer tasks before any publication, smallest gap +0.03 ms) was not repeated; it and this check are
independent and agree. Argument why it holds by construction (checked in the code, `sirius_physical_hash_join.cpp:971-1066`):
a join's probe head is nominated at its build PARTITION finalize, which is after its own session is Terminal and after its
whole build subtree has finished; every lower join is first asked for its build, so its build PARTITION cannot finalize
before the upper join was activated. Gate 2 checks the same thing per started scan and never disagreed.

## 3. Memory with scans overlapping the build move

The author's figure (lowest `min_gpu_avail_gib` in the `[eager]` line, 74.7 GiB of 95.6) is reproduced (74.7 / 74.9) but it
is read at the activation, before any probe output is buffered: it is not a peak. Peaks from Quent (`bin/mem.py`; `batch` =
bytes of data batches resident in a GPU memory space, `batch+resv` = plus the reservations of tasks between Preparing and
Finalizing; per hot iteration, worst GPU, median (max) over the hot iterations; limit 85.5 GiB per GPU). Own runs, same holds:

| q | tip1 batch / batch+resv (GiB, worst GPU) | tip2 batch / batch+resv (GiB, worst GPU) | f11 batch / batch+resv (GiB, worst GPU) | f12 batch / batch+resv (GiB, worst GPU) | fk11 batch / batch+resv (GiB, worst GPU) | fk12 batch / batch+resv (GiB, worst GPU) | limit |
|---|---|---|---|---|---|---|---|
| 3 | 2.6 / 33.9 (max 2.6 / 33.9) | 2.6 / 33.9 (max 2.6 / 33.9) | 2.7 / 25.9 (max 2.7 / 25.9) | 2.7 / 25.9 (max 2.7 / 25.9) | 2.7 / 24.4 (max 2.7 / 24.4) | 2.7 / 24.4 (max 2.7 / 24.4) | 85.5 |
| 4 | 0.9 / 40.2 (max 0.9 / 40.2) | 0.9 / 40.2 (max 1.0 / 40.2) | 0.9 / 40.2 (max 0.9 / 40.2) | 0.9 / 40.2 (max 0.9 / 40.2) | - | - | 85.5 |
| 5 | 3.2 / 37.1 (max 3.2 / 37.1) | 3.2 / 37.1 (max 3.2 / 37.1) | 3.3 / 27.0 (max 3.5 / 36.3) | 3.3 / 27.0 (max 3.5 / 27.0) | - | - | 85.5 |
| 8 | 2.5 / 37.9 (max 2.5 / 37.9) | 2.5 / 37.9 (max 2.5 / 37.9) | 2.7 / 30.8 (max 2.7 / 30.8) | 2.7 / 30.8 (max 2.7 / 30.8) | 2.7 / 25.3 (max 2.7 / 25.3) | 2.7 / 25.3 (max 2.7 / 25.3) | 85.5 |
| 9 | 14.2 / 46.2 (max 14.2 / 46.2) | 14.1 / 46.2 (max 14.2 / 46.2) | 10.8 / 36.3 (max 10.8 / 36.3) | 10.8 / 36.3 (max 10.8 / 36.3) | 14.0 / 32.9 (max 14.2 / 33.2) | 14.0 / 28.7 (max 14.2 / 28.9) | 85.5 |
| 10 | 5.5 / 35.3 (max 5.6 / 35.3) | 5.5 / 35.3 (max 5.6 / 35.3) | 5.5 / 33.1 (max 5.6 / 33.1) | 5.5 / 33.1 (max 5.6 / 33.1) | - | - | 85.5 |
| 14 | 8.4 / 40.5 (max 8.4 / 40.5) | 8.4 / 40.5 (max 8.4 / 40.5) | 8.4 / 35.4 (max 8.4 / 35.4) | 8.4 / 35.4 (max 8.4 / 35.4) | - | - | 85.5 |
| 18 | 17.5 / 53.9 (max 17.5 / 56.0) | 17.5 / 54.8 (max 17.5 / 55.2) | 19.6 / 56.5 (max 21.0 / 57.3) | 19.6 / 57.1 (max 19.6 / 58.7) | - | - | 85.5 |
| 19 | 0.1 / 48.8 (max 0.1 / 48.8) | 0.1 / 48.8 (max 0.1 / 48.8) | 0.1 / 48.8 (max 0.1 / 48.8) | 0.1 / 44.6 (max 0.1 / 48.8) | - | - | 85.5 |
| 21 | 4.3 / 45.8 (max 4.3 / 45.8) | 4.3 / 45.8 (max 4.3 / 46.2) | 4.3 / 35.9 (max 4.8 / 36.0) | 4.3 / 35.9 (max 4.3 / 36.0) | - | - | 85.5 |
worst tip1: batch 17.5 GiB, batch+resv 56.0 GiB at q18
worst tip2: batch 17.5 GiB, batch+resv 55.2 GiB at q18
worst f11: batch 21.0 GiB, batch+resv 57.3 GiB at q18
worst f12: batch 19.6 GiB, batch+resv 58.7 GiB at q18
worst fk11: batch 14.2 GiB, batch+resv 33.2 GiB at q9
worst fk12: batch 14.2 GiB, batch+resv 28.9 GiB at q9

- Resident batches: equal except **q18 +2.1 GiB (17.5 -> 19.6, max 21.0)** and q9 -3.4 GiB (f1) / equal (fk1); q3 q5 q8
  +0.1..0.2. Author's r7 telemetry gives the same picture on all 18 queries (`out/mem_r7.md`).
- Batches + task reservations: lower with eager scans at q3 q5 q8 q9 q14 q21 (-5..-10 GiB; fewer scan tasks hold a
  reservation at the same time while the build move occupies a thread), +2.6..3.3 GiB at q18. Highest f1 value 58.7 GiB
  (q18), tip 56.0.
- Logs: 0 OOM / retry / downgrade lines in any process.
- So: no memory problem on this machine, and the cost is confined to the join without a filter that buffers a large probe
  side (q18). Not tested by anyone: memory pressure, downgrade of queued probe batches, a real admission rule
  (`SIRIUS_EAGER_ADMIT_GIB=16` always passes). The S6 criterion on admission is therefore unvalidated.

## 4. Deadlock / hang

`bin/shapes.py` (+ `shapes.sh`): 37 statements on SF1000 (unpinned parquet views), each twice, watchdog 240 s, then 15
`interrupt()` calls 30 - 400 ms into a running join query, each followed by a normal query. Arms tip, f1, fk1, s2g1
(switch 2 + gate 1), s2g0 (switch 2 + gate 0), and DuckDB CPU as oracle. Table `out/shapes_table.md`, raw
`runs/veager/shapes/<arm>/{results.jsonl,log/}`.

Shapes: join without a filter plan, filter declined / near-full domain, empty build (inner, left), one-row build, empty probe,
two and three filters on one scan, 4-deep stacked probes, self-joins, LIMIT 10 / LIMIT 0 / plain LIMIT / LIMIT over a
two-filter scan, top-N, semi (IN), anti (NOT EXISTS), MARK (OR IN), left / right / full outer, a filter whose target sits
under a lower join, UNION ALL on the probe side, a CTE used twice, bushy join of two joins, residual inequality, aggregate
on the build side, correlated subqueries (delim), window on the probe side, two-column key, left-join chain with NULL keys,
string key.

- **0 hangs, rc=0 in every arm; results identical to the tip in every arm and to DuckDB CPU for all 37.**
- 13 of 15 interrupts landed mid-query in every eager arm; all 16 follow-up queries correct, process closes cleanly.
- Activations in the f1 arm: 129 (17 without a filter plan, 112 with), 0 held; gate 1 arm: 121 activated, 8 held (fall back
  to ordinary activation, results equal).
- One shape (2 statements) fell back to DuckDB ("arbitrary join conditions"), in the tip as well: not exercised on the GPU.

Risks found by reading the code, none triggered, none disproved:

1. The activation runs inside the PARTITION pipeline's status lock (`add_finalize_listener` contract: "must not block") and
   calls `task_creator::schedule` twice; the listener captures `&join` and the raw `build_concat`. Author's 6.8 / 8.3 says
   so; S6 criterion 4 forbids it. A lock-order problem would need a schedule path that takes a pipeline status lock; not seen.
2. No re-evaluation: a held join (gate, admission) or a join whose session is not Terminal at the finalize returns silently
   and relies on ordinary activation. Safe, but any gain is lost for good; with gate 2 it never happened.
3. Throttle (not proposed for the design): `build_move_begin` is paired with `build_move_end` only through the build
   CONCAT's finalize listener. With `build_concat == nullptr`, or a query that is cancelled before that CONCAT finalizes,
   `pending_build_moves` stays above 0 for the rest of the process: every later scan of every query is limited to K per
   GPU. It is process-global and blocks executor threads in a condition variable that knows nothing about cancellation.
   The interrupt runs of the fk1 arm did not show a slowdown afterwards (0.30 - 0.57 s, as before), so the leak was not
   demonstrated; the pairing is still wrong by inspection. Slots are RAII, so it cannot deadlock by itself.
4. Not exercised: memory pressure / OOM retry of a starved CONCAT task, failure inside a publication, two queries at once,
   compressed pinned scans on these shapes (TPC-H covers pinned scans only on its own shapes), unit tests on prototype builds.

## 5. Proposed design (6.8) against the S6 acceptance criteria (notes/ISSUE_1825_S6.md)

| S6 criterion | prototype | design 6.8 | finding |
|---|---|---|---|
| 1 keep `normal_build_deposit_ready` as alternative | yes (held joins and the `adm` arm fall back; my gate 1 / shape runs agree) | yes | met |
| 2 layout readiness from build-PARTITION completion; terminal rights of all relevant endpoint producers, incl. unrelated, empty / skipped / failed / plan-dropped | PARTITION finalize + channel `terminal()` of every started scan (gate 2) | same, literal Relevant-Producers rule as fallback | The design replaces what the author calls the literal rule (gate 1, the join's own publish channels) by gate 2. Gate 1 can never open for stacked probes (confirmed: 4 joins held in every iteration), so the criterion text needs an explicit update or a clarification that "relevant endpoints" = the endpoints the activation starts; that is a change to the agreed plan, not an implementation detail. Terminal for failed / plan-dropped outcomes is a `D_ASSERT` today and was not exercised (only empty, declined, one-row: Terminal in all) |
| 3 subscribe-and-recheck, re-evaluate on layout completion AND last-producer terminal AND normal deposits; coalesce through task requests | one shot at PARTITION finalize, no subscription, no re-evaluation | "add the level-triggered re-evaluation" | **not built, not measured**. Never needed on TPC-H, so the number does not depend on it, but both notification orders are untested |
| 4 notify after releasing locks; weak targets invalidated before teardown | violates: runs under the pipeline status lock with raw references | "fired outside the pipeline status mutex with weak targets" | **not built**. Moving the nomination later can only delay it; cost expected small (CONCAT starts 0.1 ms after publication today) but unmeasured |
| 5 preserve progress for absent sources, unsupported shapes, probe-subtree producers; no new blocking wait or cycle | met on TPC-H and 37 shapes (0 hangs); the throttle adds a blocking wait but is excluded from the design | throttle excluded | met for the design; keep the throttle out, as the author says |
| 6 admit buffered probe output through existing reservation / backpressure; "a terminal no-filter result does not itself justify early work" | placeholder that always passes | "a real reservation-based rule", joins without a filter plan included | **not built, cannot be validated on this config.** About 190 of the 500 ms (q9, q18, q14, q22) come from joins without a filter, the one place where memory grows (q18 +2.1..3.5 GiB): exactly the case the criterion makes conditional on admission. The unconditional part is switch 1 (-324 ms, mixed sessions) |
| 7 validation list (both orders, unrelated / probe-subtree producers, zero-filter terminals, narrowing, cancellation / teardown, missed wake-up, pressure) | none; my shape run covers zero-filter terminals, probe-subtree producers and cancellation empirically, nothing else | listed as tests to write | open |
| 8 docs, observability, internal ablation control, no public knob | `[eager]` line, env switches | same | met in intent |

Other points on 6.8:

- "Results identical, no scan before its filters": holds (sections 2, 4).
- "Throttle not in S6": agreed; K=1 is real (-80 ms on three queries) but belongs to the transfer problem.
- "#2570 owes the contract finalize => Terminal for every layout and outcome": agreed, and it is a precondition of the whole
  design; today nothing but a debug assert states it.
- The -497..-512 ms should be quoted as "with admission always granted, on a machine with 27 GiB of headroom per GPU at the
  worst query"; combined with the shadow rule / transfer tracks it is not additive (other track).

## 6. Not verified

K=2 / K=3, option 2, batched staging modes (6.4), the 6.5 contention mechanism figures (raw data lost), switch 1 on the full
suite in one session, f1 on the 11 queries not in t1, gate 1 cost at q8 / q21, memory pressure, results against DuckDB CPU
for TPC-H (campaign-wide open item; done here for the 37 shapes only), unit tests.

## 7. Files

- Scripts: `runs/veager/bin/{one_query,unit,plan,lp,shapes,shapes_tr}.sh`, `{times,mem,cmp_logs,cmp_shapes_tr,shapes,shapes_cmp}.py`;
  plans `runs/veager/plans/{t1,t2,t3,lp}.plan`.
- Raw: `runs/veager/{t1,t2,t3,lp}_<arm><n>/` (csv, logs, results), `runs/veager/shapes/<arm>[_tr]/`,
  `telemetry/veager_<label>/`. Progress logs `runs/veager/logs/`.
- Tables: `runs/veager/out/{t1_table,t2_table,t3_table,recalc_r7,cmp_logs_f1,cmp_logs_g1,cmp_shapes_tr_f1,cmp_shapes_tr_s2g0,shapes_table,mem_t1,mem_r7}.md`.

## CHECKPOINT LOG (chronological; newest last)


- 2026-11-27 23:20 UTC. Scripts written: `bin/one_query.sh` (copy of the eager one, output under runs/veager), `bin/unit.sh`
  (arms tip / off / o1 / g1 / f1 / fk1), `bin/plan.sh`, `bin/times.py` (timing table), `bin/mem.py` (peak GPU memory from
  Quent), `bin/cmp_logs.py` (TRACE log comparison tip vs arm), `bin/shapes.py` + `bin/shapes.sh` (non-TPC-H join shapes,
  watchdog, interrupt). Plans: `plans/t1.plan` (ABBA tip / f1 [/ fk1] on q18 q9 q21 q14 q5 q3 q8 + controls q4 q19 q10),
  `plans/lp.plan` (TRACE pass of f1 on the 18 activated queries, g1 on q2 q7 q8 q21), `plans/t2.plan` (binary control:
  `wt-eager2` with eager off vs tip on q9 q18 q14 q3 q21). Progress: `logs/{t1,lp,t2}_plan.out`, `logs/shapes_chain.out`.
- 2026-11-27 23:50 UTC. Recomputed the author's r7 / fs2 sums from raw `runtimes.csv` with my own script: identical
  (-497.2, -596.7, -511.5) -> `out/recalc_r7.md`. Memory table from the author's r7 Quent: `out/mem_r7.md`.
  t1 so far: q18 f1 -87.7, q9 f1 -87.1 / fk1 -116.1 (reproduce). lp: q3 q8 q9 q2 q7 q21 f1 vs tip log pass: same filter keys,
  same rows out of every scan / DYNAMIC_FILTER operator.
- 2026-11-28 00:19 UTC. Interim (tables regenerate with `python3 bin/times.py t1`, `python3 bin/times.py t2 tip off`,
  `python3 bin/shapes_cmp.py tip f1 fk1`): t1 f1 vs tip q3 -46.6, q5 -48.8, q9 -87.1, q14 -52.1, q18 -87.7, q21 -52.5
  (sum -375, author -366 on the same six); fk1 q3 -65.4, q9 -116.1. Binary control t2 (`wt-eager2`, eager off): q9 -9.8,
  q18 +3.4 (noise). Shapes (37 shapes x 2 + 15 interrupts): tip / f1 / fk1 rc=0, results f1 = fk1 = tip on all, 0 HANG,
  13 interrupts each then 16/16 correct follow-up queries. Still queued: t1 q8 q4 q19 q10; lp q14 q15 q22 + g1 pass;
  t2 q14 q3 q21; t3 (g1 vs o1 on q2 q7); shapes s2g1, s2g0, cpu.
- 2026-11-28 01:10 UTC. All queued runs done (t1 44, t2 20, t3 12, lp 22 processes; shapes tip / f1 / fk1 / s2g1 / s2g0 / cpu
  + TRACE tip / f1 / s2g0; all rc=0; the first cpu attempt failed on a script error and was rerun). Tables written to
  `runs/veager/out/`. Document finalized (sections 0-7). Nothing pending for this verification.
