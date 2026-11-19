# Base reference: pre-stack DF behaviour (merge-base `c4fa3638`) vs tip (pr2570 `ae939824`), TPC-H SF1000, 4 GPUs

Run 2026-11-18. Base build: `$R/wt-base` (git worktree of `$R/repo` at `c4fa3638`, `pixi run make`, release, rc=0, 6 min with sccache).
Config `/home/ubuntu/sirius/specific_config.yaml` unmodified. Regime identical to `BASELINE_SUITE.md` section 0 (fresh process per
query, `performance_test.py`, host pin + pin compression, 4 GPUs). Every GPU process ran under `flock $R/machine.lock`.
Raw output `$R/runs/base/`, scripts `$R/runs/base/bin/`, Quent sessions moved to `$R/telemetry/base_<label>/q<N>/`
(`/home/ubuntu/sirius/results/telemetry` back to its 245 pre-existing dirs).

## 0. What was run

| label (`$R/runs/base/<label>`) | build | what | reps |
|---|---|---|---|
| `logpass` | base | TRACE log, all 22 queries | 2 iterations (iter0 warm, iter1 hot = parsed window) |
| `base_a`, `base_b` | base | timing, info log, all 22 | 2 fresh processes x (1 warm + 3 hot) = 6 hot samples/query |
| tip `baseline_tip`, `baseline_tip_b` (`$R/runs/baseline`, other agent) | tip | same recipe, interleaved in wall time through the lock | 6 hot samples/query |
| `confirm_base_c/d`, `confirm_tip_c/d` | both | q3, q8 only, order base,tip,tip,base | 6 hot samples each |
| `nsys`, `nsys_tip` | base, tip | `--precmd nsys`, 3 iterations in one capture; q2 3 5 8 9 11 12 16 17 18 19 20 21 22 | 1 capture each; hot = iter1, iter2 |
| `dbg`, `dbg_tip` | base (q3 5 17), tip (q3 5) | DEBUG (no trace) log, 4 iterations | consumer-path check without trace overhead |

Scripts: `one_query.sh` (same as the baseline one, `REPO=` selects the build, default `wt-base`), `block.sh`, `timing.sh`,
`nsys_query.sh`, `parse_both.py` (one parser for both log dialects -> JSON), `compare.py` (coverage diff), `timing_table.py`,
`nvtx_cmp.py` + `nvtx_table.py` (DF NVTX ranges per iteration from nsys sqlite).
Health: all base runs rc=0, every parsed window `outcome=ok`, 0 execution-fallback / error / OOM-retry lines, pinned entries assigned.

Caveats (read before using numbers):
- Base logs have no `[dynamic_filter_key]` line. Base facts come from `[publish_dynamic_filters] dynamic filter key N: ... -> kind`
  (debug), `dynamic-filter publication: pushed ...` (info), `accumulated Bloom started/published` (info). The join id is attached by
  build row count (flagged "join guessed" where two joins share a row count; q7 join 21, q18 join 24).
- Base logs `in_list_set=` as estimated bytes; tip logs `size=` as key count for in-lists. In-list sizes are not comparable; Bloom bytes are.
- Operator ms from the TRACE pass are task sums under trace logging: structure only, never a timing.
- NVTX wall = same-thread inclusive time inside the range; it includes waits behind other GPU work (sync), so it is not pure own cost.
  nsys inflates time; one capture per query and build, 2 hot iterations: no variance estimate beyond iter1 vs iter2.
- HOST-staged copies are inside `replicate_devices` (base) and `accum::finish` (both). Reported as bytes, not optimized, not attributed
  to kernels. "explicit host MB" = HtoD + DtoH memcpy records launched in the range; "PtoP-API MB" = `cudaMemcpyPeer` records
  (the driver itself bounces these across the GPU pairs without P2P).

## 1. How the base builds DFs (two paths already exist at base)

- **One-shot** (`publish_dynamic_filters`, NVTX `dynfilter::push_build_side`): when the build side arrives as one whole delivery
  (broadcast and 1-partition builds), the publish hook builds every key from the concatenated batch on one GPU (CONCAT sink time in
  the trace), then `replicate_to_devices` to the other 3 GPUs, then pushes. 37 publications per suite pass: 14 Bloom keys
  (1189 MB), 13 hash IN-lists, 12 small IN-lists.
- **Accumulated Bloom at PARTITION** (multi-partition hash builds and BUILD_FILTER): Bloom only, same mechanics the tip generalizes.
  8 sessions per pass (q2 j42, q4 j9, q7 j35+j38, q10 j10+j21, q20 j49, q21 j18), 2001 MB Bloom per GPU-set, all HOST-staged.

So the stack changes (a) the 37 one-shot publications -> construction at PARTITION, and (b) exact kinds on the accumulated path.

## 3. Timing base vs tip (1 warm + 3 hot per process, 2 processes per build; median and min of 6 hot)

| q | base med (min) ms, 6 hot | base A/B med ms | base A/A % | tip med (min) ms, 6 hot | tip A/B med ms | tip A/A % | tip/base (med) | tip/base (min) | DF-off med ms | verdict | flags |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 469 (464) | 465/471 | +1.1 | 475 (464) | 475/474 | -0.2 | 1.013 | 1.000 | - | within noise | - |
| 2 | 151 (149) | 151/150 | -0.7 | 150 (146) | 151/148 | -2.2 | 0.995 | 0.979 | - | within noise | - |
| 3 | 631 (600) | 639/631 | -1.2 | 666 (664) | 665/666 | +0.2 | 1.055 | 1.108 | - | tip slower | - |
| 4 | 324 (323) | 325/324 | -0.3 | 324 (323) | 325/324 | -0.3 | 0.999 | 1.000 | - | within noise | - |
| 5 | 682 (664) | 675/690 | +2.2 | 693 (680) | 692/695 | +0.4 | 1.016 | 1.025 | - | within noise | - |
| 6 | 381 (380) | 381/381 | +0.1 | 381 (380) | 382/381 | -0.2 | 1.001 | 1.001 | - | within noise | - |
| 7 | 745 (740) | 744/746 | +0.2 | 750 (744) | 749/751 | +0.3 | 1.007 | 1.005 | - | within noise | - |
| 8 | 904 (901) | 902/905 | +0.3 | 940 (930) | 938/942 | +0.5 | 1.040 | 1.033 | - | tip slower | - |
| 9 | 1398 (1382) | 1404/1396 | -0.5 | 1407 (1400) | 1407/1407 | +0.0 | 1.007 | 1.012 | - | within noise | - |
| 10 | 844 (839) | 845/841 | -0.4 | 851 (837) | 864/841 | -2.7 | 1.009 | 0.997 | - | within noise | - |
| 11 | 179 (171) | 181/177 | -1.7 | 179 (173) | 179/179 | +0.5 | 1.000 | 1.014 | - | within noise | - |
| 12 | 709 (708) | 709/710 | +0.0 | 702 (700) | 703/702 | -0.1 | 0.989 | 0.988 | - | within noise | - |
| 13 | 1291 (1281) | 1292/1291 | -0.1 | 1320 (1288) | 1325/1305 | -1.6 | 1.022 | 1.005 | - | within noise | - |
| 14 | 697 (678) | 697/697 | -0.0 | 696 (678) | 697/696 | -0.1 | 0.999 | 1.000 | - | within noise | - |
| 15 | 529 (527) | 528/529 | +0.2 | 529 (526) | 529/528 | -0.2 | 1.000 | 0.998 | - | within noise | - |
| 16 | 307 (304) | 306/308 | +0.7 | 315 (305) | 313/317 | +1.2 | 1.026 | 1.002 | - | within noise | - |
| 17 | 496 (492) | 497/496 | -0.4 | 497 (495) | 497/497 | -0.0 | 1.001 | 1.007 | - | within noise | - |
| 18 | 1090 (1077) | 1084/1091 | +0.7 | 1092 (1079) | 1095/1091 | -0.3 | 1.002 | 1.002 | - | within noise | - |
| 19 | 886 (876) | 887/885 | -0.2 | 889 (876) | 891/888 | -0.4 | 1.004 | 1.000 | - | within noise | - |
| 20 | 713 (701) | 713/713 | -0.1 | 720 (710) | 720/721 | +0.2 | 1.011 | 1.012 | - | within noise | - |
| 21 | 1448 (1427) | 1451/1443 | -0.5 | 1455 (1443) | 1453/1458 | +0.3 | 1.005 | 1.011 | - | within noise | - |
| 22 | 352 (331) | 363/341 | -6.1 | 340 (339) | 340/340 | -0.0 | 0.967 | 1.023 | - | within noise | - |

Queries complete: 22/22. Sum of medians: base 15.225 s, tip 15.372 s (tip/base 1.0097). Geomean of medians: base 591.5 ms, tip 596.0 ms (ratio 1.0075); geomean of mins ratio 1.0099.

Verdict rule: outside max(|A/A| of either build, 2%) on the median and > 1% on the min.
- Suite: tip/base 1.010 on the sum of medians (+147 ms of 15.2 s), geomean 1.0075. Consistent with "no overall regression" within ~1%.
- **q3 +5.5% (631 -> 666 ms) and q8 +4.0% (904 -> 940 ms) are real**: confirm block (base,tip,tip,base; 6 hot each):
  q3 base med 636 (min 626) vs tip 669 (664); q8 base 902 (896) vs tip 937 (935).
- q22 base A/B differ by 6% (363/341): no verdict. q5 +1.6%, q13 +2.2%, q16 +2.6% on the median are inside noise by the rule
  (mins within 0.5-2.5%); q12 -1.1% (tip faster, small but both A/A are 0.1%).
- DF-off column empty: the other agent's DF-off labels were not complete when this was written (see `BASELINE_SUITE.md`).

## 4. One-shot construction cost at base vs construction at tip (H03 reference; nsys NVTX, per hot iteration)

| q | BASE one-shot push_build_side: n, wall ms (iter1, iter2) | of which replicate_devices wall | build kernels GPU ms | BASE accum@PARTITION: contribute GPU ms / finish wall ms | BASE total wall | BASE explicit host MB / PtoP-API MB | TIP begin wall ms (memset MB) | TIP contribute: n, GPU ms (bloom+stage) | TIP finish wall (accum::finish + stage::gather + bounds + replicate) | TIP total wall | TIP explicit host MB / PtoP-API MB | TIP - BASE wall ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 4, 46.4 (46.6, 46.2) | 45.9 | 0.07 | 0.0 / 0.2 | 46.9 | 64 / 150 | 1.07 (72) | 10, 0.03 | 6.6 | 8.1 | 0 / 0 | -38.8 |
| 3 | 2, 178.3 (271.7, 84.9) | 152.4 | 21.81 | 0.0 / 0.0 | 178.3 | 449 / 1239 | 0.40 (1351) | 5, 21.58 | 62.4 | 63.0 | 618 / 2416 | -115.3 |
| 5 | 4, 32.8 (34.4, 31.3) | 22.7 | 2.45 | 0.0 / 0.0 | 32.8 | 129 / 591 | 1.77 (576) | 7, 2.48 | 21.6 | 23.7 | 231 / 866 | -9.1 |
| 8 | 5, 99.5 (157.6, 41.4) | 87.7 | 11.45 | 0.0 / 0.0 | 99.5 | 67 / 1190 | 1.75 (945) | 9, 11.18 | 43.1 | 45.2 | 435 / 1636 | -54.3 |
| 9 | 2, 48.7 (64.1, 33.3) | 45.5 | 2.56 | 0.0 / 0.0 | 48.7 | 218 / 715 | 0.45 (746) | 7, 3.92 | 36.1 | 36.8 | 539 / 1285 | -11.9 |
| 11 | 4, 2.5 (2.7, 2.3) | 2.1 | 0.03 | 0.0 / 0.0 | 2.5 | 12 / 49 | 0.94 (12) | 4, 0.01 | 2.1 | 3.3 | 12 / 49 | +0.8 |
| 12 | 1, 18.7 (13.8, 23.6) | 18.1 | 0.54 | 0.0 / 0.0 | 18.7 | 30 / 267 | 0.20 (238) | 44, 0.79 | 11.4 | 14.0 | 119 / 476 | -4.7 |
| 16 | 1, 8.1 (8.9, 7.3) | 7.5 | 0.52 | 0.0 / 0.0 | 8.1 | 85 / 198 | 0.33 (227) | 3, 0.59 | 8.7 | 9.1 | 113 / 340 | +1.0 |
| 17 | 1, 29.5 (0.7, 58.3) | 29.3 | 0.01 | 0.0 / 0.0 | 29.5 | 2 / 14 | 0.49 (3) | 2, 0.00 | 0.6 | 1.3 | 3 / 13 | -28.2 |
| 18 | 2, 2.7 (4.2, 1.1) | 2.5 | 0.01 | 0.0 / 0.0 | 2.7 | 5 / 15 | 0.46 (4) | 40, 0.06 | 1.7 | 3.8 | 5 / 15 | +1.1 |
| 19 | 1, 1.4 (1.6, 1.1) | 1.2 | 0.02 | 0.0 / 0.0 | 1.4 | 7 / 29 | 0.39 (7) | 2, 0.00 | 1.1 | 1.7 | 7 / 30 | +0.3 |
| 20 | 4, 279.1 (285.9, 272.3) | 278.6 | 0.07 | 0.0 / 0.1 | 279.5 | 20 / 58 | 1.90 (37) | 6, 0.05 | 3.5 | 5.7 | 24 / 93 | -273.8 |
| 21 | 3, 190.1 (209.0, 171.3) | 182.7 | 6.32 | 123.2 / 266.6 | 457.1 | 2797 / 11837 | 0.75 (6113) | 11, 129.26 | 301.6 | 302.8 | 4454 / 10845 | -154.4 |
| 22 | 1, 42.4 (80.7, 4.1) | 42.0 | 0.33 | 0.0 / 0.0 | 42.4 | 18 / 164 | 0.20 (146) | 4, 0.38 | 7.3 | 7.6 | 109 / 255 | -34.7 |

Sum over the 14 captured queries, per hot iteration: base 1248.1 ms, tip 525.9 ms (same-thread wall inside DF construction ranges).

Per-range detail: `$R/runs/base/nsys/nvtx_base.md`, `$R/runs/base/nsys_tip/nvtx_tip.md` (+ `nvtx_df.json`).

Reading:
- **Base one-shot own GPU work is tiny** (build kernels: q3 21.8 ms for 177M Bloom inserts, q8 11.5, q21 6.3, others < 3 ms).
  93-99% of `push_build_side` wall is `replicate_devices`, almost all of it `cudaStreamSynchronize` wait.
- **Base replicate wall is erratic and not proportional to bytes**: q20 279 ms for 56 MB (one 2-key IN-list waits ~271 ms in every
  iteration), q17 0.7 vs 58 ms, q22 81 vs 4 ms, q3 272 vs 85 ms. It waits behind other work on the target GPUs. Query time does not
  follow it (q20 base = tip), so most of that wall is off the critical path.
- **Tip construction wall is smaller in 9 of 14 queries and deterministic** (iter1 = iter2 within 1 ms): sum over the 14 queries
  526 ms vs 1248 ms per iteration. `construction::begin` 0.2-1.9 ms per query (memset 0.2-6 GB queued, not waited). Tip insert GPU ms
  equals base (q3 21.6 vs 21.8; q8 11.2 vs 11.5).
- **Tip moves more bytes** for the same filters: q3 PtoP-API 2416 vs 1239 MB and explicit host 618 vs 449 MB; q8 1636 vs 1190 and
  435 vs 67; q9 1285 vs 715; q5 866 vs 591. Cause: batches sit on up to 4 GPUs (S = 3 sources), so finish does ingress + egress
  where base copied one array to 3 targets. This is the H03 "G-way lease + merge" cost; on this machine it is mostly the excluded
  HOST bounce. `accum::finish` wall is ~0.2 ms per MB of Bloom (q3 62 ms / 338 MB, q8 40 / 231, q21 300 / 1527).
- Small exact builds (q11, q16, q18, q19): tip +0.3 to +1.1 ms per query. Fixed cost per construction is ~0.4-0.5 ms begin +
  ~0.5-1 ms finish. Not material at SF1000.
- q21: base 457 ms (one-shot 190 + accumulated finish 267), tip 303 ms.

## 5. Where the tip spends more / less (structure)

1. **Construction moved onto the build PARTITION task** (trace task sums, base -> tip): q8 PARTITION 5 -> 55 ms with CONCAT sink
   46 -> 0; q5 1 -> 23 (sink 27 -> 0); q3 join-15 build PARTITION 0 -> 73 (57 in the Publishing Task) with CONCAT sink 259 -> 0;
   q9 286 -> 356 (sink 64 -> 0); q12 1 -> 13; q14 273 -> 370 (no DF in q14: not DF, unexplained, timing equal). Total cost is equal or
   lower, but it now sits in the PARTITION task before its deposit.
2. **q8 (+36 ms)**: consumer side identical at both builds (same probes, same rows). The only change is join 41's build
   (91M keys, 174 MB Bloom, 4 batches on 4 GPUs, broadcast/forward path): PARTITION 35 goes 0.1 -> 42.7 ms (35.5 ms in the
   Publishing Task, HOST-staged finish with 3 sources). At both builds the lineitem compressed scan runs only 1 of its 2 probes
   (keep 0.0067 = the part IN-list) and DYNAMIC_FILTER(38) passes 39.96M -> 39.96M, so the join-41 Bloom removes no row anywhere
   (it should be selective: orders date range). Why it is not applied is not logged (consumer gate state is not logged): open.
   So q8 pays ~36-43 ms on the build path for a filter that has no observed effect (H03 + H06; H05 if the gate drops it).
   Causality inferred from matching magnitudes, not proven by a timeline.
3. **q3 (+35 ms)**: publication is earlier at tip, and the consumer path changes. Tip applies both Blooms in the compressed-scan
   decode on every chunk in every iteration (24 of 24 chunks, 0 checkpoints; DEBUG run, 4 of 4 windows). Base is racy: lineitem
   mostly takes the "cached checkpoint" path (per window: 8/8, 8/31, 0/32 probed chunks / checkpoints) and DYNAMIC_FILTER(12) sees
   up to 2.2B rows. Base is still faster. Either the probe scan now waits for the build (serialized instead of overlapped) or the
   decode-pushdown probe costs more than the checkpoint gather. Unverified which; needs a Quent timeline of scan 11 vs join-15 build.
   Same structural change in q5 (base 9-24 chunks probed, tip always 24): +1.6% median, inside noise.
4. **Downstream work drops at tip in the trace pass** (DYNAMIC_FILTER input rows base -> tip: q3 2395M -> 182M, q5 2410M -> 385M,
   q17 3330M -> 6M, q18 7174M -> 1M, q20 755M -> 55M, q21 3718M -> 593M; HASH_JOIN task ms q3 454 -> 167, q17 82 -> 5), yet wall time
   is flat. Under trace logging the base publishes late relative to the scan; info-level timing shows no gain from the earlier
   filter. The "earlier publication helps" benefit assumed in H03/H09 is not visible in timings on any query.

## 6. Implications for the hypotheses

- H01: no lost filter. Drop unless another config shows declines.
- H03: confirmed as a critical-path placement effect, not a total-cost effect. Targets: q8 join 41 (and same shape q5 j32, q9 j29,
  q3 j15): forward-path Bloom builds with batches on 4 GPUs. Candidate from HYPOTHESES (single-GPU inventory shape / publish off the
  task's critical path) applies; measure against q8 +36 ms and q3 +35 ms.
- H05: q8 join 41 Bloom (174 MB, 91M keys) is built and replicated at both builds and reduces no rows (see 5.2). First check why
  the consumer does not apply it; if the gate rejects it, it is the top plan-time-skip target.
- H02/H12: finish wall scales ~0.2 ms/MB here, mostly host bounce (excluded). Report separately on the PEER pair.
- H08: q12 has 44 contributions, q18 40: contribute host time 0.8 / 0.06 GPU ms total. Not material.
- H13: no `bounds::*` range in any capture (zone maps off). Drop.

## 2. Coverage parity base vs tip (H01)

Result: **46 pushed filters at base, 46 at tip; 45 identical kind (and identical Bloom bytes); 0 base-only; 0 tip-only; 1 kind change.**
- q20 join 49 (RIGHT_SEMI, 1 partition, 399,302 keys, 2 targets): base accumulated **Bloom 0.78 MB**, tip **hash IN-list** (exact).
  Reason: tip construction offers exact kinds on the accumulated path; base accumulation is Bloom-only.
- Not pushed at either build: q2 join 42 (targets drained; base built a 1.2 MB accumulated Bloom and never published it, tip logs
  `attempt_skip_publications_skipped_targets_drained`), q20 join 45 (targets drained at both).
- No decline / refusal / lease-skip line at either build. H01 (lost filters) does not occur on TPC-H SF1000 with this config.
- Hot-iteration key outcomes equal iter0 at base for every query.

Full per-query diff (TRACE pass, hot iteration; consumer lines and operator task sums are structure only):


### q1  (base hot window 470 ms, tip 472 ms; joins 0; wired base 0 / tip 0)
no DF keys at base or tip
- consumer base: -
- consumer tip : -
- operator task ms (sum over tasks, exec+sink; trace run, not a timing): PARTITION 1->1, GPU_SCAN 3->3

### q2  (base hot window 161 ms, tip 162 ms; joins 5; wired base 5 / tip 5)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 7 | 4p bcast INNER | 1 | 0 | one-shot small_in_list 0B pushed=1 | small_in_list 1B pushed=1 - | same |
| 15 | 4p bcast INNER | 5 | 0 | one-shot small_in_list 0B pushed=1 | small_in_list 5B pushed=1 - | same |
| 25 | 4p bcast INNER | 799927 | 0 | one-shot in_list 6.1M pushed=1 | hash_in_list 781K pushed=1 - | same |
| 28 | 4p bcast INNER | 1999643 | 0 | one-shot in_list 15.3M pushed=1 | hash_in_list 1.9M pushed=1 - | same |
| 42 | 4p INNER | 638799 | 0 | accum@PARTITION(4b/4G) bloom 1.2M pushed=0 | none 0B pushed=0 attempt_skip_publications_skipped_targets_drained | same |
| 42 | 4p INNER | 638799 | 1 | - | none 0B pushed=0 attempt_skip_publications_skipped_targets_drained | same |
- consumer base: cscan probes 1/1/true: 5 chunks kept 0.2000 of 810000000; DYNAMIC_FILTER(4) keep 0.200 of 25 0ms; DYNAMIC_FILTER(12) keep 0.200 of 10000000 2ms; DYNAMIC_FILTER(22) keep 0.081 of 800000000 12ms; DYNAMIC_FILTER(34) keep 1.000 of 638799 0ms; DYNAMIC_FILTER(39) keep 1.000 of 471301 0ms
- consumer tip : cscan probes 1/1/true: 1 chunks kept 0.2000 of 10000000; cscan probes 1/2/false: 4 chunks kept 0.0040 of 800000000; DYNAMIC_FILTER(4) keep 0.200 of 25 0ms; DYNAMIC_FILTER(12) keep 0.200 of 10000000 2ms; DYNAMIC_FILTER(22) keep 1.000 of 3199708 1ms; DYNAMIC_FILTER(34) keep 1.000 of 638799 0ms; DYNAMIC_FILTER(39) keep 1.000 of 471301 0ms
- operator task ms (sum over tasks, exec+sink; trace run, not a timing): PARTITION 17->24, CONCAT 57->11, HASH_JOIN 18->17, DYNAMIC_FILTER 14->3, GPU_SCAN 4->12

### q3  (base hot window 734 ms, tip 656 ms; joins 2; wired base 2 / tip 2)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 8 | 4p bcast INNER | 30003569 | 0 | one-shot bloom 57.2M pushed=1 | bloom 57.2M pushed=1 - | same |
| 15 | 4p bcast INNER | 147060214 | 0 | one-shot bloom 280.5M pushed=1 | bloom 280.5M pushed=1 - | same |
- consumer base: cscan probes 1/1/true: 8 chunks kept 0.2011 of 735048385; ckpt n=14 1819138663->10341207; DYNAMIC_FILTER(5) keep 1.000 of 147834473 7ms; DYNAMIC_FILTER(12) keep 0.869 of 2246699919 62ms
- consumer tip : cscan probes 1/1/true: 24 chunks kept 0.0833 of 1927817321; DYNAMIC_FILTER(5) keep 1.000 of 147834473 6ms; DYNAMIC_FILTER(12) keep 1.000 of 34116680 13ms
- operator task ms (sum over tasks, exec+sink; trace run, not a timing): PARTITION 83->80, CONCAT 297->18, HASH_JOIN 454->167, DYNAMIC_FILTER 69->19, GPU_SCAN 885->4

### q4  (base hot window 317 ms, tip 323 ms; joins 1; wired base 1 / tip 1)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 9 | 4p RIGHT_SEMI | 149850442 | 0 | accum@PARTITION(10b/4G) bloom 109.4M pushed=1 | bloom 109.4M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #39); not the final regeneration, verify -->
- consumer base: cscan probes 1/1/true: 16 chunks kept 0.0395 of 3897884974; DYNAMIC_FILTER(5) keep 1.000 of 237012238 11ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- consumer tip : cscan probes 1/1/true: 16 chunks kept 0.0395 of 3897884974; DYNAMIC_FILTER(5) keep 1.000 of 237012238 11ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- operator task ms (sum over tasks, exec+sink; trace run, not a timing): PARTITION 80->72, CONCAT 8->7, HASH_JOIN 20->20, BUILD_FILTER 26->27, DYNAMIC_FILTER 11->11, GPU_SCAN 1->1 <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->

### q5  (base hot window 670 ms, tip 680 ms; joins 5; wired base 4 / tip 4)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 10 | 4p bcast INNER | 1 | 0 | one-shot small_in_list 0B pushed=1 | small_in_list 1B pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 18 | 4p bcast INNER | 5 | 0 | one-shot small_in_list 0B pushed=1 | small_in_list 5B pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 25 | 4p bcast INNER | 29997971 | 0 | one-shot bloom 57.2M pushed=1 | bloom 57.2M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 32 | 4p bcast INNER | 45508350 | 0 | one-shot bloom 86.8M pushed=1 | bloom 86.8M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
- consumer base: cscan probes 1/1/true: 9 chunks kept 0.1551 of 516896337; DYNAMIC_FILTER(7) keep 0.200 of 25 0ms; DYNAMIC_FILTER(15) keep 0.200 of 150000000 2ms; DYNAMIC_FILTER(22) keep 1.000 of 45747377 2ms; DYNAMIC_FILTER(29) keep 0.086 of 2214090977 453ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- consumer tip : cscan probes 1/1/true: 24 chunks kept 0.0561 of 2607352286; DYNAMIC_FILTER(7) keep 0.200 of 25 2ms; DYNAMIC_FILTER(15) keep 0.200 of 150000000 3ms; DYNAMIC_FILTER(22) keep 1.000 of 45747377 3ms; DYNAMIC_FILTER(29) keep 1.000 of 189710493 33ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- operator task ms (sum over tasks, exec+sink; trace run, not a timing): PARTITION 1->23, CONCAT 48->35, HASH_JOIN 116->123, DYNAMIC_FILTER 457->41, GPU_SCAN 274->13 <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->

### q6  (base hot window 376 ms, tip 375 ms; joins 0; wired base 0 / tip 0)
no DF keys at base or tip
- consumer base: - <!-- RECOVERED from an earlier compare.py run (call #35, first version); not the final regeneration, verify -->
- consumer tip : - <!-- RECOVERED from an earlier compare.py run (call #35, first version); not the final regeneration, verify -->
- operator task ms (sum over tasks, exec+sink; trace run, not a timing): GPU_SCAN 1->1 <!-- RECOVERED from an earlier compare.py run (call #35, first version); not the final regeneration, verify -->

### q7  (base hot window 882 ms, tip 761 ms; joins 5; wired base 5 / tip 5)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 9 | 4p bcast INNER | 2 | 0 | one-shot small_in_list 0B pushed=1 | small_in_list 2B pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 21 | 4p bcast INNER | 2 | 0 | one-shot small_in_list 0B pushed=1 (join guessed) | small_in_list 2B pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 28 | 4p bcast INNER | 800280 | 0 | one-shot in_list 6.1M pushed=1 | hash_in_list 782K pushed=1 - | same kind, size 6.1M vs 782K | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 35 | 4p INNER | 145884740 | 0 | accum@PARTITION(4b/4G) bloom 278.3M pushed=1 | bloom 278.3M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 38 | 4p INNER | 12000338 | 0 | accum@PARTITION(1b/4G) bloom 22.9M pushed=1 | bloom 22.9M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
- consumer base: cscan probes 1/1/true: 18 chunks kept 0.0800 of 754690081; cscan probes 1/2/false: 5 chunks kept 0.0812 of 1500000000; DYNAMIC_FILTER(6) keep 1.000 of 12000338 0ms; DYNAMIC_FILTER(18) keep 1.000 of 800280 0ms; DYNAMIC_FILTER(25) keep 1.000 of 145884740 124ms; DYNAMIC_FILTER(32) keep 0.084 of 121814490 8ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- consumer tip : cscan probes 1/1/true: 18 chunks kept 0.0800 of 754690081; cscan probes 1/2/false: 5 chunks kept 0.0812 of 1500000000; DYNAMIC_FILTER(6) keep 1.000 of 12000338 0ms; DYNAMIC_FILTER(18) keep 1.000 of 800280 0ms; DYNAMIC_FILTER(25) keep 1.000 of 145884740 61ms; DYNAMIC_FILTER(32) keep 0.084 of 121814490 8ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- operator task ms (sum over tasks, exec+sink; trace run, not a timing): PARTITION 127->113, CONCAT 23->21, HASH_JOIN 66->66, DYNAMIC_FILTER 133->70, GPU_SCAN 82->5 <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->

### q8  (base hot window 974 ms, tip 1004 ms; joins 7; wired base 5 / tip 5)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 19 | 4p bcast INNER | 1 | 0 | one-shot small_in_list 0B pushed=1 | small_in_list 1B pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 27 | 4p bcast INNER | 5 | 0 | one-shot small_in_list 0B pushed=1 | small_in_list 5B pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 34 | 4p bcast INNER | 30002171 | 0 | one-shot bloom 57.2M pushed=1 | bloom 57.2M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 41 | 4p bcast INNER | 91139462 | 0 | one-shot bloom 173.8M pushed=1 | bloom 173.8M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 44 | 4p bcast INNER | 1332415 | 0 | one-shot in_list 10.2M pushed=1 | hash_in_list 1.3M pushed=1 - | same kind, size 10.2M vs 1.3M | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
- consumer base: cscan probes 1/1/true: 8 chunks kept 0.2008 of 605715531; cscan probes 1/2/false: 50 chunks kept 0.0067 of 5999989709; DYNAMIC_FILTER(16) keep 0.200 of 25 0ms; DYNAMIC_FILTER(24) keep 0.200 of 150000000 2ms; DYNAMIC_FILTER(31) keep 1.000 of 91618588 4ms; DYNAMIC_FILTER(38) keep 1.000 of 39957901 65ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- consumer tip : cscan probes 1/1/true: 8 chunks kept 0.2008 of 605715531; cscan probes 1/2/false: 50 chunks kept 0.0067 of 5999989709; DYNAMIC_FILTER(16) keep 0.200 of 25 0ms; DYNAMIC_FILTER(24) keep 0.200 of 150000000 2ms; DYNAMIC_FILTER(31) keep 1.000 of 91618588 4ms; DYNAMIC_FILTER(38) keep 1.000 of 39957901 65ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- operator task ms (sum over tasks, exec+sink; trace run, not a timing): PARTITION 5->55, CONCAT 64->18, HASH_JOIN 124->124, DYNAMIC_FILTER 71->71, GPU_SCAN 6->6 <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->

### q9  (base hot window 1392 ms, tip 1407 ms; joins 5; wired base 2 / tip 2)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 19 | 4p bcast INNER | 10870459 | 0 | one-shot bloom 20.7M pushed=1 | bloom 20.7M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 29 | 4p bcast INNER | 43481836 | 0 | one-shot bloom 82.9M pushed=1 | bloom 82.9M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 29 | 4p bcast INNER | 43481836 | 1 | one-shot bloom 82.9M pushed=1 | bloom 82.9M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
- consumer base: cscan probes 1/2/false: 16 chunks kept 0.9886 of 1565969860; DYNAMIC_FILTER(16) keep 0.424 of 800000000 4ms; DYNAMIC_FILTER(26) keep 0.054 of 5999989709 550ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- consumer tip : cscan probes 1/1/true: 4 chunks kept 0.0556 of 800000000; cscan probes 1/2/false: 16 chunks kept 0.9886 of 1565969860; DYNAMIC_FILTER(16) keep 1.000 of 44477640 1ms; DYNAMIC_FILTER(26) keep 0.054 of 5999989709 468ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
- operator task ms (sum over tasks, exec+sink; trace run, not a timing): PARTITION 286->356, CONCAT 762->635, HASH_JOIN 953->905, DYNAMIC_FILTER 554->470, GPU_SCAN 140->127 <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->

### q10  (base hot window 833 ms, tip 917 ms; joins 3; wired base 2 / tip 2)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 10 | 4p INNER | 57360614 | 0 | accum@PARTITION(7b/4G) bloom 109.4M pushed=1 | bloom 109.4M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
| 21 | 4p INNER | 44529913 | 0 | accum@PARTITION(4b/4G) bloom 84.9M pushed=1 | bloom 84.9M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #35); not the final regeneration, verify -->
- consumer base: cscan probes 1/1/true: 23 chunks kept 0.1268 of 683433385; ckpt n=52 266507446->161175938; DYNAMIC_FILTER(7) keep 1.000 of 116507446 1ms; DYNAMIC_FILTER(18) keep 1.000 of 44668492 1ms <!-- RECOVERED from an earlier compare.py run (call #35, first version of parse_both/compare; numbers may differ); not the final regeneration, verify -->
<!-- LOST in recovery: q10 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q10 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q11  (base hot window 185 ms, tip 191 ms; joins 4; wired base 4 / tip 4)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
<!-- LOST in recovery: q11 table row 1 of 4 (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q11 table row 2 of 4 (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q11 table row 3 of 4 (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q11 table row 4 of 4 (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q11 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q11 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q11 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q12  (base hot window 704 ms, tip 823 ms; joins 1; wired base 1 / tip 1)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 8 | 4p bcast INNER | 31165956 | 0 | one-shot bloom 59.4M pushed=1 | bloom 59.4M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #39); not the final regeneration, verify -->
<!-- LOST in recovery: q12 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q12 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q12 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q13  (base hot window 1361 ms, tip 1293 ms; joins 0; wired base 0 / tip 0)
no DF keys at base or tip
<!-- LOST in recovery: q13 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q13 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q13 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q14  (base hot window 776 ms, tip 760 ms; joins 1; wired base 0 / tip 0)
no DF keys at base or tip
<!-- LOST in recovery: q14 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q14 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q14 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q15  (base hot window 527 ms, tip 525 ms; joins 2; wired base 0 / tip 0)
no DF keys at base or tip
<!-- LOST in recovery: q15 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q15 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q15 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q16  (base hot window 325 ms, tip 335 ms; joins 2; wired base 1 / tip 1)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 11 | 4p bcast INNER | 29700215 | 0 | one-shot bloom 56.6M pushed=1 | bloom 56.6M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
<!-- LOST in recovery: q16 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q16 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q16 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q17  (base hot window 487 ms, tip 486 ms; joins 2; wired base 1 / tip 1)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 7 | 4p bcast INNER | 200585 | 0 | one-shot in_list 1.5M pushed=1 | hash_in_list 196K pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
<!-- LOST in recovery: q17 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q17 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q17 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q18  (base hot window 1082 ms, tip 1073 ms; joins 3; wired base 2 / tip 2)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 14 | 4p bcast SEMI | 63430 | 0 | one-shot in_list 991K pushed=1 | hash_in_list 62K pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
| 24 | 4p bcast INNER | 63430 | 0 | one-shot in_list 991K pushed=1 (join guessed) | hash_in_list 62K pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
<!-- LOST in recovery: q18 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q18 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q18 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q19  (base hot window 878 ms, tip 879 ms; joins 1; wired base 1 / tip 1)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 9 | 4p bcast INNER | 480774 | 0 | one-shot in_list 3.7M pushed=1 | hash_in_list 470K pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
<!-- LOST in recovery: q19 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q19 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q19 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q20  (base hot window 705 ms, tip 710 ms; joins 5; wired base 5 / tip 5)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 7 | 4p bcast INNER | 1 | 0 | one-shot small_in_list 0B pushed=1 | small_in_list 1B pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #39); not the final regeneration, verify -->
| 18 | 4p bcast SEMI | 2174140 | 0 | one-shot bloom 4.1M pushed=1 | bloom 4.1M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #39); not the final regeneration, verify -->
| 31 | 4p bcast SEMI | 347054 | 0 | one-shot in_list 2.7M pushed=1 | hash_in_list 339K pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #39); not the final regeneration, verify -->
| 31 | 4p bcast SEMI | 347054 | 1 | one-shot in_list 2.7M pushed=1 | hash_in_list 339K pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #39); not the final regeneration, verify -->
| 45 | 4p bcast INNER | 132209 | 0 | - | none 0B pushed=0 attempt_skip_publications_skipped_targets_drained | same | <!-- RECOVERED from an earlier compare.py run (call #39); not the final regeneration, verify -->
| 45 | 4p bcast INNER | 132209 | 1 | - | none 0B pushed=0 attempt_skip_publications_skipped_targets_drained | same | <!-- RECOVERED from an earlier compare.py run (call #39); not the final regeneration, verify -->
| 49 | 1p RIGHT_SEMI | 131751 | 0 | accum@PARTITION(1b/4G) bloom 780K pushed=1 | hash_in_list 390K pushed=2 - | **kind differs** [('bloom', 1)] vs [('in_list', 2)] | <!-- RECOVERED from an earlier compare.py run (call #39); not the final regeneration, verify -->
- base decline x1: [publish_dynamic_filters] Skipping dynamic filter push: all target scans drained. <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
<!-- LOST in recovery: q20 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q20 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q20 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q21  (base hot window 1432 ms, tip 1435 ms; joins 5; wired base 4 / tip 4)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
| 7 | 4p bcast INNER | 1 | 0 | one-shot small_in_list 0B pushed=1 | small_in_list 1B pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
| 18 | 4p INNER | 730806711 | 0 | accum@PARTITION(5b/4G) bloom 1.4G pushed=1 | bloom 1.4G pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
| 21 | 4p bcast INNER | 399867 | 0 | one-shot in_list 3.1M pushed=1 | hash_in_list 390K pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
| 33 | 4p bcast SEMI | 69665236 | 0 | one-shot bloom 132.9M pushed=1 | bloom 132.9M pushed=1 - | same | <!-- RECOVERED from an earlier compare.py run (call #36); not the final regeneration, verify -->
<!-- LOST in recovery: q21 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q21 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q21 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->

### q22  (base hot window 363 ms, tip 339 ms; joins 2; wired base 1 / tip 1)
| join | layout | build rows | key | base: path kind size pushed | tip: kind size pushed reason | diff |
|---|---|---|---|---|---|---|
<!-- LOST in recovery: q22 "consumer base" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q22 "consumer tip" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->
<!-- LOST in recovery: q22 "operator task ms" line (regenerate: runs/base/bin/parse_both.py + compare.py on a TRACE log pass) -->


TOTAL pushed filters: base 46 tip 46 | same 45 kind-differs 1 base-only 0 tip-only 0
