# COMBINED: shadow rule + xfer-d2d + eager probe in one binary, measured against the tip

2026-11-27. Worktree `/opt/dlami/nvme/dfperf/wt-combined`, branch `proto/combined`. Scripts `$R/runs/combined/bin/`,
raw runs `$R/runs/combined/<tag>_<arm>_{a,b}/`, Quent `$R/telemetry/combined_<label>/q<N>/`, tables `$R/runs/combined/out/`.
Every build and GPU process under `flock $R/machine.lock`. Config file untouched; env switches only.
STATUS: see the checkpoint log at the end (authoritative for what is finished).

## 1. Recipe (exact)

```bash
R=/opt/dlami/nvme/dfperf
git -C $R/repo worktree add -b proto/combined $R/wt-combined prod/shadow-2570      # cd89bbc8
cd $R/wt-combined
git cherry-pick 09a04c32 c15b053e a6b8c1da b7d0eb81    # proto/xfer-d2d (microbench commit 09a04c32 included: no conflict)
git cherry-pick f05cfd12                               # proto/eager-probe
# -> ede040e3 4a205119 e3e5d0f7 bc7cf1ff e013f8d0 ; ZERO conflicts in all five picks
# + c5aa5bdf proto(combined): SIRIUS_PROTO_SHADOW_OFF=1 (measurement switch, 7 lines in judge_shadow(),
#   src/op/dynamic_filter/construction_plan.cpp: no key is judged shadowed -> tip plan). Not for any PR.
git submodule update --init --recursive
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache TMPDIR=$R/tmp
flock $R/machine.lock pixi run make > $R/logs/build_combined.log 2>&1              # $R/tmp/build_combined.sh
```

Switches in the one binary (all unset = shadow rule only, since the shadow rule is production code without a switch):

| piece | on | off |
|---|---|---|
| S shadow rule | default | `SIRIUS_PROTO_SHADOW_OFF=1` (added here) |
| X xfer-d2d | `SIRIUS_PROTO_XFER_D2D=pipe` | unset |
| E eager probe (option 1, joins with an enabled filter plan) | `SIRIUS_EAGER_PROBE=1` | unset |

Arms: `T` = tip binary `$R/repo` (pr2570 ae939824); `O` = combined binary, all three off; `S X E SX SE XE SXE` = combined
binary with those pieces on. `bin/arm.sh <arm> <label> <q> <iters>`; `bin/abba.sh <tag> <iters> "<arms>" <q...>` runs per
query a forward pass over the arms then a reverse pass (lock held per pass), each process 1 warm + 5 hot, first hot
dropped -> 4 kept per process, 8 hot per arm.


## 2. Gates

- Unit (`$R/runs/combined/unit/summary.txt`, `bin/unit.sh`): `[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]` on 1 GPU:
  661 cases / 143845 assertions passed. `[dynamic_filter][multi_gpu]` (4 GPUs): 33 cases passed with switches unset,
  with X=pipe, and with X=pipe + E=1. `[xfer_relay],[mbid]` with X=pipe: passed (16679 assertions). With
  `SIRIUS_PROTO_SHADOW_OFF=1` exactly the 9 shadow-rule cases fail (they assert the rule the switch disables), 652 pass.
- Results: md5 of `result.txt` of every process against arm T pass a. 17 DF queries x 8 arms x 2 processes (272) + non-DF
  5 x 3 x 2 (30) + o1 (8): identical everywhere except q11, where all 16 processes (tip included, Ta vs Tb too) have a
  different md5 but one sorted-content md5 (730f7529, 936989 rows) = tie order only. All 310 processes rc=0.
- Switch evidence (DEBUG log pass `lp_*`): `[eager] join` lines T 0 / S 0 / X 0 / SXE 93; shadow `decision=skip` lines
  T 0 / X 0 / S 6 / SXE 6 (2 keys per pass: q21 j18, q8 j41; x 3 iterations); Bloom route in X and SXE = `island peer
  copies + HOST pipelined exchange`.
- Binary neutrality (arm O = combined binary, all three off, vs tip binary; `out/o1.md`, `out/n1.md`): q21 -1.2 ms,
  q3 -7.3 ms (tip arm of that block 667.5 vs 660.3 in m1: tip-side wobble), non-DF q1 -1.4, q6 +0.3, q14 +1.9, q15 -1.1.

## 3. Timing, 17 DF queries, all 8 arms (block `m1`)

Per query: forward pass T S X E SX SE XE SXE, then the reverse pass; one fresh process per arm per pass, 1 warm + 5 hot,
first hot dropped; n = 8 hot per arm; pooled medians. Lock held per pass (other agents' work interleaved between passes).
Regenerate: `python3 $R/runs/combined/bin/table.py m1` -> `$R/runs/combined/out/m1.md`.

### Pooled medians, ms (n hot per arm in brackets when != 8)

| q | T | S | X | E | SX | SE | XE | SXE | results |
|---|---|---|---|---|---|---|---|---|---|
| 2 | 147.5 | 150.5 | 146.2 | 123.2 | 146.7 | 122.9 | 122.4 | 122.1 | identical |
| 3 | 660.3 | 662.9 | 622.6 | 615.6 | 626.4 | 616.8 | 579.3 | 574.8 | identical |
| 4 | 324.7 | 326.2 | 310.4 | 326.2 | 310.6 | 324.1 | 311.0 | 310.5 | identical |
| 5 | 693.9 | 692.6 | 679.3 | 646.4 | 678.9 | 649.2 | 632.4 | 635.6 | identical |
| 7 | 747.6 | 748.3 | 707.5 | 724.6 | 709.3 | 724.3 | 685.0 | 685.9 | identical |
| 8 | 936.6 | 900.7 | 911.0 | 894.4 | 898.5 | 866.0 | 876.8 | 859.6 | identical |
| 9 | 1420.2 | 1411.4 | 1404.8 | 1406.1 | 1389.6 | 1396.8 | 1379.8 | 1378.0 | identical |
| 10 | 851.4 | 844.2 | 826.2 | 851.6 | 822.6 | 847.7 | 812.1 | 822.9 | identical |
| 11 | 171.7 | 172.8 | 175.0 | 170.5 | 176.0 | 172.7 | 174.5 | 171.5 | DIFF: Ea,Eb,SEa,SEb,SXEa,SXEb,SXa,SXb,Sa,Sb,Tb,XEa,XEb,Xa,Xb |
| 12 | 700.8 | 700.7 | 693.0 | 693.3 | 694.0 | 692.9 | 685.2 | 684.7 | identical |
| 16 | 312.9 | 305.2 | 308.1 | 291.2 | 301.9 | 291.2 | 285.1 | 284.4 | identical |
| 17 | 497.0 | 496.8 | 497.2 | 497.0 | 497.2 | 497.1 | 496.5 | 496.7 | identical |
| 18 | 1088.5 | 1081.6 | 1084.4 | 1087.3 | 1081.6 | 1082.3 | 1091.7 | 1086.9 | identical |
| 19 | 879.2 | 881.0 | 879.9 | 879.2 | 880.3 | 879.9 | 880.2 | 880.4 | identical |
| 20 | 716.6 | 715.6 | 713.9 | 714.5 | 714.1 | 713.3 | 711.7 | 713.2 | identical |
| 21 | 1441.9 | 1137.7 | 1238.3 | 1384.0 | 1119.0 | 1076.8 | 1177.4 | 1061.3 | identical |
| 22 | 338.5 | 338.7 | 333.6 | 335.9 | 334.1 | 335.1 | 331.2 | 330.9 | identical |
| **sum** | **11929** | **11567** | **11531** | **11641** | **11381** | **11289** | **11232** | **11099** | |

### Delta vs T, ms (spread = |half a - half b| of the arm's process medians, T spread in column T)

| q | T a-b | S | X | E | SX | SE | XE | SXE |
|---|---|---|---|---|---|---|---|---|
| 2 | 0.6 | +3.0 | -1.3 | -24.3 | -0.7 | -24.5 | -25.1 | -25.3 |
| 3 | 3.0 | +2.5 | -37.8 | -44.7 | -33.9 | -43.5 | -81.0 | -85.5 |
| 4 | 1.5 | +1.4 | -14.3 | +1.5 | -14.1 | -0.6 | -13.7 | -14.2 |
| 5 | 1.6 | -1.3 | -14.6 | -47.5 | -15.0 | -44.7 | -61.4 | -58.3 |
| 7 | 3.5 | +0.7 | -40.1 | -23.0 | -38.4 | -23.4 | -62.6 | -61.7 |
| 8 | 2.3 | -35.9 | -25.6 | -42.1 | -38.1 | -70.5 | -59.8 | -77.0 |
| 9 | 2.3 | -8.8 | -15.4 | -14.2 | -30.6 | -23.4 | -40.5 | -42.2 |
| 10 | 14.0 | -7.2 | -25.2 | +0.2 | -28.8 | -3.7 | -39.3 | -28.5 |
| 11 | 0.3 | +1.1 | +3.3 | -1.3 | +4.3 | +0.9 | +2.7 | -0.3 |
| 12 | 0.1 | -0.1 | -7.8 | -7.5 | -6.7 | -7.9 | -15.5 | -16.1 |
| 16 | 0.5 | -7.7 | -4.8 | -21.7 | -11.0 | -21.7 | -27.8 | -28.5 |
| 17 | 0.0 | -0.2 | +0.2 | +0.0 | +0.2 | +0.1 | -0.4 | -0.2 |
| 18 | 1.5 | -6.9 | -4.1 | -1.3 | -6.9 | -6.3 | +3.1 | -1.7 |
| 19 | 0.6 | +1.9 | +0.7 | -0.0 | +1.1 | +0.8 | +1.0 | +1.2 |
| 20 | 1.1 | -1.0 | -2.7 | -2.1 | -2.4 | -3.3 | -4.9 | -3.4 |
| 21 | 4.5 | -304.2 | -203.6 | -57.9 | -322.9 | -365.1 | -264.5 | -380.6 |
| 22 | 0.5 | +0.2 | -4.9 | -2.6 | -4.4 | -3.4 | -7.3 | -7.6 |
| **sum** | | **-363** | **-398** | **-288** | **-549** | **-640** | **-697** | **-830** |

### Overlap, ms: pair/all measured vs sum of the single gains (positive overlap = gain lost to overlap)

| q | S | X | E | S+X sum | SX meas | overlap | S+E sum | SE meas | overlap | X+E sum | XE meas | overlap | S+X+E sum | SXE meas | overlap |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | +3.0 | -1.3 | -24.3 | +1.7 | -0.7 | -2.5 | -21.3 | -24.5 | -3.3 | -25.5 | -25.1 | +0.5 | -22.6 | -25.3 | -2.8 |
| 3 | +2.5 | -37.8 | -44.7 | -35.3 | -33.9 | +1.3 | -42.2 | -43.5 | -1.3 | -82.5 | -81.0 | +1.5 | -80.0 | -85.5 | -5.5 |
| 4 | +1.4 | -14.3 | +1.5 | -12.9 | -14.1 | -1.2 | +2.9 | -0.6 | -3.5 | -12.8 | -13.7 | -0.9 | -11.4 | -14.2 | -2.8 |
| 5 | -1.3 | -14.6 | -47.5 | -15.9 | -15.0 | +0.8 | -48.8 | -44.7 | +4.1 | -62.0 | -61.4 | +0.6 | -63.3 | -58.3 | +5.0 |
| 7 | +0.7 | -40.1 | -23.0 | -39.4 | -38.4 | +1.1 | -22.3 | -23.4 | -1.0 | -63.1 | -62.6 | +0.5 | -62.4 | -61.7 | +0.7 |
| 8 | -35.9 | -25.6 | -42.1 | -61.5 | -38.1 | +23.4 | -78.0 | -70.5 | +7.5 | -67.7 | -59.8 | +7.9 | -103.6 | -77.0 | +26.6 |
| 9 | -8.8 | -15.4 | -14.2 | -24.2 | -30.6 | -6.4 | -23.0 | -23.4 | -0.4 | -29.6 | -40.5 | -10.9 | -38.4 | -42.2 | -3.8 |
| 10 | -7.2 | -25.2 | +0.2 | -32.4 | -28.8 | +3.6 | -7.0 | -3.7 | +3.3 | -25.0 | -39.3 | -14.3 | -32.2 | -28.5 | +3.7 |
| 11 | +1.1 | +3.3 | -1.3 | +4.3 | +4.3 | -0.1 | -0.2 | +0.9 | +1.1 | +2.0 | +2.7 | +0.7 | +3.1 | -0.3 | -3.3 |
| 12 | -0.1 | -7.8 | -7.5 | -7.9 | -6.7 | +1.2 | -7.6 | -7.9 | -0.3 | -15.3 | -15.5 | -0.3 | -15.4 | -16.1 | -0.7 |
| 16 | -7.7 | -4.8 | -21.7 | -12.4 | -11.0 | +1.4 | -29.4 | -21.7 | +7.7 | -26.5 | -27.8 | -1.3 | -34.2 | -28.5 | +5.7 |
| 17 | -0.2 | +0.2 | +0.0 | -0.0 | +0.2 | +0.2 | -0.1 | +0.1 | +0.2 | +0.2 | -0.4 | -0.7 | +0.0 | -0.2 | -0.3 |
| 18 | -6.9 | -4.1 | -1.3 | -11.1 | -6.9 | +4.2 | -8.2 | -6.3 | +1.9 | -5.4 | +3.1 | +8.5 | -12.3 | -1.7 | +10.7 |
| 19 | +1.9 | +0.7 | -0.0 | +2.5 | +1.1 | -1.4 | +1.8 | +0.8 | -1.1 | +0.7 | +1.0 | +0.3 | +2.5 | +1.2 | -1.3 |
| 20 | -1.0 | -2.7 | -2.1 | -3.6 | -2.4 | +1.2 | -3.1 | -3.3 | -0.3 | -4.7 | -4.9 | -0.1 | -5.7 | -3.4 | +2.3 |
| 21 | -304.2 | -203.6 | -57.9 | -507.8 | -322.9 | +185.0 | -362.1 | -365.1 | -3.0 | -261.5 | -264.5 | -3.0 | -565.7 | -380.6 | +185.1 |
| 22 | +0.2 | -4.9 | -2.6 | -4.6 | -4.4 | +0.2 | -2.4 | -3.4 | -1.0 | -7.5 | -7.3 | +0.2 | -7.3 | -7.6 | -0.4 |
| sum | -362.6 | -398.0 | -288.4 | -760.6 | -548.6 | +212.0 | -650.9 | -640.2 | +10.8 | -686.3 | -696.9 | -10.6 | -1048.9 | -830.0 | +218.9 |

Reading the last table: "overlap" = measured gain of the combination minus the sum of the single gains (positive = part
of the single gains is the same time counted twice).

## 4. Non-DF queries (block `n1`: T, O, SXE) and all-off check (block `o1`)

### Pooled medians, ms (n hot per arm in brackets when != 8)

| q | T | O | SXE | results |
|---|---|---|---|---|
| 1 | 473.5 | 472.1 | 466.5 | identical |
| 6 | 380.0 | 380.3 | 381.0 | identical |
| 13 | 1314.2 | 1290.4 | 1288.7 | identical |
| 14 | 687.2 | 689.1 | 682.2 | identical |
| 15 | 528.6 | 527.4 | 526.9 | identical |
| **sum** | **3383** | **3359** | **3345** | |

### Delta vs T, ms (spread = |half a - half b| of the arm's process medians, T spread in column T)

| q | T a-b | O | SXE |
|---|---|---|---|
| 1 | 5.4 | -1.4 | -7.0 |
| 6 | 0.4 | +0.3 | +1.0 |
| 13 | 25.4 | -23.8 | -25.5 |
| 14 | 0.0 | +1.9 | -5.0 |
| 15 | 0.6 | -1.1 | -1.7 |
| **sum** | | **-24** | **-38** |

q13's -24/-26 ms is the tip arm, not a gain: its two tip processes differ by 25 ms (same wobble as in PROTO_EAGER, "one
tip process 1378") and O (everything off) shows the same delta. The 5 non-DF queries are counted as 0 below; with the
measured n1 figures the suite delta would be 38 ms larger.

### Pooled medians, ms (n hot per arm in brackets when != 8)

| q | T | O | results |
|---|---|---|---|
| 3 | 667.5 | 660.2 | identical |
| 21 | 1445.1 | 1444.0 | identical |
| **sum** | **2113** | **2104** | |

### Delta vs T, ms (spread = |half a - half b| of the arm's process medians, T spread in column T)

| q | T a-b | O |
|---|---|---|
| 3 | 1.6 | -7.3 |
| 21 | 5.3 | -1.2 |
| **sum** | | **-8** |


## 5. Suite totals

| | ms | vs tip (same session) | vs base ref 15225 | vs DF-off ref 16627 |
|---|---|---|---|---|
| tip, this session: 17 DF (m1) + 5 non-DF (n1) | 11929 + 3383 = 15312 | - | +87 (+0.6%) | -1315; DF = 1.086x |
| S alone | 14949 | -363 (-2.4%) | -276 | |
| X alone | 14914 | -398 (-2.6%) | -311 | |
| E alone | 15024 | -288 (-1.9%) | -201 | |
| S+X | 14763 | -549 (-3.6%) | -462 | |
| S+E | 14672 | -640 (-4.2%) | -553 | |
| X+E | 14615 | -697 (-4.6%) | -610 | |
| **S+X+E (combined), non-DF counted as tip** | **14482** | **-830 (-5.4%)** | **-743 (-4.9%)** | **-2145; DF = 1.148x** |
| S+X+E with the measured n1 arm | 11099 + 3345 = 14444 | -868 (-5.7%) | -781 (-5.1%) | -2183; DF = 1.151x |
| S+X+E on the reference tip (15372 - 830) | 14542 | | -683 (-4.5%) | -2085; DF = 1.143x |

- The base and DF-off references were taken on 11-18 with another protocol (median of 6 hot including the first hot) and
  before the NVMe wipe; this session's tip is 60 ms below the reference tip (15312 vs 15372). The last row removes that
  offset and is the fair figure against base / DF-off: **combined = base - 683 ms (-4.5%), DF-off - 2085 ms (1.143x)**.
  Direct subtraction gives -743 .. -781 ms against base.
- The stack's 147 ms regression against base (tip 15372 vs 15225) is recovered 4.6 to 5.6 times over.
- Singles reproduce the earlier per-track numbers measured in other sessions: S -363 (was -343), X -398 (was -370..-410),
  E -288 (was -295).

## 6. Overlap decomposition (17 DF queries, ms)

| | sum of single gains | measured | overlap (lost) | where |
|---|---|---|---|---|
| S + X | -761 | -549 | 212 | q21 185 (S removes j18's 1.39 GiB Bloom, whose finish is what X speeds up), q8 23 (j41, 174 MiB, same mechanism) |
| S + E | -651 | -640 | 11 | noise level (q8 7.5, q16 7.7, others +-4) |
| X + E | -686 | -697 | -11 | noise level; additive (q3 -81.0 vs -82.5, q7 -62.6 vs -63.1, q5 -61.4 vs -62.0) |
| S + X + E | -1049 | -830 | 219 | = the S-X overlap; nothing else |

- Marginal value of each piece on top of the other two (suite, DF queries): S adds 133 ms to X+E (-697 -> -830),
  X adds 190 ms to S+E (-640 -> -830), E adds 281 ms to S+X (-549 -> -830).
- So E is fully additive to both others (its gain is scheduling slack, not finish time). S and X compete only for the
  finish of filters that S proves dead: q21 j18 and q8 j41. Of X's 398 ms, 190 survive with S present; of S's 363 ms,
  133 survive with X present (X makes the dead filter's finish cheaper, but its inserts and scan-side cost stay).
- q21 alone: T 1441.9; S -304, X -204, E -58; SX -323, SE -365, XE -265, SXE -381 (1061.3 ms, 0.736x).
- Per query, combined vs tip: q21 -381, q3 -86, q8 -77, q7 -62, q5 -58, q9 -42, q10 -29, q16 -29, q2 -25, q12 -16,
  q4 -14, q22 -8; q11 q17 q18 q19 q20 within +-3.5 ms. No query slower in any arm beyond noise (worst +4.3 ms, q11 SX,
  171.7 ms query, inside the +-10 ms band).
- q10: E alone +0.2, XE -39.3 but SXE -28.5 and X -25.2 with a 14 ms tip a/b spread: read q10 as -25..-30 for anything
  containing X; no E effect.

## 7. Residual finish chain (DEBUG log pass `lp_*`, last of 3 iterations, one process per arm)

"finish ms" = sum over the query's constructions of the `constructed filters published ... N ms after the final
contribution` figure, i.e. the cross-GPU reduce + replicate inside the Publishing Task (BASELINE_SUITE "pub ms after
final"). Not Quent / nsys (not captured here; Quent NDJSON for every timing process is in `$R/telemetry/combined_*`).
DEBUG logging inflates slightly; the 12 queries with a Bloom. `python3 $R/runs/combined/bin/finish.py T S X SXE`.

| q | T: constr / Blooms / Bloom MiB / finish ms | S: constr / Blooms / Bloom MiB / finish ms | X: constr / Blooms / Bloom MiB / finish ms | SXE: constr / Blooms / Bloom MiB / finish ms | route(s) in last arm |
|---|---|---|---|---|---|
| 3 | 2 / 2 / 338 / 61.2 | 2 / 2 / 338 / 61.2 | 2 / 2 / 338 / 22.9 | 2 / 2 / 338 / 23.2 | island peer copies + HOST pipelined exchange |
| 4 | 1 / 1 / 109 / 21.0 | 1 / 1 / 109 / 20.9 | 1 / 1 / 109 / 6.9 | 1 / 1 / 109 / 7.0 | island peer copies + HOST pipelined exchange |
| 5 | 4 / 2 / 144 / 21.2 | 4 / 2 / 144 / 21.3 | 4 / 2 / 144 / 8.8 | 4 / 2 / 144 / 8.8 | island peer copies + HOST pipelined exchange |
| 7 | 5 / 2 / 301 / 68.9 | 5 / 2 / 301 / 69.7 | 5 / 2 / 301 / 31.9 | 5 / 2 / 301 / 31.9 | island peer copies + HOST pipelined exchange |
| 8 | 5 / 2 / 231 / 42.5 | 4 / 1 / 57 / 6.9 | 5 / 2 / 231 / 17.5 | 4 / 1 / 57 / 4.9 | island peer copies + HOST pipelined exchange |
| 9 | 2 / 2 / 187 / 35.4 | 2 / 2 / 187 / 35.3 | 2 / 2 / 187 / 11.0 | 2 / 2 / 187 / 11.3 | island peer copies + HOST pipelined exchange |
| 10 | 2 / 2 / 194 / 39.6 | 2 / 2 / 194 / 39.8 | 2 / 2 / 194 / 13.4 | 2 / 2 / 194 / 14.4 | island peer copies + HOST pipelined exchange |
| 12 | 1 / 1 / 59 / 11.2 | 1 / 1 / 59 / 11.2 | 1 / 1 / 59 / 3.4 | 1 / 1 / 59 / 3.4 | island peer copies + HOST pipelined exchange |
| 16 | 1 / 1 / 57 / 8.4 | 1 / 1 / 57 / 8.4 | 1 / 1 / 57 / 3.5 | 1 / 1 / 57 / 3.5 | island peer copies + HOST pipelined exchange |
| 20 | 4 / 1 / 4 / 3.4 | 4 / 1 / 4 / 3.4 | 4 / 1 / 4 / 2.7 | 4 / 1 / 4 / 2.7 | island peer copies + HOST pipelined exchange |
| 21 | 4 / 2 / 1527 / 344.3 | 3 / 1 / 133 / 27.2 | 4 / 2 / 1527 / 141.2 | 3 / 1 / 133 / 9.7 | island peer copies + HOST pipelined exchange |
| 22 | 1 / 1 / 36 / 7.2 | 1 / 1 / 36 / 7.2 | 1 / 1 / 36 / 2.2 | 1 / 1 / 36 / 2.2 | island peer copies + HOST pipelined exchange |
| **sum** | **32 / 19 / 3188 / 664.3** | **30 / 17 / 1620 / 312.5** | **32 / 19 / 3188 / 265.4** | **30 / 17 / 1620 / 123.0** | |

- Finish chain per suite pass: **tip 664 ms -> S 312 -> X 265 -> combined 123 ms** (-81%). Bloom bytes finished: 3188 ->
  1620 MiB (S drops q21 j18 1394 MiB and q8 j41 174 MiB; constructions 32 -> 30).
- The residual 123 ms is 78 ms/GiB over 1620 MiB (tip: 213 ms/GiB), largest items q7 32 ms (301 MiB), q3 23 ms (338 MiB),
  q10 14 ms, q9 11 ms, q21 10 ms. With E on, part of it no longer sits on the critical path (the probe side is already
  scheduled); how much of the 123 ms still reaches the wall was not measured (needs nsys / Quent per site).

## 8. Verdict

- The three pieces compose cleanly: 5 cherry-picks without a conflict, all gates green, results identical.
- Combined gain **-830 ms (-5.4%) on the suite against the tip in the same session** (-868 with the measured non-DF arm),
  79% of the sum of the three single gains (-1049); the missing 219 ms is entirely the S-X overlap on q21 j18 / q8 j41.
- Against base: -683 ms (-4.5%) on the reference scale; DF on vs off goes from 1.082x (tip) to 1.143x.
- Ledger per bucket, marginal on top of the others: shadow rule (#2569/#2570) 133..363 ms, transfer PR 190..398 ms,
  S6 eager 281..288 ms. Order-independent floor = 133 + 190 + 281 = 604 ms; the 219 ms overlap goes to whichever of
  S / X lands first (S if it lands in the stack PRs first: S 363, then X 190 +- noise, then E 281).

## 9. What ran, limits

- 310 timing processes (m1 272, n1 30, o1 8), 48 log-pass processes, 6 unit invocations. One ABBA block per query, no
  repeat block; n = 8 hot per arm. Noise band +-3% / +-10 ms per query; suite sums of 17 queries repeat within ~20-30 ms
  (VERIFY_PROTO_XFER_D2D). Differences between arms below ~30 ms on the suite (S+E vs X+E ordering, the +-11 ms
  "overlaps") are not resolved.
- Other agents' GPU work ran between passes (never during: machine lock), so passes a and b of a query can be 5-20 min
  apart; the T a-b column shows the drift (<= 5 ms except q10 14, q13 25).
- E = switch 1 only (joins with an enabled filter plan); switch 2 and the option-2 / throttle variants were not run here.
- Prototype caveats of each track stand (PROTO_XFER_D2D 10, VERIFY_PROTO_XFER_D2D 5-6, PROTO_EAGER 8); `proto/combined`
  is a measurement branch, not mergeable. The `SIRIUS_PROTO_SHADOW_OFF` commit must never reach a PR.
- Base and DF-off were not re-measured in this session (reference numbers of 11-18 used, see section 5).

## 0. Checkpoint log (chronological; append only)

- 11-27: worktree + branch created, 5 picks clean, switch commit c5aa5bdf, build started (`$R/logs/build_combined.log`).
- 11-27 18:35: build rc=0. Unit gates (`$R/runs/combined/unit/summary.txt`, `bin/unit.sh`): single-GPU DF 661 cases /
  143845 assertions passed; `[dynamic_filter][multi_gpu]` 33 cases passed with switches unset, with X=pipe, with X+E;
  `[xfer_relay],[mbid]` passed. With `SIRIUS_PROTO_SHADOW_OFF=1` the 9 shadow-rule cases fail (expected: the switch
  disables what they assert), the other 652 pass.
- 11-27 18:45: main block started: `bin/abba.sh m1 6 "T S X E SX SE XE SXE" 21 3 7 8 5 10 9 4 12 16 2 22 20 11 17 18 19`
  (progress `$R/runs/combined/m1.progress`, raw `$R/runs/combined/m1_<arm>_{a,b}/`). Then non-DF block `n1` (T, SXE).
- 11-27 19:26: m1 done for q21 q3 q7 q8 q5 (all rc=0, results identical in all 16 processes per query). Partial table
  `$R/runs/combined/out/m1_partial.md` (regenerate: `python3 $R/runs/combined/bin/table.py m1`). q21: T 1441.9, S 1137.7,
  X 1238.3, E 1384.0, SX 1119.0, SE 1076.8, XE 1177.4, SXE 1061.3 ms.
- 11-27 19:50: m1 done for q21 q3 q7 q8 q5 q10 q9. The harness kills a background command after 60 min: the block is
  relaunched in chunks of 4 queries (same tag `m1`, same script; `m1b.log`, then q22 q20 q11 17, then 18 19 + n1).
- 11-27 20:39: m1 done for 11 queries (21 3 7 8 5 10 9 4 12 16 2); chunk 3 (22 20 11) started, then 17 18 19, then n1.
- 11-27 21:20: m1 done for 14 queries (+22 20 11); chunk 4 (17 18 19) started.
- 11-27 22:00: m1 complete (17 DF queries x 8 arms x 2 processes = 272 processes, all rc=0). Table `/opt/dlami/nvme/dfperf/runs/combined/out/m1.md`. q11: 16 different md5 but one sorted-content md5 (730f7529, 936989 lines) = tie order only. n1 (1 6 13 14 15; T O SXE) + o1 (21 3; T O) started.
- 11-27 22:48: n1 + o1 complete (rc=0 all). Tables `/opt/dlami/nvme/dfperf/runs/combined/out/n1.md`, `/opt/dlami/nvme/dfperf/runs/combined/out/o1.md`. Log pass lp (T S X SXE; DEBUG, 3 iterations) started for the finish-chain residual.
- 11-27 23:25: log pass complete (48 processes rc=0), `out/finish.md`. Sections 2-9 written. ALL DONE. Branch `proto/combined` head c5aa5bdf (worktree `$R/wt-combined`, built).

<!-- VERIFICATION-BEGIN -->
## 10. Verification (independent verifier, 2026-11-27/28) — IN PROGRESS, interim checkpoint

Raw data `$R/runs/vcombined/<arm>_<a|b>/`, scripts `$R/runs/vcombined/bin/{one.sh,abba.sh,audit.py}`, progress
`$R/runs/vcombined/{status.txt,progress.txt}`. Regenerate every table: `python3 $R/runs/vcombined/bin/audit.py $R/runs/vcombined ""`
(author's blocks: `audit.py $R/runs/combined m1_` / `n1_` / `o1_`). `wt-combined` not modified.

- 00:10 interim (q21 q3 q8 done, q6 half): branch content, switch evidence, result identity, author's tables all CONFIRMED
  from raw data; own ABBA so far q21 SXE -375 (author -381), q8 -74 (-77), q3 -72 (-86), O vs T q3 +2.5. Remaining queued:
  q6 b, q7 q10 q9 q2 (T SXE), q13 (T O). Final text replaces this block.
<!-- VERIFICATION-END -->
