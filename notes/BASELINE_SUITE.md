# Baseline suite: tip (pr2570 `ae939824`) TPC-H SF1000, 4 GPUs

Run 2026-11-18. Build: `$R/repo` (release). Config: `/home/ubuntu/sirius/specific_config.yaml` unmodified.
Regime: fresh process per query, host pin + Simpatico pin compression (as the config), 4 GPUs.
Raw output: `$R/runs/baseline/`. Scripts: `$R/runs/baseline/bin/`. All GPU runs under `flock $R/machine.lock`.

## 0. Working recipe (reuse this)

```bash
B=/opt/dlami/nvme/dfperf/runs/baseline/bin
# one TPC-H query, fresh process, N iterations (iter0 = warm), output $R/runs/baseline/<label>/, Quent -> $R/telemetry/<label>/q<N>/
flock /opt/dlami/nvme/dfperf/machine.lock $B/one_query.sh <label> <q> <iterations>
$B/block.sh <label> <iterations> 1 2 3 ...           # loop of the above, lock taken per query
SIRIUS_LOG_LEVEL=trace $B/block.sh logpass 2 $(seq 1 22)   # log pass (trace = debug + per-operator rows/time)
# DF off: the SET is a TEST-ONLY option, it does not exist unless SIRIUS_ENABLE_TEST_OPTIONS=1
SIRIUS_ENABLE_TEST_OPTIONS=1 SIRIUS_PRE_SQL="SET enable_dynamic_filter = false;" flock $LOCK $B/one_query.sh <label> <q> 4
python3 $B/parse_df.py $R/runs/baseline/<label> [q ...]   # per-query DF construction tables from trace/debug logs (+ df_summary.json)
python3 $B/timing_table.py                                 # timing table from the 4 timing labels
```
`one_query.sh` = env block of tooling.md section 0 + `performance_test.py --input $DATA --scale-factor 1000 --data-source parquet
--engine gpu --config $SIRIUS_CONFIG_FILE --iterations N --queries Q --pin host --pin-compression --compression-plan-dir
/home/ubuntu/sirius/src/compression/simpatico_codegen/plans/tpch_sf1000 --output $R/runs/baseline/<label> --name q<Q>`, then moves the
Quent session dir(s) created by that process from `/home/ubuntu/sirius/results/telemetry/` to `$R/telemetry/<label>/q<Q>/`
(before/after listing inside the locked script; `DATA=`/`SF=` env override the dataset).

Recipe facts / hazards found:
- **`SET enable_dynamic_filter` fails with "unrecognized configuration parameter"** unless `SIRIUS_ENABLE_TEST_OPTIONS=1` is in the
  environment (`option_visibility::internal`, `src/sirius_extension.cpp:141-158, 3634-3641`). Same for `enable_dynamic_zone_map_filter`
  and other internal options. The env var only registers options; tip runs were taken without it.
- The split per-query log `<bench>/sirius/q<N>/sirius.log` can lose the tail of the last iteration (`[window] end` missing for
  q5/q10/q12/q18). Parse `<bench>/log_dir/sirius_*.log` instead (parse_df.py does).
- `grep -i fallback` always hits one benign config line ("cuDF pinned memory resource ... fallback node=0"). Use
  `execution fallback|fallback to DuckDB|falling back`.
- A fresh process per query costs 35-60 s wall (LOAD + pin + 4 iterations); first-ever q3 iter0 took 58 s (JIT/OS cache cold), later 1-17 s.
- Consumer-side gate state (kept/disabled) is **not logged at any level**. Available: per-chunk compressed-scan lines
  `[decompression-pushdown] join filters kept X/Y rows (N of M probe(s) ran, complete=..)` (debug) and per-task
  `DYNAMIC_FILTER (id=) ... executing on/produced ... num rows` (trace). Per-filter keep is only inferable when one probe ran.
- Quent session dirs: 1-2 MB per query process (4 iterations).

Smoke (q1, q3; 2 iterations): rc=0, `outcome=ok` windows, pinned entries assigned, 0 fallback/error/OOM-retry lines, GPU path used
(q3 logs 2 DF constructions per iteration). q3 hot 0.81 s.

## 1. Log pass (untimed; TRACE; 2 iterations per query: iter0 cold + iter1 hot; tables are the hot iteration)

Full per-query tables (every join, consumer lines, PARTITION operator times): `$R/runs/baseline/logpass/df_tables.md`;
machine-readable: `$R/runs/baseline/logpass/df_summary.json`. All 22 queries: rc=0, last window `outcome=ok`, 0 fallback / error /
OOM-retry lines. Key outcomes identical between cold and hot iteration for 21/22 (q2 join 42 differs, see H01).

Association caveat: `construction started` / `published` / Bloom `finish` lines carry no join id; they are attached to the join of
the next `[dynamic_filter_key]` line (Bloom finish bytes matched the join's key sizes in every case). "build PARTITION sum/max ms"
is matched by row count (ambiguous where two operators carry the same rows; q4's DF build is PARTITION id=2: 10 tasks, 29.8 ms sum, 3.4 max).
Path: `forward` = `mode != hash || partitions < 2` (`sirius_physical_partition.cpp:158`), i.e. broadcast and 1-partition; `hash(scatter)` = 4p hash.
"pub ms after final" for hash-path rows includes one scatter (HYPOTHESES C2). Trace logging inflates times slightly.

### 1.1 Every DF construction (hot iteration)

| q | join | type | layout/path | key kinds | valid rows | domain (prov) | bloom MB | #batches | GPUs leased | bloom src/tgt | pub ms after final | HOST-staged | build PARTITION sum/max ms | note |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 7 | INNER | 4p broadcast bc / forward | small_in_list | 1 | 5(exact) | - | 1 | 4 | - | 0.2 | no | 0.2/0.2 | pushed=1  |
| 2 | 15 | INNER | 4p broadcast bc / forward | small_in_list | 5 | unknown | - | 1 | 4 | - | 0.2 | no | 0.0/0.0; 0.2/0.2 | pushed=1  |
| 2 | 28 | INNER | 4p broadcast bc / forward | hash_in_list | 1999643 | unknown | - | 1 | 4 | - | 3.2 | no | 0.0/0.0; 3.2/3.2 | pushed=1  |
| 2 | 25 | INNER | 4p broadcast bc / forward | hash_in_list | 799927 | 200000000(exact) | - | 3 | 4 | - | 1.5 | no | 1.6/1.6 | pushed=1  |
| 2 | 42 | INNER | 4p hash / hash(scatter) | none+none | 638799 | unknown;unknown | - | 4 | 4 | - | - | - | 18.5/5.4 | pushed=0/0 attempt_skip_publications_skipped_targets_drained |
| 3 | 8 | INNER | 4p broadcast bc / forward | bloom | 30003569 | 150000000(exact) | 57.2 | 1 | 4 | 0/3 | 4.1 | yes | 4.2/4.2 | pushed=1  |
| 3 | 15 | INNER | 4p broadcast bc / forward | bloom | 147060214 | unknown | 280.5 | 4 | 4 | 3/3 | 57.0 | yes | 72.8/57.1 | pushed=1  |
| 4 | 9 | RIGHT_SEMI | 4p hash / hash(scatter) | bloom | 57356572 | ? | 109.4 | 10 | 4 | 3/3 | 20.9 | yes | 42.3/4.3 | pushed=1  |
| 5 | 10 | INNER | 4p broadcast bc / forward | small_in_list | 1 | 5(exact) | - | 1 | 4 | - | 0.2 | no | 0.3/0.3 | pushed=1  |
| 5 | 18 | INNER | 4p broadcast bc / forward | small_in_list | 5 | unknown | - | 1 | 4 | - | 0.2 | no | 0.0/0.0; 0.2/0.2 | pushed=1  |
| 5 | 25 | INNER | 4p broadcast bc / forward | bloom | 29997971 | unknown | 57.2 | 1 | 4 | 0/3 | 4.1 | yes | 0.0/0.0; 4.2/4.2 | pushed=1  |
| 5 | 32 | INNER | 4p broadcast bc / forward | bloom | 45508350 | unknown | 86.8 | 4 | 4 | 3/3 | 16.7 | yes | 18.1/16.7 | pushed=1  |
| 7 | 9 | INNER | 4p broadcast bc / forward | small_in_list | 2 | 25(exact) | - | 1 | 4 | - | 0.2 | no | 0.3/0.3; 0.3/0.3 | pushed=1  |
| 7 | 38 | INNER | 4p hash / hash(scatter) | bloom | 12000338 | unknown | 22.9 | 1 | 4 | 0/3 | 3.8 | yes | 0.0/0.0; 3.8/3.8 | pushed=1  |
| 7 | 21 | INNER | 4p broadcast bc / forward | small_in_list | 2 | 25(exact) | - | 1 | 4 | - | 0.3 | no | 0.3/0.3; 0.3/0.3 | pushed=1  |
| 7 | 28 | INNER | 4p broadcast bc / forward | hash_in_list | 800280 | unknown | - | 1 | 4 | - | 1.5 | no | 0.0/0.0; 1.5/1.5 | pushed=1  |
| 7 | 35 | INNER | 4p hash / hash(scatter) | bloom | 145884740 | unknown | 278.3 | 4 | 4 | 3/3 | 63.2 | yes | 99.6/63.2 | pushed=1  |
| 8 | 44 | INNER | 4p broadcast bc / forward | hash_in_list | 1332415 | 200000000(exact) | - | 2 | 4 | - | 2.3 | no | 2.4/2.4 | pushed=1  |
| 8 | 19 | INNER | 4p broadcast bc / forward | small_in_list | 1 | 5(exact) | - | 1 | 4 | - | 0.2 | no | 0.3/0.3 | pushed=1  |
| 8 | 27 | INNER | 4p broadcast bc / forward | small_in_list | 5 | unknown | - | 1 | 4 | - | 0.3 | no | 0.0/0.0; 0.3/0.3 | pushed=1  |
| 8 | 34 | INNER | 4p broadcast bc / forward | bloom | 30002171 | unknown | 57.2 | 1 | 4 | 0/3 | 4.1 | yes | 0.0/0.0; 4.1/4.1 | pushed=1  |
| 8 | 41 | INNER | 4p broadcast bc / forward | bloom | 91139462 | unknown | 173.8 | 4 | 4 | 3/3 | 35.4 | yes | 42.7/35.5 | pushed=1  |
| 9 | 19 | INNER | 4p broadcast bc / forward | bloom | 10870459 | 200000000(exact) | 20.7 | 3 | 4 | 2/3 | 3.6 | yes | 3.8/3.6 | pushed=1  |
| 9 | 29 | INNER | 4p broadcast bc / forward | bloom+bloom | 43481836 | unknown;unknown | 165.9 | 4 | 4 | 3/3 | 31.8 | yes | 0.0/0.0; 35.2/31.8 | pushed=1/1  |
| 10 | 10 | INNER | 4p hash / hash(scatter) | bloom | 57360614 | 1500000000(exact) | 109.4 | 7 | 4 | 3/3 | 21.9 | yes | 33.3/21.9 | pushed=1  |
| 10 | 21 | INNER | 4p hash / hash(scatter) | bloom | 44529913 | unknown | 84.9 | 4 | 4 | 3/3 | 17.3 | yes | 21.9/17.3 | pushed=1  |
| 11 | 7 | INNER | 4p broadcast bc / forward | small_in_list | 1 | 25(exact) | - | 1 | 4 | - | 0.2 | no | 0.3/0.3; 0.0/0.0 | pushed=1  |
| 11 | 14 | INNER | 4p broadcast bc / forward | hash_in_list | 399948 | unknown | - | 1 | 4 | - | 0.9 | no | 0.0/0.0; 0.9/0.9 | pushed=1  |
| 11 | 28 | INNER | 4p broadcast bc / forward | small_in_list | 1 | 25(exact) | - | 1 | 4 | - | 0.2 | no | 0.3/0.3; 0.0/0.0 | pushed=1  |
| 11 | 35 | INNER | 4p broadcast bc / forward | hash_in_list | 399948 | unknown | - | 1 | 4 | - | 0.9 | no | 0.0/0.0; 0.9/0.9 | pushed=1  |
| 12 | 8 | INNER | 4p broadcast bc / forward | bloom | 31165956 | 5999989709(exact) | 59.4 | 44 | 4 | 3/3 | 11.2 | yes | 13.0/11.3 | pushed=1  |
| 16 | 11 | INNER | 4p broadcast bc / forward | bloom | 29700215 | 200000000(exact) | 56.6 | 3 | 4 | 2/3 | 8.4 | yes | 8.9/8.5 | pushed=1  |
| 17 | 7 | INNER | 4p broadcast bc / forward | hash_in_list | 200585 | 200000000(exact) | - | 2 | 4 | - | 0.6 | no | 0.8/0.7 | pushed=1  |
| 18 | 14 | SEMI | 4p broadcast bc / forward | hash_in_list | 63430 | 5999989709(exact) | - | 36 | 4 | - | 1.0 | no | 2.3/1.0 | pushed=1  |
| 18 | 24 | INNER | 4p broadcast bc / forward | hash_in_list | 63430 | unknown | - | 4 | 4 | - | 0.5 | no | 2.4/0.7; 0.6/0.5 | pushed=1  |
| 19 | 9 | INNER | 4p broadcast bc / forward | hash_in_list | 480774 | 200000000(exact) | - | 2 | 4 | - | 1.1 | no | 1.3/1.3 | pushed=1  |
| 20 | 7 | INNER | 4p broadcast bc / forward | small_in_list | 1 | 25(exact);unknown | - | 1 | 4 | - | 0.2 | no | 0.3/0.3 | pushed=1  |
| 20 | 49 | RIGHT_SEMI | 1p hash / forward | hash_in_list | 399302 | ? | - | 1 | 4 | - | 0.9 | no | 0.0/0.0 | pushed=2  |
| 20 | 18 | SEMI | 4p broadcast bc / forward | bloom | 2174140 | 200000000(exact) | 4.1 | 3 | 4 | 2/3 | 0.9 | yes | 1.1/1.0 | pushed=1  |
| 20 | 31 | SEMI | 4p broadcast bc / forward | hash_in_list+hash_in_list | 347054 | unknown;unknown | - | 1 | 4 | - | 1.4 | no | 1.4/1.4 | pushed=1/1  |
| 20 | 45 | INNER | 4p broadcast bc / forward | none+none | 132209 | 5999989709(exact);5999989709(exact) | - | 1 | 4 | - | - | - | 0.0/0.0 | pushed=0/0 attempt_skip_publications_skipped_targets_drained |
| 21 | 7 | INNER | 4p broadcast bc / forward | small_in_list | 1 | 25(exact) | - | 1 | 4 | - | 0.2 | no | 0.2/0.2 | pushed=1  |
| 21 | 21 | INNER | 4p broadcast bc / forward | hash_in_list | 399867 | unknown | - | 1 | 4 | - | 0.9 | no | 0.0/0.0; 0.9/0.9 | pushed=1  |
| 21 | 18 | INNER | 4p hash / hash(scatter) | bloom | 730806711 | 1500000000(exact) | 1393.9 | 5 | 4 | 3/3 | 316.9 | yes | 473.5/317.0 | pushed=1  |
| 21 | 33 | SEMI | 4p broadcast bc / forward | bloom | 69665236 | unknown | 132.9 | 4 | 4 | 3/3 | 26.0 | yes | 3.7/1.0; 30.7/26.0 | pushed=1  |
| 22 | 28 | SEMI | 4p broadcast bc / forward | bloom | 19092648 | unknown | 36.4 | 4 | 4 | 3/3 | 7.1 | yes | 7.5/7.1 | pushed=1  |

Joins that appear in `partition strategy` but have no DF key line (q5 j35, q8 j6/j47, q9 j32/j9/j22, q10 j25, q14 j6, q15 j13/j19,
q16 j14, q17 j21, q18 j17, q21 j42, q22 j33) were never wired for DF by the planner (`Wired hash join` count == constructions in
every query: 46 wired, 46 `construction started`). q1, q6, q13, q14, q15: no DF at all.

**Third construction site**: q4 (join 9, RIGHT_SEMI) and q20 (join 49, RIGHT_SEMI) construct inside a **BUILD_FILTER** operator, not
PARTITION. q4 BUILD_FILTER id=1: 10 tasks, 57.36M rows, 26.7 ms summed task time, 20.95 ms in the publishing task (9 contributing
tasks 0.4-1.1 ms each for ~6.1M rows each, run concurrently on 4 GPUs). q20 BUILD_FILTER id=8: 1 task 0.9 ms.

### 1.2 Per-query DF summary (hot iteration)

| q | wired = constructions | keys | kinds | Bloom MB | max batches | HOST-staged publication ms (sum, "after final") | other publication ms (sum) |
|---|---|---|---|---|---|---|---|
| 2 | 5 | 6 | 2 small, 2 hash IN, 2 none (skipped) | 0 | 4 | 0 | 5.1 |
| 3 | 2 | 2 | 2 bloom | 338 | 4 | 61.1 | 0 |
| 4 | 1 | 1 | bloom (BUILD_FILTER site) | 109 | 10 | 20.9 | 0 |
| 5 | 4 | 4 | 2 small, 2 bloom | 144 | 4 | 20.8 | 0.4 |
| 7 | 5 | 5 | 2 small, 1 hash IN, 2 bloom | 301 | 4 | 67.0 | 2.0 |
| 8 | 5 | 5 | 2 small, 1 hash IN, 2 bloom | 231 | 4 | 39.5 | 2.8 |
| 9 | 2 | 3 | 3 bloom (one 2-key join) | 187 | 4 | 35.4 | 0 |
| 10 | 2 | 2 | 2 bloom | 194 | 7 | 39.2 | 0 |
| 11 | 4 | 4 | 2 small, 2 hash IN | 0 | 1 | 0 | 2.2 |
| 12 | 1 | 1 | bloom | 59 | 44 | 11.2 | 0 |
| 16 | 1 | 1 | bloom | 57 | 3 | 8.4 | 0 |
| 17 | 1 | 1 | hash IN | 0 | 2 | 0 | 0.6 |
| 18 | 2 | 2 | 2 hash IN | 0 | 36 | 0 | 1.5 |
| 19 | 1 | 1 | hash IN | 0 | 2 | 0 | 1.1 |
| 20 | 5 | 7 | 1 small, 3 hash IN, 1 bloom, 2 none (skipped) | 4 | 3 | 0.9 | 2.5 |
| 21 | 4 | 4 | 1 small, 1 hash IN, 2 bloom | 1527 | 5 | 342.9 | 1.1 |
| 22 | 1 | 1 | bloom | 36 | 4 | 7.1 | 0 |
| suite | 46 | 50 | 20 bloom keys / 19 Bloom publications | 3188 | 44 | **654** | **19** |

Consumer side (hot iteration; gate state itself is not logged, see section 0):
- Observed keep ratios of join-filter probes inside compressed scans (rows kept / rows probed): q2 0.20 and 0.004; q3 0.083; q4 0.040;
  q5 0.056; q7 0.080; q8 0.20 and 0.007; q9 0.056 and **0.989**; q10 0.127; q11 0.040; q12 0.020; q16 0.150; q17 0.001; q18 0.0001;
  q19 0.002; q20 0.040 / 0.031; q21 0.059 / 0.040. q22 via DYNAMIC_FILTER operator: 1.5B -> 192.7M (0.128).
- "1 of 2 probes ran, complete=false" (scan ran one of two requested probes; the other is applied later by the DYNAMIC_FILTER
  operator or not at all): q2, q7, q8, q9, q20, q21. DYNAMIC_FILTER operators that still removed rows: q7 id=32 121.8M -> 10.3M;
  q9 id=26 6.0B -> 326M (**468 ms summed task time over 62 tasks**); q16 id=8 800M -> 119.7M; q20 id=15/28; q22 id=25; q2, q5, q8 small dims.
- Most DYNAMIC_FILTER operators show keep = 1.000 because the scan already applied the filter; that is not a "useless filter" signal.
- No `passed through unfiltered` warning in any query.

### 1.3 Hypotheses settled by the log pass

- **H01 (declines/skips): no hit.** 0 lines of `construction declined`, `constructs no dynamic filter`, `construction skipped`
  (lease refused), `Bloom filters refused`. All 46 wired joins started construction; 44 published. The 2 that did not:
  q2 join 42 (2 keys, MIXED_JOIN, 4p hash, 638,799 rows) and q20 join 45 (2 keys, 132,209 rows), both
  `reason=attempt_skip_publications_skipped_targets_drained` (consumers already finished). q2 join 42 is timing dependent: in the
  cold iteration it built 2 hash IN-lists and pushed 1. No compressed/unreadable arrival, no non-GPU-tier batch.
  Not checked here: parity against the base build (needs the base log pass; another agent runs `$R/runs/base`).
- **H02a (rows vs distinct): the domain cap saves 0 bytes on TPC-H SF1000.** Every Bloom is exactly 16.0 bits per valid row
  (20/20 keys). Where a domain is logged it is always larger than the valid rows (q3 j8 30.0M rows / 150M; q9 j19 10.9M / 200M;
  q10 j10 57.4M / 1.5B; q12 j8 31.2M / 6.0B; q16 j11 29.7M / 200M; q20 j18 2.2M / 200M; q21 j18 730.8M / 1.5B); 12 of 20 Bloom keys
  have `domain=0 provenance=unknown`. So `min(valid, domain)` never binds. True distinct counts are not logged; duplicate-heavy
  candidates by schema (l_orderkey from lineitem): **q21 j18 (730.8M rows, 1.39 GB)**, q10 j10 (57.4M, 109 MB), q12 j8 (31.2M, 59 MB).
  Oversizing there is plausible but **unverified** (needs a distinct count, e.g. SQL `count(distinct)` on the build side).
- **H05 (built but disabled): one clear hit.** q9 join 29 builds two 82.9 MB Blooms (ps_partkey, ps_suppkey; 43.5M rows, 165.9 MB
  through one HOST-staged finish, 31.8 ms). One of the two keeps 98.9% of probed lineitem rows (1.548B / 1.566B over 16 chunks),
  above the 0.9 gate threshold; which condition it is is inferred (suppkey), not logged. Every other observed filter keeps <= 20%.
  Per-filter keep is not observable for the "1 of 2 probes" scans of q2/q7/q8/q20/q21. The two skipped attempts (H01) are also
  "work with no filter": q2 j42's 4 hash-path PARTITION tasks sum 18.5 ms, but that includes the scatter.
- **H08 (batch counts): no hit.** Batches per construction: 1 (24x), 2 (3x), 3 (4x), 4 (10x), 5, 7, 10, 36, 44 (1x each). Never > 50.
  The 36-batch exact build (q18 j14, 63,430 rows) costs 2.3 ms summed PARTITION time (1.0 ms publishing task). The 44-batch Bloom
  build (q12 j8) costs 13.0 ms summed, 11.2 of it the HOST-staged publication: ~0.04 ms per contributing batch.
- **H13 (bounds): no use.** `zone_map=0` on all 50 key lines, no `replicated N zone(s)` line, no bounds kind. Do not spend time on it.
- Layout facts for H03/H07: **every** construction leases on 4 GPUs (`on 4 GPU(s)`), including 1-row builds. 38 of 46
  constructions are forward path at PARTITION (all `4p broadcast`), 6 are hash path at PARTITION (q2 j42, q7 j38/j35, q10 j10/j21,
  q21 j18), 2 are at BUILD_FILTER (q4 j9, q20 j49). Forward small builds cost 0.2-0.3 ms (small IN-list), 0.6-3.2 ms (hash IN-list, 63K-2M keys), 4.1-4.2 ms (57 MB
  Bloom from one batch, 0 sources / 3 targets, HOST-staged replicas included).
- Publication latency (INFO line): all 19 Bloom publications are `through HOST staging` (38/38 finish lines incl. cold iteration);
  none took the PEER path. ms vs bytes, 3 sources / 3 targets: 36 MB 7.1; 57 MB 8.4; 59 MB 11.2; 87 MB 16.7; 109 MB 20.9-21.9;
  133 MB 26.0; 166 MB 31.8; 174 MB 35.4; 278 MB 63.2 (hash, incl. scatter); 280 MB 57.0; 1394 MB 316.9 (hash, incl. scatter)
  => ~0.19-0.23 ms per MB. 0 sources / 3 targets: 23 MB 3.8; 57 MB 4.1 => ~0.07 ms per MB. **This is the excluded HOST bounce**
  (report only). Non-Bloom publications: 0.2-3.2 ms each, 19 ms over the whole suite.
- Non-excluded construction cost visible in trace task times (forward path = the PARTITION task does nothing else):
  q3 j15: 3 non-publishing tasks 15.7 ms for ~110M rows into a 280 MB array (~5 ms per 37M-row batch); q4: 0.4-1.1 ms per 6.1M-row
  batch. These are tens of ms of summed task time per suite, spread over 4 GPUs: small next to the 654 ms of HOST-staged finish.

## 2. Timing (INFO logs, Quent on, 4 GPUs)

Protocol: per query, a fresh locked process per run: 1 warm + 3 hot iterations (`--iterations 4`, hot = iter 1-3). Runs
interleaved per query in time: tip A, tip B (labels `baseline_tip`, `baseline_tip_b`) and DF-off A, DF-off B (`baseline_off`,
`baseline_off_b`; `SIRIUS_ENABLE_TEST_OPTIONS=1` + `SET enable_dynamic_filter = false`). 88 runs, all rc=0, every window `outcome=ok`,
pinned entries assigned, 0 fallback / error / OOM-retry lines; DF-off logs contain 0 `construction started` / `Wired` /
DYNAMIC_FILTER lines. Results of tip vs DF-off are byte-identical for 21 queries; q11 differs only in the order of ties (same
multiset; tip A vs tip B differ the same way). Results were not validated against DuckDB CPU.
Other agents used the machine between runs (lock respected); the per-query interleave keeps A/A/off close in time.
Labels `baseline_dfoff*` are failed runs (SET rejected, see section 0): ignore.
"tip med (6 hot)" = median of the 6 hot iterations of A+B. Net value = DF-off median - tip median (positive = DF helps).

| q | tip A med (min) ms | tip B med (min) ms | A/A delta % (B vs A) | tip med ms (6 hot) | DF-off A med ms | DF-off B med ms | off A/A % | DF-off med ms (6 hot) | DF net value ms (off - tip) | off/tip | tip constr/iter | flags |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 475 (464) | 474 (472) | -0.2 | 475 | 471 | 466 | -1.2 | 468 | -7 | 0.98x | 0 | hot spread 3%  |
| 2 | 151 (149) | 148 (146) | -2.2 | 150 | 142 | 142 | +0.1 | 142 | -8 | 0.95x | 5 | hot spread 21%  |
| 3 | 665 (664) | 666 (665) | +0.2 | 666 | 672 | 663 | -1.3 | 666 | +1 | 1.00x | 2 | hot spread 0%  |
| 4 | 325 (323) | 324 (323) | -0.3 | 324 | 681 | 683 | +0.4 | 682 | +358 | 2.10x | 1 | hot spread 1%  |
| 5 | 692 (680) | 695 (681) | +0.4 | 693 | 709 | 701 | -1.1 | 705 | +12 | 1.02x | 4 | hot spread 5%  |
| 6 | 382 (380) | 381 (380) | -0.2 | 381 | 380 | 380 | +0.1 | 380 | -2 | 1.00x | 0 | hot spread 1%  |
| 7 | 749 (744) | 751 (749) | +0.3 | 750 | 967 | 939 | -2.9 | 954 | +204 | 1.27x | 5 | hot spread 4%  |
| 8 | 938 (936) | 942 (930) | +0.5 | 940 | 952 | 947 | -0.6 | 950 | +10 | 1.01x | 5 | hot spread 10%  |
| 9 | 1407 (1400) | 1407 (1404) | +0.0 | 1407 | 1549 | 1560 | +0.7 | 1555 | +148 | 1.10x | 2 | hot spread 2%  |
| 10 | 864 (856) | 841 (837) | -2.7 | 851 | 1155 | 1137 | -1.6 | 1146 | +295 | 1.35x | 2 | hot spread 12%  |
| 11 | 179 (173) | 179 (173) | +0.5 | 179 | 175 | 180 | +2.8 | 177 | -2 | 0.99x | 4 | hot spread 14%  |
| 12 | 703 (701) | 702 (700) | -0.1 | 702 | 739 | 739 | +0.0 | 739 | +37 | 1.05x | 1 | hot spread 1%  |
| 13 | 1325 (1314) | 1305 (1288) | -1.6 | 1320 | 1328 | 1309 | -1.4 | 1318 | -1 | 1.00x | 0 | hot spread 7%  |
| 14 | 697 (687) | 696 (678) | -0.1 | 696 | 708 | 706 | -0.3 | 707 | +11 | 1.02x | 0 | hot spread 14%  |
| 15 | 529 (527) | 528 (526) | -0.2 | 529 | 528 | 529 | +0.3 | 528 | -0 | 1.00x | 0 | hot spread 2%  |
| 16 | 313 (305) | 317 (314) | +1.2 | 315 | 309 | 301 | -2.7 | 309 | -6 | 0.98x | 1 | hot spread 11%  |
| 17 | 497 (496) | 497 (495) | -0.0 | 497 | 531 | 530 | -0.2 | 531 | +34 | 1.07x | 1 | hot spread 2%  |
| 18 | 1095 (1079) | 1091 (1082) | -0.3 | 1092 | 1101 | 1125 | +2.2 | 1108 | +16 | 1.02x | 2 | hot spread 2%  |
| 19 | 891 (876) | 888 (877) | -0.4 | 889 | 888 | 894 | +0.7 | 891 | +2 | 1.00x | 1 | hot spread 2%  |
| 20 | 720 (710) | 721 (710) | +0.2 | 720 | 717 | 714 | -0.4 | 715 | -5 | 0.99x | 5 | hot spread 4%  |
| 21 | 1453 (1448) | 1458 (1443) | +0.3 | 1455 | 1597 | 1568 | -1.8 | 1569 | +113 | 1.08x | 4 | hot spread 1%  |
| 22 | 340 (339) | 340 (339) | -0.0 | 340 | 387 | 389 | +0.6 | 388 | +48 | 1.14x | 1 | hot spread 3%  |

Queries complete: 22/22
Totals (sum of per-query medians, s): tip A 15.388, tip B 15.352 (A/A -0.23%), tip(6 hot) 15.372; DF-off A 16.685, DF-off B 16.602 (A/A -0.49%), DF-off(6 hot) 16.627; off/tip 1.082x
Geomean (ms): tip A 596.5, tip B 595.2 (A/A -0.21%), tip 596.0; DF-off A 643.6, DF-off B 641.4, DF-off 642.4; geomean off/tip 1.078x

Reading the table:
- A/A noise: suite total -0.23% (tip) / -0.49% (DF-off); geomean -0.21%. Per query |A/A| <= 0.5% for 16 queries; worst q10 -2.7%,
  q2 -2.2%, q13 -1.6%, q16 +1.2%. DF-off A/A worst: q7 -2.9%, q11 +2.8%, q16 -2.7%, q18 +2.2%. Treat +-3% (or +-10 ms) as noise per query.
- **DF net value, suite: +1.255 s of 16.627 s (DF-off total / tip total = 1.082x; geomean 1.078x).**
- DF clearly helps (> noise): q4 +358 ms (2.10x), q10 +295 (1.35x), q7 +204 (1.27x), q9 +148 (1.10x), q21 +113 (1.08x),
  q22 +48 (1.14x), q12 +37 (1.05x), q17 +34 (1.07x).
- DF net value within noise (benefit ~= cost): q3 +1 ms (2 Blooms, 338 MB, 61 ms HOST-staged publication, lineitem probe kept 8%),
  q5 +12, q8 +10, q18 +16, q19 +2, q14 +11 (no DF: pure noise).
- DF net value <= 0: **q2 -8 ms (-5%)**, q16 -6 (-2%), q20 -5 (-0.7%), q11 -2 (-1%); q1/q6/q13/q15 have no DF (-7..0 = noise).
  Upper bound on total DF cost in these: q2 <= ~8 ms, q16 <= ~6 ms, q20 <= ~5 ms, q11 <= ~2 ms, all at or inside A/A noise.
- HOST-staged publication latency (section 1.2) as a share of the tip median: q21 343/1455 = 24%; q3 61/666 = 9%; q7 67/750 = 9%;
  q4 21/324 = 6%; q10 39/851 = 5%; q8 40/940 = 4%; q5 21/693 = 3%; q9 35/1407 = 2.5%; q16 8/315 = 3%; q22 7/340 = 2%; q12 11/702 = 2%.
  Suite: 654 ms / 15.37 s = 4.3%. It is a latency inside one build task, not proven to be on the critical path, and it is the
  excluded mechanism: reported, not to be optimized or attributed to kernels. Non-HOST publication latency: 19 ms / suite = 0.12%.

Quent sets for the next stage: `$R/telemetry/baseline_tip/q<N>/<session-uuid>/` (tip A, one session dir per query, 4 queries
inside: iter0 warm + 3 hot, unlabeled, in order). Also `baseline_tip_b`, `baseline_off`, `baseline_off_b`, `logpass` (trace run), `smoke`.
Raw timings: `$R/runs/baseline/<label>/tpch_*_q<N>/csv/runtimes.csv`; summary `$R/runs/baseline/timing_summary.json`.

## 3. nsys shortlist (in priority order)

| q | why | what to read in the capture |
|---|---|---|
| 21 | largest Bloom of the suite: j18 730.8M rows -> 1.39 GB, hash path, 5 batches, 317 ms "after final" (24% of 1455 ms); second Bloom 133 MB forward; PARTITION id=11 473 ms summed | split of 317 ms into scatter / `accum::finish` HOST legs / rest; `add_if_n` time per batch at a 1.4 GB array (H04); lease + memset 4 x 1.4 GB (H07); candidate for H02 (duplicates, bits/key) |
| 3 | DF net value ~0 although the probe keeps 8%; cleanest forward multi-batch Bloom (j15: 147M rows, 280 MB, 4 batches / 4 GPUs, 57 ms) + single-batch forward Bloom (j8, 4.1 ms) | forward-path contribute cost (~5 ms per 37M-row batch in trace) vs finish; how much of the 57 ms is HOST bounce; is the lineitem scan waiting on publication |
| 7 | 278 MB hash-path Bloom (j35, 63 ms incl. scatter) + 23 MB single-batch hash-path Bloom + 3 small forward builds; "1 of 2 probes ran" on the consumer | hash-path insert ahead of scatter (H04/H09), publication ordering |
| 9 | H05 hit: 2-key join builds 2 x 83 MB, one keeps 98.9%; DYNAMIC_FILTER id=26 468 ms summed task time; DF value +148 ms | cost of the useless key (insert + finish share), probe-side time |
| 10 | highest value among hash-path builds; 2 hash-path Blooms with 7 and 4 batches, one with a known domain; A/A noisiest query (-2.7%) | hash-path contribute + scatter per batch |
| 8, 5 | most constructions (5 / 4), DF net value ~+1% (inside noise): cost ~= benefit; forward small builds + 174 MB / 87 MB Blooms | fixed cost per small forward construction (H03): begin, 4-GPU leases, replicate |
| 2 | DF net value <= 0 (-5%, 150 ms query); 5 small constructions incl. a 2M-key hash IN-list (3.2 ms) and a timing-dependent skipped 2-key attempt (j42) | H03 fixed costs; what the skipped attempt did before it was skipped |
| 4 | BUILD_FILTER site (not PARTITION): 10 batches, 109 MB, 21 ms publication; best DF value (2.10x) | confirm the BUILD_FILTER path shares the accumulator mechanics; per-batch contribute 0.4-1.1 ms |
| 12 | 44-batch Bloom build (max batch count), 59 MB | per-batch overhead (H08 control); expected negligible |
| 20 | 5 constructions of mixed kinds incl. `1p hash` BUILD_FILTER, 2-target publication, one skipped attempt; net <= 0 (noise) | optional |

Not worth a capture: q11, q17, q18, q19 (exact IN-lists only, <= 2.2 ms publication total), q16/q22 (one mid-size forward Bloom each;
q3/q5 cover the shape), q1/q6/q13/q14/q15 (no DF).

## 4. Not verified / caveats

- Base-vs-tip key-outcome parity (H01 "coverage parity") is not done here: only the tip was run.
- Consumer gate decisions are inferred from keep ratios, not logged; which q9 key is the useless one is inferred.
- Distinct-key counts of Bloom builds are not logged, so real oversizing from duplicates (H02a beyond the domain cap) is open.
- Join attribution of `started` / `published` lines is positional (section 1 caveat); log pass times come from a TRACE run.
- Publication "ms after final" on hash-path rows includes one scatter; none of the latencies are proven critical-path.
- Timing: 3 hot iterations per run x 2 runs per variant; no thermal/clock log beyond `q<N>.gpu.txt` snapshots taken before each run.
- One process per query (LOAD + pin per query) rather than one process for the 22-query block; DF-off runs carry
  `SIRIUS_ENABLE_TEST_OPTIONS=1`, tip runs do not (registers options only).
