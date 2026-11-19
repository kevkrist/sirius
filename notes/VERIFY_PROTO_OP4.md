# VERIFY_PROTO_OP4: adversarial check of PROTO_OP4 (OP4 filter kind, OP5 8 bits/key)

2026-11-18. Verifier only: prototype not modified (`$R/wt-op4`, `proto/op4-filter-kind` @ `843cece7`, clean tree,
`libsirius.so` contains both `SIRIUS_PROTO_BLOOM_*` strings, tip library contains none).
A = tip binary `$R/repo` (`ae939824`). B = `$R/wt-op4` + env. Config `/home/ubuntu/sirius/specific_config.yaml` untouched.
Scripts / raw: `$R/runs/vop4/` (`bin/one_query.sh abba.sh logpass.sh unit.sh table.py qsplit.py lpdiff.py`, `*_table.md`,
`unit_*.out`); Quent sessions moved to `$R/telemetry/vop4/<label>/q<N>/`.

## 0. Verdict

| claim | verdict | corrected numbers |
|---|---|---|
| OP4: no wall gain, drop | **confirmed, one correction** | 8 affected queries sum -14 ms (suite ~-0.1%); only q11 resolves (-2.7 ms, -1.6%). The small gain is real but comes from the build site (exact-set publish 0.6-3.3 ms -> Bloom 0.1-0.4 ms per join), not from the probe kernels |
| OP5 this machine: q21 -143, q3 -27.6, q7 -22 | **confirmed** | q21 -151.7 (-10.5%), q3 -27.1 (-4.1%), q7 -25.2 (-3.4%), 16 hot per side |
| OP5 suite ~-280 ms (-1.8%), extrapolated | **confirmed in total, wrong per query** | 8 of 12 Bloom queries measured: -261 ms. q4 gains 0 (not -10): false positives cancel it. Est. suite -265 to -275 ms (-1.7 to -1.8%) |
| OP5 gain is finish-chain time only | **confirmed** | build-site change is 94-110% of the wall change on q21, q3, q7, q8; on q5, q9, q10, q4 the difference is the false-positive cost or noise outside the site (section 2.2) |
| OP5 outside the build site: +2 to +12 ms loss | **weakened** | q21 is -8 (not +12; noise), q3 +2.0, q7 +2.5, q8 +1.6, q9 +3.9; real losses only q10 +8.0 and q4 +11.1 (ranges do not overlap) |
| OP5 "bounce excluded: no gain, drop for speed" | **weakened** | finish chain is not pure bounce. With the campaign's own split (VERIFY_methodology #7, P2P pays ~37% of a 3-source HOST finish): P2P-equivalent gain ~-90 ms on the 8 queries (~0.6% suite), q21 ~-61 ms (4%). Model, not measurable here. It vanishes only if OP1 lands |
| Rates low by 2x (cub 2 items per thread) | **confirmed (source)** | `tuning_for.cuh:62` `ForPolicy{256, 2}` |
| Correctness | **confirmed**, unit-test claim **weakened** | results identical; with the overrides set the full DF tag has 9 (8 bpk) / 1 (kind range) failures, all assertions of the tip's sizes or kinds (section 4.4) |

Recommendation: OP4 drop. OP5 drop if OP1 lands; otherwise "needs more" (size-thresholded 8 bpk for q21 j18 only is the
untested variant that keeps most of the gain and avoids the q4 / q10 loss).

## 1. What ran

- Every GPU process under `flock $R/machine.lock`. One lock hold = one query's block of 4 fresh processes (A B B A or
  B A A B), so A and B of a block are 20-60 s apart (prototype: 10-45 min apart).
- Each process: 1 warm + 5 hot, first hot dropped -> 4 hot values. Medians pooled per side.
- Kind (B env `SIRIUS_PROTO_BLOOM_RANGE=100000:3000000`): q17, q8, q11, q21, control q18 (set of 63K keys, out of
  range): 2 blocks (ABBA 22:02-22:10, BAAB 22:44-22:51) = 16 hot per side. q2, q7, q19, q20 (affected, never timed by
  the prototype) and control q6 (no DF): 1 block = 8 hot per side.
- 8 bpk (B env `SIRIUS_PROTO_BLOOM_BPK=8`): q21, q3, q7: 2 blocks = 16 hot per side. q8, q5, q9, q10, q4 and controls
  q17 (no Bloom), q6: 1 block.
- Binary control: tip vs prototype binary with no env, q3, 1 block.
- Log pass (trace, 2 iterations, untimed): tip / kind / 8 bpk on q17, q8, q11, q21, q3, q7.
- Unit tests, prototype build, 4 GPUs visible, `[dynamic_filter]~[cuda_status_fault]~[.]` (669 cases, includes
  `[multi_gpu]`): no env, 8 bpk, kind range 100000:3000000, kind range 1:3000000.
- 134 query processes + 4 unit runs, all rc=0 (unit rc=42 where tests failed). No fallback / error lines.
- Not run: full 22-query suite, nsys, microbench, q12 / q16 / q20 / q22 at 8 bpk.

## 2. Timing (ms, this machine)

### 2.1 Kind policy (OP4)

| q | tip | proto | delta | n hot A/B | tip range | proto range | build-site union (Quent) | outside build site |
|---|---|---|---|---|---|---|---|---|
| 17 | 497.2 | 495.4 | -1.8 (-0.4%) | 16/16 | 488-508 | 487-507 | 0.7 -> 0.3 | -1.0 |
| 8 | 935.3 | 933.6 | -1.7 (-0.2%) | 16/16 | 930-941 | 926-939 | 42.7 -> 40.9 | -0.7 |
| 11 | 172.3 | 169.5 | -2.7 (-1.6%) | 16/16 | 167-183 | 164-178 | 2.2 -> 0.8 | -1.2 |
| 21 | 1444.0 | 1440.0 | -4.0 (-0.3%) | 16/16 | 1429-1467 | 1417-1471 | 344.4 -> 344.2 (j21 0.9 -> 0.2) | -3.2 |
| 2 | 148.5 | 147.9 | -0.6 (-0.4%) | 8/8 | 147-152 | 147-150 | 11.3 -> 6.0 | **+4.7** (126-127 vs 130-133) |
| 7 | 743.3 | 745.1 | +1.9 (+0.3%) | 8/8 | 740-751 | 741-749 | 69.2 -> 67.9 | +3.4 |
| 19 | 879.5 | 879.5 | 0.0 | 8/8 | 876-889 | 876-891 | 1.2 -> 0.4 | +0.6 |
| 20 | 717.2 | 711.8 | -5.4 (-0.8%) | 8/8 | 700-735 | 692-731 | 3.7 -> 1.7 | -3.4 |
| 18 (control) | 1089.0 | 1091.1 | +2.1 (+0.2%) | 16/16 | 1073-1110 | 1081-1115 | 2.1 -> 2.1 | +2.1 |
| 6 (control) | 380.3 | 379.7 | -0.7 (-0.2%) | 8/8 | 378-382 | 379-382 | - | - |

- Affected 8 queries: -14.3 ms in total; controls +2.1 / -0.7. Only q11 separates from the control band: B below A in
  7 of 8 process pairs (A 176 172 172 172, B 169 169 169 173).
- Mechanism the prototype did not report: every hash IN-list site publishes faster as a Bloom (Publishing Task, Quent
  medians): q2 j28 3.3 -> 0.4, q2 j25 1.6 -> 0.4, q8 j44 2.3 -> 0.5, q7 j28 1.5 -> 0.2, q20 j31 1.4 -> 0.2, q19 j9
  1.2 -> 0.3, q11 j14 / j35 0.9 / 0.8 -> 0.2, q21 j21 0.9 -> 0.2, q17 j7 0.6 -> 0.3. Sum ~-13 ms = the whole measured
  gain. This is the exact-set replicate cost already listed under OP1b; it is on the critical path.
- False positives cost wall time where the filter was selective: q2 outside the build site +4.7 ms, ranges disjoint,
  which cancels q2's -5.3 ms build gain.
- Bounce: none in the tip; the Bloom replica goes through HOST staging but is 0.4-2.7 MB (0.1-0.4 ms). Same figure for
  "this machine" and "bounce excluded".

### 2.2 8 bits per key (OP5)

| q | tip | 8 bpk | delta | n hot A/B | Publishing Task per Bloom site (Quent) | build-site union | outside build site (ranges A / B) |
|---|---|---|---|---|---|---|---|
| 21 | 1443.2 | 1291.4 | -151.7 (-10.5%) | 16/16 | j18 317.1 -> 187.3; j33 26.1 -> 12.8 | 344.5 -> 201.4 (-143.1) | -8.1 (1068-1097 / 1062-1091) |
| 3 | 660.0 | 632.9 | -27.1 (-4.1%) | 16/16 | j15 57.1 -> 29.6; j8 4.2 -> 2.5 | 61.4 -> 32.2 (-29.2) | +2.0 (586-601 / 584-679) |
| 7 | 745.8 | 720.6 | -25.2 (-3.4%) | 16/16 | j35 63.2 -> 36.2; j38 3.8 -> 3.1 | 69.1 -> 41.4 (-27.7) | +2.5 (661-672 / 661-674) |
| 8 | 935.9 | 917.2 | -18.6 (-2.0%) | 8/8 | j41 35.6 -> 17.4; j34 4.1 -> 2.5 | 42.8 -> 22.8 (-20.0) | +1.6 |
| 5 | 690.2 | 675.3 | -14.9 (-2.2%) | 8/8 | j32 16.7 -> 8.7; j25 4.2 -> 2.5 | 21.5 -> 11.9 (-9.7) | -5.2 (645-683 / 637-678, noisy) |
| 9 | 1411.9 | 1399.0 | -12.9 (-0.9%) | 8/8 | j29 31.8 -> 16.6; j19 3.6 -> 2.1 | 35.5 -> 18.7 (-16.8) | +3.9 |
| 10 | 860.6 | 849.7 | -10.9 (-1.3%) | 8/8 | j10 22.5 -> 11.7; j21 17.4 -> 9.6 | 40.1 -> 21.5 (-18.6) | **+8.0** (794-818 / 804-828) |
| 4 | 325.2 | 325.4 | **+0.2** | 8/8 | j9 21.2 -> 10.5 | 21.6 -> 10.8 (-10.7) | **+11.1** (293-299 / 305-309) |
| 17 (control) | 497.4 | 497.4 | 0.0 | 8/8 | - | 0.7 -> 0.7 | +0.1 |
| 6 (control) | 379.8 | 379.7 | -0.1 | 8/8 | - | - | - |
| 3, binary only (no env) | 662.3 | 659.1 | -3.1 (-0.5%) | 8/8 | - | - | - |

- 8 Bloom queries: -261.1 ms. Not measured: q12, q16, q20, q22 (finish 11.5 / 8.5 / 0.9 / 7.1 ms at 16 bpk): at most
  ~-14 ms more, less if they behave like q4. Suite on this machine: about -265 to -275 ms (-1.7 to -1.8% of 15.37 s).
- The prototype's per-query rule "half the finish time is gained" fails for q4 (0 gained) and q10 (60% gained).
- Linearity check: q21 j18 317.1 -> 187.3 = 56 ms pre-finish + half of 261 ms. The reduction is the finish chain; the
  insert share did not move.

## 3. Bounce vs non-bounce, and a machine with full P2P

Split used: VERIFY_methodology #7 (MB4 rates): HOST finish 185.7 ms/GB with 3 contributing sources, of which 68 ms/GB
(36.6%) is what 3 P2P targets would still cost; single-batch sites (0 sources: q3 j8, q5 j25, q8 j34, q7 j38) cost
60.9 ms/GB HOST vs 60.75 P2P, so they carry no bounce excess; 2 sources ~46%. Applied to the measured Publishing Task
reductions. **Model, not measurable on this machine.**

| q | wall delta (this machine) | of it bounce excess | bounce excluded (P2P-equivalent) |
|---|---|---|---|
| 21 | -151.7 | -90.7 | **-61** (4.2% of the tip here) |
| 3 | -27.1 | -17.4 | -9.7 |
| 7 | -25.2 | -17.1 | -8.1 |
| 8 | -18.6 | -11.5 | -7.1 |
| 5 | -14.9 | -5.1 | -9.8 (structural -4.6; the rest is noise outside the site) |
| 9 | -12.9 | -10.4 | -2.5 |
| 10 | -10.9 | -11.7 | +0.8 |
| 4 | +0.2 | -6.8 | **+7.0** (loss) |
| sum | -261 | -171 | **-90** (~0.6% of suite) |

- Strict reading (no finish time counted at all, the prototype's headline): sum of "outside build site" = +16 ms,
  carried by q4 and q10. Agrees with the prototype in sign, not per query.
- OP4 has no bounce component: its ~-14 ms survives unchanged on a P2P machine.
- OP5 on a P2P machine: about one third survives (model), concentrated in q21. If OP1 lands first, the finish sits in
  slack (q21 j18 slack 76 ms vs P2P-equivalent finish 95 -> 47 ms at 8 bpk: <= 19 ms left to gain; all other sites 0)
  while the false-positive losses (q4 +11, q10 +8) stay: net about zero or negative.

## 4. Correctness

### 4.1 Results
- Byte-identical `result.txt` tip vs prototype on every process: kind q2, q6, q7, q8, q17, q18, q19, q20, q21; 8 bpk
  q3, q4, q5, q6, q7, q8, q9, q10, q17, q21.
- q11 (kind): 8 different md5 over 8 processes, 4 of them among tip processes alone; sorted content identical on all 8.
  Tie order, not a prototype effect.

### 4.2 Filters built / published / probed (trace log diff, `bin/lpdiff.py`)
- `construction started` counts equal per process on every timed query (table column constr A/B).
- Same joins, same keys, same `pushed=1`, same filters -> targets, same "N of M probe(s) ran" buckets and chunk counts
  in all 6 queries, both overrides. No lost, declined or newly gated filter.
- Only differences: the intended kind / size, HOST staging on the converted keys, and scan survivors:

| override | q | scan output rows (tip -> proto) |
|---|---|---|
| kind | 17 | 6.01M -> 13.83M (2.3x) |
| kind | 8 | 39.96M -> 47.77M (+19.5%) |
| kind | 21 | 239.9M -> 247.5M (+3.2%) |
| kind | 11 | 32.00M -> 32.99M per scan (+3.1%) |
| kind | 7 | 145.9M -> 148.1M (+1.5%) |
| 8 bpk | 3 | lineitem 34.1M -> 135.5M (**4.0x**); orders 147.8M -> 166.6M |
| 8 bpk | 21 | 352.5M -> 532.8M (+51%) |
| 8 bpk | 7 | 10.3M -> 16.6M (+61%) |
| 8 bpk | 8 | 91.6M -> 103.2M (+13%) |

### 4.3 Consumer scan before its filter is visible (Quent, `bin/qsplit.py`, all hot iterations)
- 0 scan tasks started before publication on both sides in every query: kind q17 0/248, q8 0/944, q11 0/192, q21
  0/752, q18 0/560, q2 0/80, q7 0/904, q19 0/648, q20 0/552; 8 bpk q21 0/1504, q3 0/832, q7 0/1808, q8 0/944, q5 0/424,
  q9 0/528, q10 0/416, q4 0/200. Gap publication -> first scan unchanged within 4 ms except where the publication
  itself moved.

### 4.4 Unit tests (prototype build, `[dynamic_filter]~[cuda_status_fault]~[.]`, 4 GPUs)
| env | cases | failed | what fails |
|---|---|---|---|
| none | 669 | 0 | 146197 assertions passed |
| `BPK=8` | 669 | 9 (45 assertions) | literal 16-bpk sizes: chunk geometry `chunks_per_key() == 3` (32x), `raw_bytes == 2'000'032`, `storage_bytes() == 64 KiB`, scratch-request counts matched by byte size, a 1 MiB reservation case built around a 2,000,128-byte chunk, and "Bloom over the cap is refused" (at 8 bpk it fits, so it is built) |
| `RANGE=100000:3000000` | 669 | 1 | `test_build_filter_construction.cpp:571` asserts `exact_membership` |
| `RANGE=1:3000000` | 669 | 15 (43 assertions) | kind assertions (`planned_kind`, `hashed != nullptr`, `kind=` log text, exact scratch bytes, exact ticket slices) |

- All failures are assertions of the tip's sizes or kinds; membership, parity-with-CPU, lifetime and cancellation
  cases pass under both overrides. Triage by reading the failing assertions; `test_in_task_publication.cpp:841/895`,
  `test_complete_build_inventory.cpp:586/594`, `test_dynamic_filter_accumulation.cpp:226` were not debugged further.
- The prototype reported 21 cases at 8 bpk and none with the range set. The full tag is not green under either override.

### 4.5 Edge paths
- The prototype adds no new lifetime, lease or stream code; it reroutes existing kinds. No race or late-filter path
  is introduced (4.3).
- Kind override + per-join Bloom cap (`construction_plan.cpp` cap block): converted keys join the Bloom byte sum, and a
  cap refusal drops **every** Bloom key of the join. A small key that the tip keeps as an exact set would be lost with
  an oversized sibling key. Not reachable on TPC-H SF1000 (cap 16 GB per GPU).
- 8 bpk changes which builds pass the cap (unit case above): more filters get built near the cap.
- Both overrides are read once per process in a static; `bloom_blocks_for` is no longer `constexpr`.

## 5. Issues a production implementation must solve

OP4 (if ever revived):
1. Cap coupling above: a converted key must not be refused with its siblings (fall back to the exact set).
2. False positives on selective filters (q17 2.3x, q8 +20%, q2 +4.7 ms wall): needs a selectivity guard.
3. Policy belongs in config (`dynamic_filter_inlist_max_l2_fraction` already exists), not an env static.
4. The only wall gain is the exact-set publish cost; fixing that directly (OP1b) keeps exactness.

OP5:
1. 25x false positives: q3 lineitem scan output 4.0x, q21 +51%. Wall cost is small here except q4 / q10, but row and
   byte volume into PARTITION / join grows; memory pressure and spill at tighter budgets are untested.
2. Net loss where the build has no slack and the probe side is large (q4 +7 ms bounce excluded).
3. Per-size policy (8 bpk only for very large arrays) and hash-count retuning are untested.
4. 9 unit cases hard-code 16-bpk sizes; cap behaviour changes.
5. Value depends on OP1: decide after OP1, on the full suite.

## 6. Unverified by me

- nsys-based statements (probe kernel ms, copy-bound scan windows, insert kernel ms), the microbench tables and the
  false-positive rates: not re-run. Only the 2-items-per-thread fact was checked in the cub source.
- The P2P split is the campaign model (MB4 rates), not a measurement.
- q12, q16, q20, q22 at 8 bpk; the 10 non-Bloom queries; the full suite.
- Single-block queries (8 hot per side) resolve about +-3 ms; q5 and q9 tip ranges are wide (38 / 62 ms).
- Second blocks ran 40-45 min after the first; medians agree within 3 ms between blocks.
