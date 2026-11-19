# VERIFY_PROTO_OP2: adversarial verification of PROTO_OP2 (dead filters / shadow rule)

2026-11-19. Verifier did not modify the prototype (`$R/wt-op2`, branch `proto/op2-dead-filters`, tip `523ae08c`); nothing built.
Scripts `$R/runs/vop2/bin/` (one_query.sh, unit.sh, dbg.sh, ut.sh, p2p.sh, p2p_off.sh, next.sh, vtable.py, logdiff.py, rowdiff.py,
fire.py, qsum.py). Raw output `$R/runs/vop2/` (`done.txt` = execution log), Quent `$R/telemetry/vop2_*` (45 label dirs),
Quent summaries `$R/runs/vop2/quent/*.json`. Every GPU process ran under `flock $R/machine.lock`; one hold = one query's whole
ABBA unit (4-6 fresh processes) or one debug pass. Config untouched (SQL SET / env only). 121 query processes + 6 unit-test runs, all rc=0. Lock queue was 60-110 waiters; all
verifier units ran back to back 01:40-02:37.

## 0. Verdict

| claim | verdict | corrected number |
|---|---|---|
| Shadow rule gains 338 ms (2.2% of suite) on this machine | **confirmed** | -335.6 ms = 2.18% (q21 -300.5, q8 -35.1) |
| q21 -303.5 ms (one AB pass) | **confirmed**, now full ABBA x3 | -300.5 ms (-20.9%), blocks -297.6 / -302.8 / -303.5, n = 24 + 24 |
| q8 -35.0 ms | **confirmed** | -35.1 ms (-3.75%), blocks -37.4 / -34.1 / -34.3, n = 24 + 24 |
| Bounce-excluded ~154 ms (1.0%) | **weakened to a range** (model; top of range) | ~83-152 ms (0.5-1.0%): q21 75-136, q8 8-16 |
| Results identical, no useful filter lost | **confirmed** for the default consumer config | see 2; config-dependent (decode cap), see 4 |
| Latch fix q9 -20 ms "unresolved" | **refuted as a gain** | +11.6 ms (+0.8%), blocks +10.8 / +10.1, n = 16 + 16 |
| q9 oracle skip suppkey -41 ms | **weakened** | -16.7 ms (-1.2%), blocks -16.5 / -20.8 (not buildable anyway) |
| Controls inside noise | **confirmed** | q2 -0.3, q3 +1.1, q5 -0.5, q7 +1.0, q16 -2.9, q20 +1.3; q10 -11.0 (see 1) |
| "No filter a consumer applies at tip is removed" as a property of the rule | **weakened** | true on TPC-H at the default decode cap only (4) |

Recommendation: shadow skip is real and worth landing in a hardened, order-independent form (section 5). Latch fix: drop.

## 1. Timing (independent re-run)

Protocol: fresh process per query, 6 iterations (1 warm + 5 hot, first hot dropped -> 4 samples per process), tip = `$R/repo`,
prototype = `$R/wt-op2` + env switches, ABBA inside one lock hold (so A and B are 25-60 s apart, not 10-20 min as in PROTO_OP2).
`sh` = SHADOW=1; `shl` = SHADOW=1 LATCH=1 (prototype's final config); `la` = LATCH=1; `sk9` = oracle SKIP=29:1; `off` = prototype
binary, no switch; `tip2` = tip binary + `SET fused_scan_max_membership_sources = 2` (decode carries 2 join probes).
Table = `vtable.py` (pooled medians; per-block delta = median(variant) - median(tip) inside one block).

| q | variant | tip med (n) [min-max] | variant med (n) [min-max] | delta ms | delta % | per-block deltas | results |
|---|---|---|---|---|---|---|---|
| 21 | sh | 1438.8 (24) [1426-1456] | 1138.3 (24) [1121-1154] | **-300.5** | -20.89 | -297.6 / -302.8 / -303.5 | identical |
| 8 | sh | 934.6 (24) [926-942] | 899.5 (24) [896-904] | **-35.1** | -3.75 | -37.4 / -34.1 / -34.3 | identical |
| 8 | off | 936.9 (8) [928-940] | 935.8 (8) [928-939] | -1.1 | -0.11 | -1.1 | identical |
| 9 | la | 1415.6 (16) [1393-1442] | 1427.3 (16) [1399-1439] | **+11.6** | +0.82 | +10.8 / +10.1 | identical |
| 9 | sk9 | 1415.6 (16) [1393-1442] | 1399.0 (16) [1378-1435] | -16.7 | -1.18 | -16.5 / -20.8 | identical |
| 2 | shl | 148.1 (8) [147-152] | 147.7 (8) [146-149] | -0.3 | -0.23 | | identical |
| 3 | shl | 660.6 (8) [658-676] | 661.7 (8) [653-673] | +1.1 | +0.17 | | identical |
| 5 | shl | 694.8 (8) [678-714] | 694.3 (8) [679-716] | -0.5 | -0.07 | | identical |
| 7 | shl | 743.8 (8) [739-747] | 744.8 (8) [740-749] | +1.0 | +0.14 | | identical |
| 10 | shl | 858.7 (16) [844-869] | 847.7 (16) [841-867] | -11.0 | -1.28 | -9.4 / -4.4 | identical |
| 16 | shl | 314.7 (8) [304-318] | 311.8 (8) [302-315] | -2.9 | -0.92 | | identical |
| 20 | shl | 714.6 (8) [699-734] | 715.9 (8) [698-734] | +1.3 | +0.18 | | identical |
| 21 | tip2 | 1439.7 (8) [1432-1456] | 1436.4 (8) [1417-1458] | -3.3 | -0.23 | | identical |
| 8 | tip2 | 934.4 (8) [929-942] | 943.1 (8) [940-947] | +8.7 | +0.93 | | identical |
| 9 | tip2 | 1418.4 (8) [1400-1438] | 1421.5 (8) [1409-1441] | +3.1 | +0.22 | | identical |

- Per-process medians: q21 tip 1436.5-1443.4 (6 runs), sh 1135.5-1143.0 (6); q8 tip 933.7-938.6 (8), sh 897.9-901.3 (6). No overlap,
  spread 7 ms.
- The skip fired in every iteration of every `sh` timing run, cold one included: q21 36 of 36, q8 36 of 36 (`fire.py` on INFO logs:
  constructions 3 instead of 4, resp. 4 instead of 5).
- `off` = tip (-1.1 ms): the prototype binary itself costs nothing.
- q10 -11.0 ms (-1.3%): inside the +-3% / +-10 ms noise, same sign in both blocks, no mechanism (no decision differs on q10: same
  key lines, no skip, `latch=true` x7). Treated as noise; not counted.
- q9 latch fix: both of my blocks are positive (+10.8, +10.1); with the prototype's two passes (-32, -0.6) the four blocks straddle
  zero. No gain. (Trace: the scan output drops 6.0 B -> 1.8 B rows as intended, but wall time does not follow.)
- **Suite**, shadow rule alone: q21 + q8 = **-335.6 ms = 2.18% of 15372**. Final config SHADOW+LATCH: -335.6 + 11.6 (q9) = -324 ms.
  The suite total was not re-timed as 22 queries: 10 queries carry the measurement (2, 3, 5, 7, 8, 9, 10, 16, 20, 21).
  **Not timed**: q4, q11, q12, q14, q15, q17, q18, q19, q22 (debug pass with SHADOW+LATCH shows every filter outcome identical to
  tip, section 2) and q1, q6, q13 (no dynamic filter).

## 2. Correctness

- **Results**: `result.txt` md5 equal to baseline tip in 120 of 121 runs with a result (tip, sh, shl, la, sk9, off, tip2, debug
  and trace passes of all 22 queries with SHADOW+LATCH). The one difference is q11: same 936,989 rows and values, tie order differs;
  the two baseline tip runs of q11 differ from each other the same way (not a prototype effect).
- **Filters built / published** (`logdiff.py`, hot window, tip TRACE pass `baseline/logpass` vs prototype DEBUG pass with
  SHADOW=1 LATCH=1, all 22 queries): `[dynamic_filter_key]` lines identical for 20 queries. Only differences: q8 j41 and q21 j18
  `kind=bloom pushed=1` -> `kind=none pushed=0 reason=not_constructed`. 0 error / fallback lines, `outcome=ok` everywhere.
- **Filters probed**: independent tip-behaviour gate logs (prototype binary, switches off, `dbg_off`): q8 j41 Bloom
  `skip negligible_removed_share` on 50 of 50 endpoint batches and never decode-probed; q21 j18 31 of 31, never decode-probed
  (q2 j28: 4 of 4). TRACE row totals per operator, tip vs SHADOW+LATCH (`rowdiff.py`): identical on every operator for q8 (59
  operators), q21 (50), q2, q20, q16. q9 differs only at GPU_SCAN(25) (the latch moves filtering into the decode; DYNAMIC_FILTER
  output equal).
  - Side effect seen in the logs: the decode "join filters kept" lines drop from 50 to 16 lineitem chunks on q8 (31 -> 16 on q21).
    Cause: at tip the never-measured Bloom keeps decode measurement on for every chunk (`result.measure`); it is a logging /
    counting difference, row totals are equal.
- **Quent** (`quent_df.py`, all timed runs): 0 consumer scan tasks start before the publication of a filter they receive, in 180
  tip, 60 sh, 80 shl, 20 la, 20 sk9, 30 tip2 hot query records (1700 join-consumer pairs).
  q21 j18 site: Publishing Task 317.1 -> 12.0 ms, site end 368 -> 63 ms, first lineitem scan 442 -> 139 ms.
  q8 j41 site: 35.6 -> 0.0 ms, site end 197 -> 161, first scan 297 -> 262. The gain is the removed task time, 1:1.
- **Unit tests** on the prototype build (`ut.sh`): `[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]` 638 cases,
  `[publisher],[construction_plan],[physical_partition],[build_filter],[in_task_publication]~[multi_gpu]` 131 cases,
  `[dynamic_filter][multi_gpu]` 31 cases: all pass with switches off **and with SHADOW=1 LATCH=1**. Not shown: that any test
  reaches `decision=skip` (the rule needs a reliable estimate on both filters; no test was written for it).
- **Edge paths (code reading, not run)**:
  - A skipped single-key join ends through the existing `planned.keys.empty()` -> `end_now(SKIPPED)` path; no lease, stream or
    accumulator is taken (the decision precedes `try_create`). This is the only path TPC-H exercises (q8 j41, q21 j18 are
    single-key). **A multi-key join with one skipped key (partial skip) was never executed.**
  - Estimates are set before fan-out, and the rule reads a channel snapshot: no race found. A throw inside the rule is caught by
    `begin`'s handler and drops every key of that join (performance only).
  - Cross-rank: a rank that skips hands a null share, which makes the other ranks drop the key (existing protocol): performance
    only, but the local `inventory.total_rows()` makes the own estimate too small per rank, so ranks decide differently from a
    single process. Not run.

## 3. Bounce split

Measured here: the whole gain is the Publishing Task of the removed construction (2). Its parts at tip:

| | q21 j18 (1394 MiB, hash path) | q8 j41 (174 MiB, forward path) |
|---|---|---|
| Publishing Task removed (Quent, mine) | 305.1 ms | 35.6 ms |
| non-bounce: insert + fixed, net of the no-DF site (PROFILE 3.4, VERIFY_transfer #4) | ~44 ms | ~3.4 ms |
| finish chain (merge + replicate), HOST-staged here | ~261-267 ms | ~32 ms |
| finish on a PEER path, 1 source + 1 target (MB8 microbench) | 30.8 ms | 4.1 ms |
| P2P-equivalent finish, 3 sources + 3 targets (1x if legs overlap .. 3x if serial as today) | 31-92 ms | 4-12 ms |
| **this machine** | **-300.5 ms (20.9%)** | **-35.1 ms (3.75%)** |
| **bounce excluded / P2P-equivalent (model)** | **~75-136 ms (5-9.5%)** | **~8-16 ms (0.8-1.7%)** |

In-engine check on the one PEER pair (GPUs 0,1; tip behaviour = OP1A binary with `SIRIUS_DF_ASYNC_PUBLISH=0`,
`SIRIUS_PROTO_NUM_GPUS=2`; label `g2_sync2`, 5 hot iterations, 0 HOST-staged publications; debug log level):
- q21 j18: site tasks 62 / 62 / 92 / 92 / 123 ms, publication 123 ms after the final contribution, site ends 170 ms into the query
  (4-GPU skip run: site tasks 6 ms, last 12 ms). Roughly 100 ms of removable task time with one source and one target.
- q8 j41: site tasks 5.2 / 9.6 ms: about 9.6 ms removable.
- It is a site measurement, not an A/B (wt-op2 has no GPU-count override), and 2 GPUs double the per-GPU insert load.
Suite, bounce excluded: **~83-152 ms (0.5-1.0%)**; the prototype's 154 ms assumes fully serial legs. Bounce share of the measured
gain: ~185-250 ms of 335.6.

## 4. Would the gain survive on a full-P2P machine

- Yes in sign, smaller in size: the insert share (44 + 3.4 ms) is transport-independent, and the PEER-pair run shows the finish
  still sits in the Publishing Task there. Expected 0.5-1.0% of the suite instead of 2.2%.
- The filters stay worthless when they are applied: with the decode cap raised to 2 the tip does probe them in the decode
  (q21 Bloom keeps 0.485 of the IN-list survivors, q8 0.062, `dbg_off2`), and wall time is q21 -3.3 ms, q8 **+8.7 ms**. The
  prototype's own endpoint-apply test gave q21 -21 ms, q8 +0.4 ms. So build cost (136 ms / 16 ms P2P-equivalent) exceeds benefit
  (0-21 ms) on both transports.
- "After OP1" figures of PROTO_OP2 are moot: PROTO_OP1A measured that OP1 hides nothing on the PEER path, and OP1 is demoted.
- With eager probe scheduling (S6: a scan waits for all its filters) the dead construction would stay on the critical path, so
  the skip keeps its full value. Not measured.

## 5. Issues a production implementation must solve

1. **Order dependence carries 89% of the gain.** The rule sees only filters already published. q21: the stronger j21 IN-list is
   published 31 ms (hot) / 61 ms (cold) before j18's `begin`; q8: 144 ms. It fired 72 of 72 times here, but a different machine,
   GPU count or S6 scheduling can flip the order and silently lose the 300 ms. q2 j28 (begins 58 ms before j25 publishes) is
   missed for that reason; q20 j49 is built only because it begins 57 ms before the stronger Bloom publishes.
   Needs a decision that does not depend on arrival order (plan-time estimates of sibling producers, or a deferred decision).
2. **The rule mirrors only one of two consumer policies.** It copies the endpoint's negligible-share formula; the decode picks by
   kind rank then key count (`static_selectivity_of`), and by `fused_scan_max_membership_sources`. At cap 2 the consumer does
   apply both skipped Blooms. With a Bloom already on the channel and a later, weaker-by-estimate hash IN-list, the rule would
   remove the filter the decode prefers (q20 shape; avoided on TPC-H by order and a second binding only).
3. **One definition shared with the consumer** (`k_negligible_removed_share`, formula, cap). The prototype also deviates: reliable
   estimates only, no availability / carrier check, own estimate from all rows while the published estimate uses valid rows.
4. **Wrong static estimate of the stronger filter.** If the consumer measures the stronger filter weak and drops it, the weaker one
   would have been applied at tip; with the skip it does not exist. Unique build key does not guarantee the probe-side keep ratio
   (skew, correlation).
5. Own reason token and counter instead of `not_constructed`; the skip must count as a terminal producer outcome for S6
   ("plan-dropped"), including the partial-skip case.
6. Tests: skip, no-skip, partial skip of a multi-key join, both orders of publication, cross-rank disagreement.
7. Placement per pr_split: decision + reason token in `construction_plan.cpp` (#2569); channel facts supplied by the session
   (#2570); `estimate_reliable` on the filter and all consumer code are pre-existing files outside the stack.

## 6. What ran / what is unverified

- Ran: 3 ABBA blocks q21, 3 blocks q8 (+ off control), 2 blocks q9 (la, sk9), 2 blocks q10, 1 block each q2, q3, q5, q7, q16, q20,
  decode-cap arms on q8 / q9 / q21, debug pass of all 22 queries with SHADOW+LATCH, debug pass switches-off (q2, q8, q20, q21) and
  cap 2 (q8, q9, q21), trace pass (q2, q8, q9, q16, q20, q21), DF unit tests off / on, PEER-pair site runs (q8, q21).
- Unverified: timing of the 12 queries listed in 1; any multi-key partial skip; distributed / cross-rank; a real A/B on a P2P
  path (model + site measurement only); behaviour under S6; ENDPOINT and OP3 switches (prototype says drop; not re-run).
