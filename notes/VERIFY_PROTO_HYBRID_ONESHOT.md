# VERIFY_PROTO_HYBRID_ONESHOT: independent check of PROTO_HYBRID_ONESHOT.md

2026-11-27/28. Verifier worktree `$R/wt-vhybrid`, branch `verify/hybrid-oneshot` = `proto/hybrid-oneshot` 9414a46d + 1 commit
(9b0fb9e4, +10 lines, local only). Scripts `$R/runs/vhybrid/bin/`, raw `$R/runs/vhybrid/{v1,r2}_<arm>_{a,b}/`, Quent
`$R/telemetry/vhybrid_*`. All GPU processes + the build under `flock $R/machine.lock`. Config untouched. `wt-hybrid` untouched.

## 0. Verdict per claim

| # | author's claim | verdict | corrected number |
|---|---|---|---|
| 1 | no eager: one-shot (H) beats tip by -159 ms suite | **confirmed** | 9 queries -161 (author -134), 6 others -18; 15 queries -179. q9 is noisy (-40 here, -17 author) |
| 2 | eager: E -240, EH -157, "one-shot forfeits 84 ms" | measurement **confirmed** (E -270, EH -188..-200, EH-E +71..+82), interpretation **REFUTED** | the forfeit is an artifact of the prototype's release point. Fair hybrid+eager (arm X): **-268..-270 = E (X-E = -1 +-~30 ms)** |
| 3 | the other 13 queries have no material one-shot site | **confirmed** (6 of 13 run: q7 10 11 18 19 20) | EH-E and X-E within noise on all six (sum X-E -3 ms) |
| 4 | 38 of 46 filter joins are one-shot class | **confirmed** | 38 distinct deferring joins counted from H logs (22 queries) |
| 5 | ~650 src + ~450 test lines, second transfer path | **confirmed** | prototype +671 lines (9 files); #2570 deletes ~445 test lines in the two named files |
| - | overall verdict "keep PARTITION-only" | **stands, but on different grounds** | not "one-shot loses 84 ms under S6" (it ties) but: no gain once S6 lands, loses once the transfer PR lands, costs a second path |

## 1. The fairness defect in the author's EH arm

In the prototype, with both switches on, a one-shot site gets **no eager release at all**: the eager listener sees a
non-terminal session at PARTITION finalize and gives up; the probe then starts by the default rule = when the LAST of the 4
broadcast slots' build CONCAT has deposited. But the filter is published by the FIRST slot to arrive. Slack between
"filter published" and "probe released", from the author's own EH logs (`bin/slack.py`, median of hot iterations):

| site | filter published | probe released (last deposit) | avoidable delay |
|---|---|---|---|
| q5 j25 | 44-46 ms | 78-80 | **34** |
| q2 j28 | 32 | 56-57 | **24-25** |
| q8 j34 | 38 | 60 | **22** |
| q16 j11 | 112-118 | 129-132 | **10-16** |
| q3 j8, q5 j32 | | | 2-8 |
| q3 j15, q8 j41, q9 j29, q12 j8, q21 j33, q22 j28 | | | 0 (claimant is the last to finish) |

The author's site table (3.3) reports "ready H/EH = build CONCAT end", which hides this (e.g. q5 j25 listed 74, really 44).
These are exactly the queries carrying the claimed forfeit (q5 +29, q2 +23, q16 +13, q8 +13).

Fix tested (arm **X**): `SIRIUS_VERIFY_EAGER_ONESHOT=1` calls the existing `activate_probe_after_build_partition()` right after
`observe_whole_build()` returns with a terminal session (publish, deposit, release). Scans still start with their filter.
Results identical to T in every process (q11 tie order only, as before).

## 2. Timing (fresh process per query, 1 warm + 6 hot, first hot dropped, ABBA halves in separate lock holds)

Arms: T, H, E, EH = author's binary + author's switches; X = `wt-vhybrid`, eager + one-shot + release after publication.
Block v1: 15 queries x 5 arms, 10 hot/arm. Block r2 (replicate, T E EH X): q3 5 8 9 10 21, +10 hot/arm.
Tables: `runs/vhybrid/out_v1_table.md`, `out_r2_table.md`, `out_v1r2_table.md` (`bin/vtable.py`; bootstrap 95% CI).
All 198 processes rc=0, 0 error/fallback lines.

Delta vs T, ms (v1+r2 pooled; H from v1 only):

| q | T | H | E | EH | X | EH-E [95% CI] | **X-E [95% CI]** |
|---|---|---|---|---|---|---|---|
| 2 | 148.4 | -4.0 | -25.6 | -2.8 | -25.6 | +22.7 [+19, +24] | 0.0 [-3, +2] |
| 3 | 661.6 | -34.9 | -44.5 | -29.7 | -40.2 | +14.8 [+5, +21] | +4.4 [-4, +14] |
| 5 | 701.2 | -15.9 | -52.7 | -15.9 | -37.4 | +36.8 [+17, +43] | **+15.4 [+5, +22]** |
| 8 | 937.4 | -31.1 | -42.2 | -31.7 | -44.6 | +10.5 [+2, +15] | -2.4 [-12, +1] |
| 9 | 1419.8 | -39.7 | -13.4 | -33.3 | -30.0 | -19.9 [-26, -10] | **-16.6 [-27, -5]** |
| 12 | 701.5 | -5.0 | -9.1 | -6.6 | -7.0 | +2.5 [+1, +4] | +2.1 [+1, +5] |
| 16 | 312.2 | -8.3 | -21.3 | -6.1 | -18.2 | +15.3 [+10, +17] | +3.1 [+1, +4] |
| 21 | 1439.7 | -17.1 | -57.6 | -56.4 | -60.1 | +1.2 [-8, +10] | -2.5 [-11, +7] |
| 22 | 338.9 | -5.1 | -3.3 | -5.0 | -5.1 | -1.6 | -1.7 |
| **9 queries** | 6661 | **-161** | **-270** | **-188** | **-268** | **+82** (v1 alone +71; author +84) | **+1** (v1 alone +1; r2 subset +3) |
| 7 | 749.1 | -3.0 | -24.6 | -25.2 | -25.2 | -0.6 | -0.6 |
| 10 | 858.4 | -5.6 | -11.4 | -20.8 | -10.2 | -9.4 (no one-shot site: noise, T spread 10) | +1.2 |
| 11 | 175.1 | +0.5 | -0.1 | -4.5 | -3.8 | -4.5 | -3.8 |
| 18 | 1092.0 | -2.6 | -5.4 | -6.2 | -7.2 | -0.8 | -1.8 |
| 19 | 881.7 | -2.8 | -1.2 | -1.5 | -1.4 | -0.3 | -0.1 |
| 20 | 719.3 | -4.2 | -0.8 | +2.0 | +0.7 | +2.8 | +1.6 |
| **6 others** | 4476 | -18 | -44 | -56 | -47 | -13 | **-3** |

Noise: the author's 84 ms (their EH vs E) IS outside noise in sum and per query on q2, q5, q16, q3 (reproduced 3 times:
+84, +71, +82), not on q8, q21, q22; on q9 the sign is opposite (one-shot better, reproduced). But it measures the late
release, not one-shot. With the release fixed, only q5 (+15, E better) and q9 (-17, hybrid better) differ outside noise, and
they cancel; the 9-query sum X-E is +1 ms with a sum-of-bounds range of about +-50 ms.

## 3. Switch evidence (per join)

Distinct joins per process log: `defers` / `[eager] probe activated` / `[eager] NOT terminal` (v1 half a):
T 0/0/0 everywhere; H n/0/0; E 0/n/0; EH n/0/n (plus the multi-partition sites activated: q21 j18, q7 j35+j38, q10 x2);
X n/n/n. n = q2 4, q3 2, q5 4, q8 5, q9 2, q12 1, q16 1, q21 3, q22 1, q7 3, q11 4, q18 2, q19 1, q20 4, q10 0.

First scan start, ms from query start (Quent, median of hot; full `runs/vhybrid/out_sites_v1_full.md`, `.json`):

| site | PARTITION end T | T | H | E | EH | X |
|---|---|---|---|---|---|---|
| q3 j8 | 11 | 24 | 23 | 12 | 22 | 15 |
| q3 j15 | 180 | 294 | 259 | 172 | 261 | 256 |
| q5 j25 | 26 | 75 | 74 | 26 | 73 | **30** |
| q5 j32 | 206 | 265 | 248 | 186 | 248 | 227 |
| q8 j34 | 33 | 62 | 59 | 32 | 59 | **37** |
| q8 j41 | 197 | 298 | 268 | 182 | 267 | 254 |
| q9 j29 | 439 | 491 | 462 | 426 | 461 | 462 |
| q12 j8 | 625 | 656 | 649 | 625 | 649 | 650 |
| q16 j11 | 64 | 136 | 128 | 64 | 130 | **113** |
| q21 j18 (multi-partition) | 356 | 436 | 433 | 355 | 357 | 356 |
| q21 j33 | 918 | 944 | 927 | 871 | 888 | 887 |

E starts at PARTITION end, EH at the last CONCAT deposit, X right after the one-shot publication: the switches did what
they say. At the big sites (q3 j15, q8 j41, q9 j29) X cannot start earlier than EH (the filter needs the CONCAT), yet X ties
E on wall time: E's earlier scan (-43) and one-shot's saved finish F (-35..-40) are worth about the same today.

## 4. Would the transfer PR flip it? No, it strengthens PARTITION-only (reasoning + other agent's data; not measured here)

- One-shot's only merit is avoiding the tip's finish F (merge ingress, mostly host bounce) before the CONCAT. The transfer PR
  cuts F at the PARTITION site. `runs/combined/out/m1.md` (other agent, transfer-only arm X there): q3 -38, q8 -26, q5 -15,
  q9 -15 = the same size as one-shot's H gain (q3 -35, q8 -31, q5 -16) without one-shot.
- Transfer + eager on the tip path is additive (same table, XE): q3 -81, q5 -61, q8 -60, q9 -41, q16 -28, q12 -16.
  Fair hybrid + eager here: q3 -40, q5 -37, q8 -45, q9 -30, q16 -18, q12 -7. On the 8 queries without q21: about -318 vs
  -210, i.e. **~100 ms in favour of PARTITION-only** (cross-experiment, different binaries; upper bound, because a transfer PR
  that also covered one-shot's replicate would give some back, but a one-shot filter can never be ready before CONCAT end).
- So: today without S6 one-shot is +160 ms better; with S6 it is a tie; with S6 + transfer PR it is ~100 ms worse.

## 5. What to correct in PROTO_HYBRID_ONESHOT.md

- Section 0 row "With eager scans": replace "the hybrid forfeits 84 ms" by "the prototype's EH arm loses 84 ms because it
  releases one-shot probes at the last CONCAT deposit; with the release right after publication the hybrid ties (+1 ms)".
- Section 3.3: "ready H/EH" is CONCAT end, not the publication time (q5 j25 44 not 74; q8 j34 38 not 59; q2 j28 32).
- Section 4 row (b) "84 ms worse than (a)" -> "equal with S6; ~100 ms worse with S6 + transfer PR (estimate)".
- The decision now rests on (i) the transfer PR landing and (ii) the cost of a second path. If the transfer PR were dropped
  and S6 delayed, one-shot (with release after publication) would be a real -160 ms interim win at no eager cost.

Not run: remaining 7 queries (q1 4 6 13 14 15 17: no filter or controls), hybrid + transfer in one binary, 2-GPU / P2P box.
