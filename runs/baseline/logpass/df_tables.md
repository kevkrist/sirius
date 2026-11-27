
### q1
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=0 hot window 18:36:12.513 -> 18:36:12.984
no hash-join partition strategy / DF key lines
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=0: n=71 survivors 3157113029/5999989709
PARTITION operators (trace): id=5 tasks=71 rows_in=284 gpus=4 sum_ms=0.7 max_ms=0.0
iter0 vs hot: constructions 0 vs 0, key lines 0 vs 0, key outcomes identical=True

### q2
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=5 hot window 18:36:42.397 -> 18:36:42.564

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 7 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 1 | 4B | 1k/1b/4G | c0 small_in_list 1B 1/1 dom=5(pinned_parquet_exact,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 0.2ms, no | id=1 1t 0.2 0.2 |
| 15 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 5 | 104B | 1k/1b/4G | c0 small_in_list 5B 5/5 dom=0(unknown,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 0.2ms, no | id=5 1t 0.0 0.0; id=9 1t 0.2 0.2 |
| 28 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 1999643 | 298.5MB | 1k/1b/4G | c0 hash_in_list 1.9MB 1999643/1999643 dom=0(unknown,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 3.2ms, no | id=13 1t 0.0 0.0; id=16 1t 3.2 3.2 |
| 25 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 799927 | 16.8MB | 1k/3b/4G | c0 hash_in_list 781.2KB 799927/799927 dom=200000000(pinned_parquet_exact,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 1.5ms, no | id=19 3t 1.6 1.6 |
| 42 | INNER/MIXED_JOIN | 4p hash | 638799 | 113.7MB | 2k/4b/4G | c0 none 0B 638799/638799 dom=0(unknown,keep) bits=- pushed=0 zone=0 reason=attempt_skip_publications_skipped_targets_drained<br>c1 none 0B 638799/638799 dom=0(unknown,keep) bits=- pushed=0 zone=0 reason=attempt_skip_publications_skipped_targets_drained | - | - | id=32 4t 18.3 5.1 |

DYNAMIC_FILTER operators (trace): id=4 [GPU_SCAN (id=3) -> DYNAMIC_FILTER (id=4)] tasks=1 rows 25->5 keep=0.200 ms=0.3; id=12 [GPU_SCAN (id=11) -> DYNAMIC_FILTER (id=12)] tasks=1 rows 10000000->1999643 keep=0.200 ms=2.0; id=22 [GPU_SCAN (id=21) -> DYNAMIC_FILTER (id=22)] tasks=4 rows 3199708->3199708 keep=1.000 ms=0.5; id=34 [DYNAMIC_FILTER (id=34) -> PROJECTION (id=35) -> HASH_GROUP_BY (id=36)] tasks=4 rows 638799->638799 keep=1.000 ms=0.1; id=39 [MERGE_GROUP_BY (id=38) -> DYNAMIC_FILTER (id=39)] tasks=1 rows 471301->471301 keep=1.000 ms=0.0
Compressed-scan join-filter probes (ran of requested, complete): 1/1 complete=true: chunks=1 kept 1999643/10000000 = 0.2000; 1/2 complete=false: chunks=4 kept 3199708/800000000 = 0.0040
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=0: n=3 survivors 4000594/200000000; APP,jf=2: n=4 survivors 3199708/800000000; NOT,jf=1: n=1
PARTITION operators (trace): id=1 tasks=1 rows_in=1 gpus=1 sum_ms=0.2 max_ms=0.2; id=5 tasks=1 rows_in=5 gpus=1 sum_ms=0.0 max_ms=0.0; id=9 tasks=1 rows_in=5 gpus=1 sum_ms=0.2 max_ms=0.2; id=13 tasks=1 rows_in=1999643 gpus=1 sum_ms=0.0 max_ms=0.0; id=16 tasks=1 rows_in=1999643 gpus=1 sum_ms=3.2 max_ms=3.2; id=19 tasks=3 rows_in=799927 gpus=3 sum_ms=1.6 max_ms=1.6; id=23 tasks=4 rows_in=3199708 gpus=4 sum_ms=0.0 max_ms=0.0; id=26 tasks=4 rows_in=3199708 gpus=4 sum_ms=0.0 max_ms=0.0; id=32 tasks=4 rows_in=638799 gpus=4 sum_ms=18.3 max_ms=5.1; id=37 tasks=4 rows_in=471301 gpus=4 sum_ms=0.0 max_ms=0.0; id=40 tasks=1 rows_in=471301 gpus=1 sum_ms=0.4 max_ms=0.4
iter0 vs hot: constructions 5 vs 5, key lines 6 vs 6, key outcomes identical=False

### q3
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=2 hot window 18:37:30.407 -> 18:37:31.231

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 8 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 30003569 | 114.5MB | 1k/1b/4G | c0 bloom 57.2MB 30003569/30003569 dom=150000000(pinned_parquet_exact,keep) bits=16.0 pushed=1 zone=0 reason=- | 57.2MB root1 0/3 HOST staging | 1->1, 4.1ms, HOST | id=2 1t 4.2 4.2 |
| 15 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 147060214 | 2.1GB | 1k/4b/4G | c0 bloom 280.5MB 147060214/147060214 dom=0(unknown,keep) bits=16.0 pushed=1 zone=0 reason=- | 280.5MB root3 3/3 HOST staging | 1->1, 57.0ms, HOST | id=9 4t 72.7 57.0 |

DYNAMIC_FILTER operators (trace): id=5 [GPU_SCAN (id=4) -> DYNAMIC_FILTER (id=5)] tasks=8 rows 147834473->147834473 keep=1.000 ms=6.1; id=12 [GPU_SCAN (id=11) -> DYNAMIC_FILTER (id=12)] tasks=44 rows 34116680->34116680 keep=1.000 ms=10.9
Compressed-scan join-filter probes (ran of requested, complete): 1/1 complete=true: chunks=24 kept 160514096/1927817321 = 0.0833
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=1: n=52 survivors 181951153/7499989709; NOT,jf=0: n=1
PARTITION operators (trace): id=2 tasks=1 rows_in=30003569 gpus=1 sum_ms=4.2 max_ms=4.2; id=6 tasks=8 rows_in=147834473 gpus=4 sum_ms=0.1 max_ms=0.0; id=9 tasks=4 rows_in=147060214 gpus=4 sum_ms=72.7 max_ms=57.0; id=13 tasks=44 rows_in=34116680 gpus=4 sum_ms=0.3 max_ms=0.0; id=18 tasks=4 rows_in=11327255 gpus=4 sum_ms=3.3 max_ms=0.9
iter0 vs hot: constructions 2 vs 2, key lines 2 vs 2, key outcomes identical=True

### q4
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=1 hot window 18:38:53.457 -> 18:38:53.783

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 9 | RIGHT_SEMI/STANDARD | 4p hash | 149850442 | 1.1GB | 1k/10b/4G | c0 bloom 109.4MB 57356572/57356572 dom=? bits=16.0 pushed=1 zone=0 reason=- | 109.4MB root3 3/3 HOST staging | 1->1, 20.9ms, HOST | id=7 25t 30.1 2.0 |

DYNAMIC_FILTER operators (trace): id=5 [GPU_SCAN (id=4) -> DYNAMIC_FILTER (id=5) -> FILTER (id=6)] tasks=25 rows 237012238->237012238 keep=1.000 ms=10.8
Compressed-scan join-filter probes (ran of requested, complete): 1/1 complete=true: chunks=16 kept 153973596/3897884974 = 0.0395
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=0: n=10 survivors 57356572/1500000000; APP,jf=1: n=25 survivors 237012238/5999989709
PARTITION operators (trace): id=2 tasks=10 rows_in=57356572 gpus=4 sum_ms=27.6 max_ms=3.2; id=7 tasks=25 rows_in=149850442 gpus=4 sum_ms=30.1 max_ms=2.0; id=11 tasks=4 rows_in=20 gpus=4 sum_ms=0.0 max_ms=0.0
iter0 vs hot: constructions 1 vs 1, key lines 1 vs 1, key outcomes identical=True

### q5
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=4 hot window 18:39:41.816 -> 18:39:42.511

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 35 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 10000000 | 76.3MB | - | - | - | - | id=1 1t 0.0 0.0 |
| 10 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 1 | 4B | 1k/1b/4G | c0 small_in_list 1B 1/1 dom=5(pinned_parquet_exact,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 0.2ms, no | id=4 1t 0.3 0.3 |
| 18 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 5 | 104B | 1k/1b/4G | c0 small_in_list 5B 5/5 dom=0(unknown,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 0.2ms, no | id=8 1t 0.0 0.0; id=12 1t 0.3 0.3 |
| 25 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 29997971 | 572.2MB | 1k/1b/4G | c0 bloom 57.2MB 29997971/29997971 dom=0(unknown,keep) bits=16.0 pushed=1 zone=0 reason=- | 57.2MB root2 0/3 HOST staging | 1->1, 4.1ms, HOST | id=16 1t 0.0 0.0; id=19 1t 4.2 4.2 |
| 32 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 45508350 | 1.0GB | 1k/4b/4G | c0 bloom 86.8MB 45508350/45508350 dom=0(unknown,keep) bits=16.0 pushed=1 zone=0 reason=- | 86.8MB root3 3/3 HOST staging | 1->1, 16.7ms, HOST | id=26 4t 18.1 16.7 |

DYNAMIC_FILTER operators (trace): id=7 [GPU_SCAN (id=6) -> DYNAMIC_FILTER (id=7)] tasks=1 rows 25->5 keep=0.200 ms=0.3; id=15 [GPU_SCAN (id=14) -> DYNAMIC_FILTER (id=15)] tasks=1 rows 150000000->29997971 keep=0.200 ms=1.9; id=22 [GPU_SCAN (id=21) -> DYNAMIC_FILTER (id=22)] tasks=7 rows 45747377->45747377 keep=1.000 ms=2.4; id=29 [GPU_SCAN (id=28) -> DYNAMIC_FILTER (id=29)] tasks=44 rows 189710493->189710493 keep=1.000 ms=14.0
Compressed-scan join-filter probes (ran of requested, complete): 1/1 complete=true: chunks=24 kept 146228618/2607352286 = 0.0561
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=1: n=51 survivors 235457870/7499989709; NOT,jf=1: n=1
PARTITION operators (trace): id=1 tasks=1 rows_in=10000000 gpus=1 sum_ms=0.0 max_ms=0.0; id=4 tasks=1 rows_in=1 gpus=1 sum_ms=0.3 max_ms=0.3; id=8 tasks=1 rows_in=5 gpus=1 sum_ms=0.0 max_ms=0.0; id=12 tasks=1 rows_in=5 gpus=1 sum_ms=0.3 max_ms=0.3; id=16 tasks=1 rows_in=29997971 gpus=1 sum_ms=0.0 max_ms=0.0; id=19 tasks=1 rows_in=29997971 gpus=1 sum_ms=4.2 max_ms=4.2; id=23 tasks=7 rows_in=45747377 gpus=4 sum_ms=0.1 max_ms=0.0; id=26 tasks=4 rows_in=45508350 gpus=4 sum_ms=18.1 max_ms=16.7; id=30 tasks=44 rows_in=189710493 gpus=4 sum_ms=0.5 max_ms=0.0; id=33 tasks=4 rows_in=182053050 gpus=4 sum_ms=0.0 max_ms=0.0; id=38 tasks=4 rows_in=20 gpus=4 sum_ms=0.1 max_ms=0.0
iter0 vs hot: constructions 4 vs 4, key lines 4 vs 4, key outcomes identical=True

### q6
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=0 hot window 18:40:41.794 -> 18:40:42.174
no hash-join partition strategy / DF key lines
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=0: n=44 survivors 114177066/5999989709
iter0 vs hot: constructions 0 vs 0, key lines 0 vs 0, key outcomes identical=True

### q7
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=5 hot window 18:41:32.724 -> 18:41:33.492

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 9 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 2 | 38B | 1k/1b/4G | c0 small_in_list 2B 2/2 dom=25(pinned_parquet_exact,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 0.2ms, no | id=3 1t 0.3 0.3; id=15 1t 0.3 0.3 |
| 38 | INNER/BUILD_PROBE | 4p hash | 12000338 | 194.6MB | 1k/1b/4G | c0 bloom 22.9MB 12000338/12000338 dom=0(unknown,keep) bits=16.0 pushed=1 zone=0 reason=- | 22.9MB root0 0/3 HOST staging | 1->1, 3.7ms, HOST | id=7 1t 0.0 0.0; id=10 1t 3.8 3.8 |
| 21 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 2 | 38B | 1k/1b/4G | c0 small_in_list 2B 2/2 dom=25(pinned_parquet_exact,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 0.2ms, no | id=3 1t 0.3 0.3; id=15 1t 0.3 0.3 |
| 28 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 800280 | 13.0MB | 1k/1b/4G | c0 hash_in_list 781.5KB 800280/800280 dom=0(unknown,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 1.5ms, no | id=19 1t 0.0 0.0; id=22 1t 1.5 1.5 |
| 35 | INNER/BUILD_PROBE | 4p hash | 145884740 | 4.2GB | 1k/4b/4G | c0 bloom 278.3MB 145884740/145884740 dom=0(unknown,keep) bits=16.0 pushed=1 zone=0 reason=- | 278.3MB root2 3/3 HOST staging | 1->1, 63.0ms, HOST | id=26 50t 0.5 0.0; id=29 4t 99.3 63.1 |

DYNAMIC_FILTER operators (trace): id=6 [GPU_SCAN (id=5) -> DYNAMIC_FILTER (id=6)] tasks=1 rows 12000338->12000338 keep=1.000 ms=0.3; id=18 [GPU_SCAN (id=17) -> DYNAMIC_FILTER (id=18)] tasks=1 rows 800280->800280 keep=1.000 ms=0.2; id=25 [GPU_SCAN (id=24) -> DYNAMIC_FILTER (id=25)] tasks=50 rows 145884740->145884740 keep=1.000 ms=56.5; id=32 [GPU_SCAN (id=31) -> DYNAMIC_FILTER (id=32)] tasks=5 rows 121814490->10284847 keep=0.084 ms=8.4
Compressed-scan join-filter probes (ran of requested, complete): 1/1 complete=true: chunks=18 kept 60396902/754690081 = 0.0800; 1/2 complete=false: chunks=5 kept 121814490/1500000000 = 0.0812
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=1: n=52 survivors 158685358/6159989709; APP,jf=2: n=5 survivors 121814490/1500000000
PARTITION operators (trace): id=3 tasks=1 rows_in=2 gpus=1 sum_ms=0.3 max_ms=0.3; id=7 tasks=1 rows_in=12000338 gpus=1 sum_ms=0.0 max_ms=0.0; id=10 tasks=1 rows_in=12000338 gpus=1 sum_ms=3.8 max_ms=3.8; id=15 tasks=1 rows_in=2 gpus=1 sum_ms=0.3 max_ms=0.3; id=19 tasks=1 rows_in=800280 gpus=1 sum_ms=0.0 max_ms=0.0; id=22 tasks=1 rows_in=800280 gpus=1 sum_ms=1.5 max_ms=1.5; id=26 tasks=50 rows_in=145884740 gpus=4 sum_ms=0.5 max_ms=0.0; id=29 tasks=4 rows_in=145884740 gpus=4 sum_ms=99.3 max_ms=63.1; id=33 tasks=5 rows_in=10284847 gpus=4 sum_ms=2.1 max_ms=0.5; id=36 tasks=4 rows_in=11842566 gpus=4 sum_ms=4.8 max_ms=1.3; id=42 tasks=4 rows_in=16 gpus=4 sum_ms=0.1 max_ms=0.0
iter0 vs hot: constructions 5 vs 5, key lines 5 vs 5, key outcomes identical=True

### q8
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=5 hot window 18:43:10.402 -> 18:43:11.344

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 6 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 25 | 485B | - | - | - | - | id=1 1t 0.0 0.0 |
| 47 | INNER/BUILD_PROBE | 4p hash | 10000000 | 143.8MB | - | - | - | - | id=4 1t 0.0 0.0; id=7 1t 1.5 1.5 |
| 44 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 1332415 | 5.1MB | 1k/2b/4G | c0 hash_in_list 1.3MB 1332415/1332415 dom=200000000(pinned_parquet_exact,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 2.3ms, no | id=10 2t 2.4 2.4 |
| 19 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 1 | 4B | 1k/1b/4G | c0 small_in_list 1B 1/1 dom=5(pinned_parquet_exact,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 0.2ms, no | id=13 1t 0.3 0.3 |
| 27 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 5 | 40B | 1k/1b/4G | c0 small_in_list 5B 5/5 dom=0(unknown,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 0.2ms, no | id=17 1t 0.0 0.0; id=21 1t 0.3 0.3 |
| 34 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 30002171 | 343.3MB | 1k/1b/4G | c0 bloom 57.2MB 30002171/30002171 dom=0(unknown,keep) bits=16.0 pushed=1 zone=0 reason=- | 57.2MB root2 0/3 HOST staging | 1->1, 4.1ms, HOST | id=25 1t 0.0 0.0; id=28 1t 4.1 4.1 |
| 41 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 91139462 | 1.9GB | 1k/4b/4G | c0 bloom 173.8MB 91139462/91139462 dom=0(unknown,keep) bits=16.0 pushed=1 zone=0 reason=- | 173.8MB root3 3/3 HOST staging | 1->1, 35.4ms, HOST | id=35 4t 42.6 35.4 |

DYNAMIC_FILTER operators (trace): id=16 [GPU_SCAN (id=15) -> DYNAMIC_FILTER (id=16)] tasks=1 rows 25->5 keep=0.200 ms=0.1; id=24 [GPU_SCAN (id=23) -> DYNAMIC_FILTER (id=24)] tasks=1 rows 150000000->30002171 keep=0.200 ms=1.9; id=31 [GPU_SCAN (id=30) -> DYNAMIC_FILTER (id=31)] tasks=7 rows 91618588->91618588 keep=1.000 ms=3.9; id=38 [GPU_SCAN (id=37) -> DYNAMIC_FILTER (id=38)] tasks=50 rows 39957901->39957901 keep=1.000 ms=69.4
Compressed-scan join-filter probes (ran of requested, complete): 1/1 complete=true: chunks=8 kept 121620759/605715531 = 0.2008; 1/2 complete=false: chunks=50 kept 39957901/5999989709 = 0.0067
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=0: n=2 survivors 1332415/200000000; APP,jf=1: n=7 survivors 91618588/1500000000; APP,jf=2: n=50 survivors 39957901/5999989709; NOT,jf=1: n=1
PARTITION operators (trace): id=1 tasks=1 rows_in=25 gpus=1 sum_ms=0.0 max_ms=0.0; id=4 tasks=1 rows_in=10000000 gpus=1 sum_ms=0.0 max_ms=0.0; id=7 tasks=1 rows_in=10000000 gpus=1 sum_ms=1.5 max_ms=1.5; id=10 tasks=2 rows_in=1332415 gpus=2 sum_ms=2.4 max_ms=2.4; id=13 tasks=1 rows_in=1 gpus=1 sum_ms=0.3 max_ms=0.3; id=17 tasks=1 rows_in=5 gpus=1 sum_ms=0.0 max_ms=0.0; id=21 tasks=1 rows_in=5 gpus=1 sum_ms=0.3 max_ms=0.3; id=25 tasks=1 rows_in=30002171 gpus=1 sum_ms=0.0 max_ms=0.0; id=28 tasks=1 rows_in=30002171 gpus=1 sum_ms=4.1 max_ms=4.1; id=32 tasks=7 rows_in=91618588 gpus=4 sum_ms=0.1 max_ms=0.0; id=35 tasks=4 rows_in=91139462 gpus=4 sum_ms=42.6 max_ms=35.4; id=39 tasks=50 rows_in=39957901 gpus=4 sum_ms=0.5 max_ms=0.0; id=42 tasks=4 rows_in=2430345 gpus=4 sum_ms=0.0 max_ms=0.0; id=45 tasks=4 rows_in=2430345 gpus=4 sum_ms=2.6 max_ms=0.9; id=51 tasks=4 rows_in=8 gpus=4 sum_ms=0.1 max_ms=0.0
iter0 vs hot: constructions 5 vs 5, key lines 5 vs 5, key outcomes identical=True

### q9
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=2 hot window 18:45:28.073 -> 18:45:29.524

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 32 | INNER/BUILD_PROBE | 4p hash | 1500000000 | 14.0GB | - | - | - | - | id=1 5t 105.0 27.0 |
| 9 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 25 | 485B | - | - | - | - | id=4 1t 0.0 0.0 |
| 22 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 10000000 | 143.8MB | - | - | - | - | id=7 1t 0.0 0.0; id=10 1t 0.0 0.0 |
| 19 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 10870459 | 41.5MB | 1k/3b/4G | c0 bloom 20.7MB 10870459/10870459 dom=200000000(pinned_parquet_exact,keep) bits=16.0 pushed=1 zone=0 reason=- | 20.7MB root1 2/3 HOST staging | 1->1, 3.6ms, HOST | id=13 3t 3.9 3.6 |
| 29 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 43481836 | 957.1MB | 2k/4b/4G | c0 bloom 82.9MB 43481836/43481836 dom=0(unknown,keep) bits=16.0 pushed=1 zone=0 reason=-<br>c1 bloom 82.9MB 43481836/43481836 dom=0(unknown,keep) bits=16.0 pushed=1 zone=0 reason=- | 165.9MB root1 3/3 HOST staging | 2->1, 31.8ms, HOST | id=20 4t 0.0 0.0; id=23 4t 35.2 31.8 |

DYNAMIC_FILTER operators (trace): id=16 [GPU_SCAN (id=15) -> DYNAMIC_FILTER (id=16)] tasks=4 rows 44477640->44477640 keep=1.000 ms=1.4; id=26 [GPU_SCAN (id=25) -> DYNAMIC_FILTER (id=26)] tasks=62 rows 5999989709->326123049 keep=0.054 ms=458.9
Compressed-scan join-filter probes (ran of requested, complete): 1/1 complete=true: chunks=4 kept 44477640/800000000 = 0.0556; 1/2 complete=false: chunks=16 kept 1548070146/1565969860 = 0.9886
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=1: n=4 survivors 44477640/800000000; NOT,jf=2: n=16
PARTITION operators (trace): id=1 tasks=5 rows_in=1500000000 gpus=4 sum_ms=105.0 max_ms=27.0; id=4 tasks=1 rows_in=25 gpus=1 sum_ms=0.0 max_ms=0.0; id=7 tasks=1 rows_in=10000000 gpus=1 sum_ms=0.0 max_ms=0.0; id=10 tasks=1 rows_in=10000000 gpus=1 sum_ms=0.0 max_ms=0.0; id=13 tasks=3 rows_in=10870459 gpus=3 sum_ms=3.9 max_ms=3.6; id=17 tasks=4 rows_in=44477640 gpus=4 sum_ms=0.0 max_ms=0.0; id=20 tasks=4 rows_in=43481836 gpus=4 sum_ms=0.0 max_ms=0.0; id=23 tasks=4 rows_in=43481836 gpus=4 sum_ms=35.2 max_ms=31.8; id=27 tasks=62 rows_in=326123049 gpus=4 sum_ms=0.6 max_ms=0.0; id=30 tasks=12 rows_in=326116789 gpus=4 sum_ms=221.4 max_ms=37.3; id=35 tasks=16 rows_in=2800 gpus=4 sum_ms=0.2 max_ms=0.0
iter0 vs hot: constructions 2 vs 2, key lines 3 vs 3, key outcomes identical=True

### q10
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=2 hot window 18:49:12.194 -> 18:49:13.047

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 25 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 25 | 485B | - | - | - | - | id=1 1t 0.0 0.0 |
| 10 | INNER/BUILD_PROBE | 4p hash | 57360614 | 656.4MB | 1k/7b/4G | c0 bloom 109.4MB 57360614/57360614 dom=1500000000(pinned_parquet_exact,keep) bits=16.0 pushed=1 zone=0 reason=- | 109.4MB root2 3/3 HOST staging | 1->1, 22.5ms, HOST | id=4 7t 32.9 22.5 |
| 21 | INNER/BUILD_PROBE | 4p hash | 44529913 | 849.3MB | 1k/4b/4G | c0 bloom 84.9MB 44529913/44529913 dom=0(unknown,keep) bits=16.0 pushed=1 zone=0 reason=- | 84.9MB root3 3/3 HOST staging | 1->1, 17.3ms, HOST | id=15 4t 21.8 17.4; id=23 8t 0.1 0.0; id=28 8t 135.4 17.2 |

DYNAMIC_FILTER operators (trace): id=7 [GPU_SCAN (id=6) -> DYNAMIC_FILTER (id=7)] tasks=45 rows 116507446->116507446 keep=1.000 ms=0.6; id=18 [GPU_SCAN (id=17) -> DYNAMIC_FILTER (id=18)] tasks=7 rows 44668492->44668492 keep=1.000 ms=0.1
Compressed-scan join-filter probes (ran of requested, complete): 1/1 complete=true: chunks=23 kept 86644247/683433385 = 0.1268
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=0: n=7 survivors 57360614/1500000000; APP,jf=1: n=45 survivors 116507446/5999989709; NOT,jf=1: n=7
Scan cached checkpoints with folded DFs: n=52 rows 266507446->161175938
PARTITION operators (trace): id=1 tasks=1 rows_in=25 gpus=1 sum_ms=0.0 max_ms=0.0; id=4 tasks=7 rows_in=57360614 gpus=4 sum_ms=32.9 max_ms=22.5; id=8 tasks=45 rows_in=116507446 gpus=4 sum_ms=33.9 max_ms=3.2; id=13 tasks=4 rows_in=48052555 gpus=4 sum_ms=5.4 max_ms=1.4; id=15 tasks=4 rows_in=44529913 gpus=4 sum_ms=21.8 max_ms=17.4; id=19 tasks=7 rows_in=44668492 gpus=4 sum_ms=83.0 max_ms=21.6; id=23 tasks=8 rows_in=44529913 gpus=4 sum_ms=0.1 max_ms=0.0; id=28 tasks=8 rows_in=44529913 gpus=4 sum_ms=135.4 max_ms=17.2
iter0 vs hot: constructions 2 vs 2, key lines 2 vs 2, key outcomes identical=True

### q11
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=4 hot window 18:52:42.362 -> 18:52:42.556

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 7 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 1 | 4B | 1k/1b/4G | c0 small_in_list 1B 1/1 dom=25(pinned_parquet_exact,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 0.2ms, no | id=1 1t 0.4 0.4; id=19 1t 0.0 0.0; id=22 1t 0.2 0.2 |
| 14 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 399948 | 4.6MB | 1k/1b/4G | c0 hash_in_list 390.6KB 399948/399948 dom=0(unknown,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 0.9ms, no | id=5 1t 0.0 0.0; id=8 1t 0.9 0.9; id=26 1t 0.0 0.0; id=29 1t 0.9 0.9 |
| 28 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 1 | 4B | 1k/1b/4G | c0 small_in_list 1B 1/1 dom=25(pinned_parquet_exact,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 0.2ms, no | id=1 1t 0.4 0.4; id=19 1t 0.0 0.0; id=22 1t 0.2 0.2 |
| 35 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 399948 | 4.6MB | 1k/1b/4G | c0 hash_in_list 390.6KB 399948/399948 dom=0(unknown,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 0.8ms, no | id=5 1t 0.0 0.0; id=8 1t 0.9 0.9; id=26 1t 0.0 0.0; id=29 1t 0.9 0.9 |

DYNAMIC_FILTER operators (trace): id=4 [GPU_SCAN (id=3) -> DYNAMIC_FILTER (id=4)] tasks=1 rows 399948->399948 keep=1.000 ms=0.2; id=11 [GPU_SCAN (id=10) -> DYNAMIC_FILTER (id=11)] tasks=5 rows 31995840->31995840 keep=1.000 ms=1.3; id=25 [GPU_SCAN (id=24) -> DYNAMIC_FILTER (id=25)] tasks=1 rows 399948->399948 keep=1.000 ms=0.2; id=32 [GPU_SCAN (id=31) -> DYNAMIC_FILTER (id=32)] tasks=5 rows 31995840->31995840 keep=1.000 ms=1.6
Compressed-scan join-filter probes (ran of requested, complete): 1/1 complete=true: chunks=12 kept 64791576/1620000000 = 0.0400
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=1: n=12 survivors 64791576/1620000000
PARTITION operators (trace): id=1 tasks=1 rows_in=1 gpus=1 sum_ms=0.4 max_ms=0.4; id=5 tasks=1 rows_in=399948 gpus=1 sum_ms=0.0 max_ms=0.0; id=8 tasks=1 rows_in=399948 gpus=1 sum_ms=0.9 max_ms=0.9; id=12 tasks=5 rows_in=31995840 gpus=4 sum_ms=0.0 max_ms=0.0; id=19 tasks=1 rows_in=1 gpus=1 sum_ms=0.0 max_ms=0.0; id=22 tasks=1 rows_in=1 gpus=1 sum_ms=0.2 max_ms=0.2; id=26 tasks=1 rows_in=399948 gpus=1 sum_ms=0.0 max_ms=0.0; id=29 tasks=1 rows_in=399948 gpus=1 sum_ms=0.9 max_ms=0.9; id=33 tasks=5 rows_in=31995840 gpus=4 sum_ms=0.0 max_ms=0.0; id=38 tasks=4 rows_in=30129021 gpus=4 sum_ms=6.0 max_ms=1.6; id=41 tasks=4 rows_in=30129021 gpus=4 sum_ms=0.0 max_ms=0.0
iter0 vs hot: constructions 4 vs 4, key lines 4 vs 4, key outcomes identical=True

### q12
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=1 hot window 18:56:19.239 -> 18:56:20.125

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 8 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 31165956 | 535.0MB | 1k/44b/4G | c0 bloom 59.4MB 31165956/31165956 dom=5999989709(pinned_parquet_exact,keep) bits=16.0 pushed=1 zone=0 reason=- | 59.4MB root2 3/3 HOST staging | 1->1, 11.6ms, HOST | id=2 44t 13.3 11.6 |

DYNAMIC_FILTER operators (trace): id=5 [GPU_SCAN (id=4) -> DYNAMIC_FILTER (id=5)] tasks=8 rows 30639256->30639256 keep=1.000 ms=3.5
Compressed-scan join-filter probes (ran of requested, complete): 1/1 complete=true: chunks=8 kept 30639256/1500000000 = 0.0204
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=0: n=44 survivors 886317625/5999989709; APP,jf=1: n=8 survivors 30639256/1500000000
PARTITION operators (trace): id=2 tasks=44 rows_in=31165956 gpus=4 sum_ms=13.3 max_ms=11.6; id=6 tasks=8 rows_in=30639256 gpus=4 sum_ms=0.1 max_ms=0.0; id=11 tasks=4 rows_in=8 gpus=4 sum_ms=0.0 max_ms=0.0
iter0 vs hot: constructions 1 vs 1, key lines 1 vs 1, key outcomes identical=True

### q13
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=0 hot window 18:59:45.460 -> 18:59:46.739
no hash-join partition strategy / DF key lines
PARTITION operators (trace): id=1 tasks=1 rows_in=150000000 gpus=1 sum_ms=4.2 max_ms=4.2; id=3 tasks=25 rows_in=1483831048 gpus=4 sum_ms=282.7 max_ms=13.2; id=7 tasks=4 rows_in=120 gpus=4 sum_ms=0.0 max_ms=0.0
iter0 vs hot: constructions 0 vs 0, key lines 0 vs 0, key outcomes identical=True

### q14
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=0 hot window 19:03:45.702 -> 19:03:46.472

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 6 | INNER/BUILD_PROBE | 4p hash | 200000000 | 5.8GB | - | - | - | - | id=1 2t 48.5 36.1 |
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=0: n=37 survivors 77333931/5999989709
PARTITION operators (trace): id=1 tasks=2 rows_in=200000000 gpus=2 sum_ms=48.5 max_ms=36.1; id=4 tasks=37 rows_in=77333931 gpus=4 sum_ms=323.5 max_ms=93.1
iter0 vs hot: constructions 0 vs 0, key lines 0 vs 0, key outcomes identical=True

### q15
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=0 hot window 19:07:43.248 -> 19:07:43.785

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 13 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 1 | 16B | - | - | - | - | id=9 1t 0.0 0.0; id=14 4t 0.0 0.0 |
| 19 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 1 | 36B | - | - | - | - | id=9 1t 0.0 0.0; id=14 4t 0.0 0.0 |
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=0: n=37 survivors 229420935/5999989709
PARTITION operators (trace): id=3 tasks=37 rows_in=167196286 gpus=4 sum_ms=64.9 max_ms=2.6; id=9 tasks=1 rows_in=1 gpus=1 sum_ms=0.0 max_ms=0.0; id=11 tasks=4 rows_in=10000000 gpus=4 sum_ms=0.0 max_ms=0.0; id=14 tasks=4 rows_in=1 gpus=4 sum_ms=0.0 max_ms=0.0; id=17 tasks=1 rows_in=10000000 gpus=1 sum_ms=0.0 max_ms=0.0
iter0 vs hot: constructions 0 vs 0, key lines 0 vs 0, key outcomes identical=True

### q16
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=1 hot window 19:12:13.201 -> 19:12:13.542

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 14 | MARK/BUILD_PROBE | 4p broadcast [broadcast] | 4898 | 19.1KB | - | - | - | - | id=1 1t 0.0 0.0 |
| 11 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 29700215 | 1.2GB | 1k/3b/4G | c0 bloom 56.6MB 29700215/29700215 dom=200000000(pinned_parquet_exact,keep) bits=16.0 pushed=1 zone=0 reason=- | 56.6MB root0 2/3 HOST staging | 1->1, 8.4ms, HOST | id=5 3t 8.9 8.5 |

DYNAMIC_FILTER operators (trace): id=8 [GPU_SCAN (id=7) -> DYNAMIC_FILTER (id=8)] tasks=2 rows 800000000->119695852 keep=0.150 ms=7.4
Compressed-scan join-filter probes (ran of requested, complete): 1/1 complete=true: chunks=2 kept 119695852/800000000 = 0.1496
Compressed-scan decode outcomes (applied?, #join_filters): NOT,jf=1: n=2
PARTITION operators (trace): id=1 tasks=1 rows_in=4898 gpus=1 sum_ms=0.0 max_ms=0.0; id=5 tasks=3 rows_in=29700215 gpus=3 sum_ms=8.9 max_ms=8.5; id=9 tasks=2 rows_in=119695852 gpus=2 sum_ms=0.0 max_ms=0.0; id=12 tasks=2 rows_in=118800860 gpus=2 sum_ms=0.0 max_ms=0.0; id=19 tasks=2 rows_in=55680 gpus=2 sum_ms=9.6 max_ms=5.1
iter0 vs hot: constructions 1 vs 1, key lines 1 vs 1, key outcomes identical=True

### q17
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=1 hot window 19:16:51.877 -> 19:16:52.374

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 7 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 200585 | 783.5KB | 1k/2b/4G | c0 hash_in_list 195.9KB 200585/200585 dom=200000000(pinned_parquet_exact,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 0.6ms, no | id=1 2t 0.8 0.7; id=16 1t 0.0 0.0 |
| 21 | INNER/MIXED_JOIN | 1p hash | 200585 | 2.3MB | - | - | - | - | id=1 2t 0.8 0.7; id=16 1t 0.0 0.0 |

DYNAMIC_FILTER operators (trace): id=4 [GPU_SCAN (id=3) -> DYNAMIC_FILTER (id=4)] tasks=31 rows 6013896->6013896 keep=1.000 ms=12.8
Compressed-scan join-filter probes (ran of requested, complete): 1/1 complete=true: chunks=16 kept 3139534/3131945555 = 0.0010
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=0: n=2 survivors 200585/200000000; APP,jf=1: n=31 survivors 6013896/5999989709
PARTITION operators (trace): id=1 tasks=2 rows_in=200585 gpus=2 sum_ms=0.8 max_ms=0.7; id=5 tasks=31 rows_in=6013896 gpus=4 sum_ms=0.1 max_ms=0.0; id=13 tasks=4 rows_in=802198 gpus=4 sum_ms=0.0 max_ms=0.0; id=16 tasks=1 rows_in=200585 gpus=1 sum_ms=0.0 max_ms=0.0; id=19 tasks=4 rows_in=6013896 gpus=4 sum_ms=0.0 max_ms=0.0
iter0 vs hot: constructions 1 vs 1, key lines 1 vs 1, key outcomes identical=True

### q18
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=2 hot window 19:21:51.032 -> 19:21:52.122

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 17 | INNER/BUILD_PROBE | 4p hash | 150000000 | 4.2GB | - | - | - | - | id=1 1t 36.7 36.7 |
| 14 | SEMI/BUILD_PROBE | 4p broadcast [broadcast] | 63430 | 495.5KB | 1k/36b/4G | c0 hash_in_list 61.9KB 63430/63430 dom=5999989709(pinned_parquet_exact,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 1.0ms, no | id=8 36t 2.2 1.1; id=12 10t 0.1 0.0; id=15 4t 2.3 0.6; id=18 4t 0.6 0.5; id=28 4t 0.0 0.0 |
| 24 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 63430 | 2.4MB | 1k/4b/4G | c0 hash_in_list 61.9KB 63430/63430 dom=0(unknown,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 0.5ms, no | id=8 36t 2.2 1.1; id=12 10t 0.1 0.0; id=15 4t 2.3 0.6; id=18 4t 0.6 0.5; id=28 4t 0.0 0.0 |

DYNAMIC_FILTER operators (trace): id=11 [GPU_SCAN (id=10) -> DYNAMIC_FILTER (id=11)] tasks=10 rows 63430->63430 keep=1.000 ms=2.1; id=21 [GPU_SCAN (id=20) -> DYNAMIC_FILTER (id=21)] tasks=25 rows 444010->444010 keep=1.000 ms=3.5
Compressed-scan join-filter probes (ran of requested, complete): 1/1 complete=true: chunks=26 kept 355645/5431925084 = 0.0001
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=1: n=35 survivors 507440/7499989709
PARTITION operators (trace): id=1 tasks=1 rows_in=150000000 gpus=1 sum_ms=36.7 max_ms=36.7; id=5 tasks=25 rows_in=1500000000 gpus=4 sum_ms=1243.5 max_ms=84.9; id=8 tasks=36 rows_in=63430 gpus=4 sum_ms=2.2 max_ms=1.1; id=12 tasks=10 rows_in=63430 gpus=4 sum_ms=0.1 max_ms=0.0; id=15 tasks=4 rows_in=63430 gpus=4 sum_ms=2.3 max_ms=0.6; id=18 tasks=4 rows_in=63430 gpus=4 sum_ms=0.6 max_ms=0.5; id=22 tasks=25 rows_in=444010 gpus=4 sum_ms=0.2 max_ms=0.0; id=28 tasks=4 rows_in=63430 gpus=4 sum_ms=0.0 max_ms=0.0
iter0 vs hot: constructions 2 vs 2, key lines 2 vs 2, key outcomes identical=True

### q19
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=1 hot window 19:25:48.636 -> 19:25:49.524

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 9 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 480774 | 12.7MB | 1k/2b/4G | c0 hash_in_list 469.5KB 480774/480774 dom=200000000(pinned_parquet_exact,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 1.1ms, no | id=2 2t 1.3 1.2 |

DYNAMIC_FILTER operators (trace): id=5 [GPU_SCAN (id=4) -> DYNAMIC_FILTER (id=5) -> FILTER (id=6)] tasks=81 rows 2229241->2229241 keep=1.000 ms=1.5
Compressed-scan join-filter probes (ran of requested, complete): 1/1 complete=true: chunks=16 kept 714288/297867837 = 0.0024
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=1: n=81 survivors 3597127/5999989709
Scan cached checkpoints with folded DFs: n=81 rows 3597127->2229241
PARTITION operators (trace): id=2 tasks=2 rows_in=480774 gpus=2 sum_ms=1.3 max_ms=1.2; id=7 tasks=81 rows_in=319311 gpus=4 sum_ms=0.7 max_ms=0.0
iter0 vs hot: constructions 1 vs 1, key lines 1 vs 1, key outcomes identical=True

### q20
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=5 hot window 19:32:27.717 -> 19:32:28.438

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 7 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 1 | 4B | 1k/1b/4G | c0 small_in_list 1B 1/1 dom=0(unknown,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 0.2ms, no | id=1 1t 0.3 0.3 |
| 49 | RIGHT_SEMI/STANDARD | 1p hash | 131751 | 514.7KB | 1k/1b/4G | c0 hash_in_list 389.9KB 399302/399302 dom=? bits=- pushed=2 zone=0 reason=- | - | 2->2, 0.9ms, no | id=47 3t 0.0 0.0 |
| 18 | SEMI/BUILD_PROBE | 4p broadcast [broadcast] | 2174140 | 8.3MB | 1k/3b/4G | c0 bloom 4.1MB 2174140/2174140 dom=200000000(pinned_parquet_exact,keep) bits=16.0 pushed=1 zone=0 reason=- | 4.1MB root1 2/3 HOST staging | 1->1, 0.9ms, HOST | id=12 3t 1.1 1.0 |
| 31 | SEMI/BUILD_PROBE | 4p broadcast [broadcast] | 347054 | 2.6MB | 2k/1b/4G | c0 hash_in_list 338.9KB 347054/347054 dom=0(unknown,keep) bits=- pushed=1 zone=0 reason=-<br>c1 hash_in_list 338.9KB 347054/347054 dom=0(unknown,keep) bits=- pushed=1 zone=0 reason=- | - | 2->1, 1.4ms, no | id=23 3t 0.0 0.0; id=25 1t 1.4 1.4; id=43 3t 0.0 0.0 |
| 45 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 132209 | 3.0MB | 2k/1b/4G | c0 none 0B 132209/132209 dom=5999989709(pinned_parquet_exact,keep) bits=- pushed=0 zone=0 reason=attempt_skip_publications_skipped_targets_drained<br>c1 none 0B 132209/132209 dom=5999989709(pinned_parquet_exact,keep) bits=- pushed=0 zone=0 reason=attempt_skip_publications_skipped_targets_drained | - | - | id=38 1t 0.0 0.0 |

DYNAMIC_FILTER operators (trace): id=4 [GPU_SCAN (id=3) -> DYNAMIC_FILTER (id=4)] tasks=1 rows 399302->399302 keep=1.000 ms=0.3; id=15 [GPU_SCAN (id=14) -> DYNAMIC_FILTER (id=15)] tasks=3 rows 31944160->388843 keep=0.012 ms=0.7; id=28 [GPU_SCAN (id=27) -> DYNAMIC_FILTER (id=28)] tasks=31 rows 21137378->395501 keep=0.019 ms=28.4; id=40 [DYNAMIC_FILTER (id=40) -> DYNAMIC_FILTER (id=41) -> DYNAMIC_FILTER (id=42)] tasks=3 rows 347054->347054 keep=1.000 ms=0.9; id=41 [DYNAMIC_FILTER (id=40) -> DYNAMIC_FILTER (id=41) -> DYNAMIC_FILTER (id=42)] tasks=3 rows 347054->347054 keep=1.000 ms=0.0; id=42 [DYNAMIC_FILTER (id=40) -> DYNAMIC_FILTER (id=41) -> DYNAMIC_FILTER (id=42)] tasks=3 rows 347054->347054 keep=1.000 ms=0.0
Compressed-scan join-filter probes (ran of requested, complete): 1/1 complete=true: chunks=1 kept 399302/10000000 = 0.0399; 1/2 complete=false: chunks=34 kept 53081538/1710220849 = 0.0310
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=1: n=1 survivors 399302/10000000; APP,jf=2: n=34 survivors 53081538/6799989709
PARTITION operators (trace): id=1 tasks=1 rows_in=1 gpus=1 sum_ms=0.3 max_ms=0.3; id=5 tasks=1 rows_in=399302 gpus=1 sum_ms=0.0 max_ms=0.0; id=9 tasks=1 rows_in=399302 gpus=1 sum_ms=0.0 max_ms=0.0; id=12 tasks=3 rows_in=2174140 gpus=3 sum_ms=1.1 max_ms=1.0; id=16 tasks=3 rows_in=388843 gpus=3 sum_ms=0.0 max_ms=0.0; id=23 tasks=3 rows_in=347054 gpus=3 sum_ms=0.0 max_ms=0.0; id=25 tasks=1 rows_in=347054 gpus=1 sum_ms=1.4 max_ms=1.4; id=29 tasks=31 rows_in=395501 gpus=4 sum_ms=0.3 max_ms=0.0; id=35 tasks=4 rows_in=281916 gpus=4 sum_ms=0.0 max_ms=0.0; id=38 tasks=1 rows_in=132209 gpus=1 sum_ms=0.0 max_ms=0.0; id=43 tasks=3 rows_in=347054 gpus=3 sum_ms=0.0 max_ms=0.0; id=47 tasks=3 rows_in=131751 gpus=3 sum_ms=0.0 max_ms=0.0
iter0 vs hot: constructions 5 vs 5, key lines 7 vs 7, key outcomes identical=True

### q21
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=4 hot window 19:36:50.792 -> 19:36:52.248

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 7 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 1 | 4B | 1k/1b/4G | c0 small_in_list 1B 1/1 dom=25(pinned_parquet_exact,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 0.2ms, no | id=1 1t 0.3 0.3 |
| 21 | INNER/BUILD_PROBE | 4p broadcast [broadcast] | 399867 | 11.4MB | 1k/1b/4G | c0 hash_in_list 390.5KB 399867/399867 dom=0(unknown,keep) bits=- pushed=1 zone=0 reason=- | - | 1->1, 0.9ms, no | id=5 1t 0.0 0.0; id=8 1t 0.9 0.9 |
| 18 | INNER/BUILD_PROBE | 4p hash | 730806711 | 5.4GB | 1k/5b/4G | c0 bloom 1.4GB 730806711/730806711 dom=1500000000(pinned_parquet_exact,keep) bits=16.0 pushed=1 zone=0 reason=- | 1.4GB root3 3/3 HOST staging | 1->1, 316.9ms, HOST | id=11 5t 473.4 316.9 |
| 33 | SEMI/BUILD_PROBE | 4p broadcast [broadcast] | 69665236 | 531.5MB | 1k/4b/4G | c0 bloom 132.9MB 69665236/69665236 dom=0(unknown,keep) bits=16.0 pushed=1 zone=0 reason=- | 132.9MB root0 3/3 HOST staging | 1->1, 26.1ms, HOST | id=25 4t 3.8 1.0; id=27 4t 30.8 26.1; id=36 8t 32.4 6.3; id=38 4t 10.6 2.7 |
| 42 | LEFT/BUILD_PROBE | 4p hash | 69665236 | 2.1GB | - | - | - | - | id=25 4t 3.8 1.0; id=27 4t 30.8 26.1; id=36 8t 32.4 6.3; id=38 4t 10.6 2.7 |

DYNAMIC_FILTER operators (trace): id=4 [GPU_SCAN (id=3) -> DYNAMIC_FILTER (id=4)] tasks=1 rows 399867->399867 keep=1.000 ms=0.2; id=14 [GPU_SCAN (id=13) -> DYNAMIC_FILTER (id=14) -> FILTER (id=15)] tasks=31 rows 239905061->239905061 keep=1.000 ms=55.9; id=30 [GPU_SCAN (id=29) -> DYNAMIC_FILTER (id=30)] tasks=31 rows 352473750->352473750 keep=1.000 ms=68.6
Compressed-scan join-filter probes (ran of requested, complete): 1/1 complete=true: chunks=17 kept 183392112/3124919155 = 0.0587; 1/2 complete=false: chunks=31 kept 239905061/5999989709 = 0.0400
Compressed-scan decode outcomes (applied?, #join_filters): APP,jf=1: n=32 survivors 352873617/6009989709; APP,jf=2: n=31 survivors 239905061/5999989709; NOT,jf=0: n=5
PARTITION operators (trace): id=1 tasks=1 rows_in=1 gpus=1 sum_ms=0.3 max_ms=0.3; id=5 tasks=1 rows_in=399867 gpus=1 sum_ms=0.0 max_ms=0.0; id=8 tasks=1 rows_in=399867 gpus=1 sum_ms=0.9 max_ms=0.9; id=11 tasks=5 rows_in=730806711 gpus=4 sum_ms=473.4 max_ms=316.9; id=16 tasks=31 rows_in=151663502 gpus=4 sum_ms=258.4 max_ms=79.7; id=19 tasks=4 rows_in=73243004 gpus=4 sum_ms=0.0 max_ms=0.0; id=25 tasks=4 rows_in=69665236 gpus=4 sum_ms=3.8 max_ms=1.0; id=27 tasks=4 rows_in=69665236 gpus=4 sum_ms=30.8 max_ms=26.1; id=31 tasks=31 rows_in=352473750 gpus=4 sum_ms=0.3 max_ms=0.0; id=36 tasks=8 rows_in=69665236 gpus=4 sum_ms=32.4 max_ms=6.3; id=38 tasks=4 rows_in=69665236 gpus=4 sum_ms=10.6 max_ms=2.7; id=40 tasks=4 rows_in=73243004 gpus=4 sum_ms=15.2 max_ms=3.9; id=45 tasks=4 rows_in=1403585 gpus=4 sum_ms=0.0 max_ms=0.0
iter0 vs hot: constructions 4 vs 4, key lines 4 vs 4, key outcomes identical=True

### q22
windows=2 last_ok=True fallback_lines=1 error_lines=0 oom_retries=0 wired_joins=1 hot window 19:44:01.423 -> 19:44:01.769

| join | type/mode | layout (parts, exch) | build rows | build bytes | constr: keys/batches/GPUs | key: cond kind size valid/rows domain(prov) bits/valid pushed reason | bloom finish: bytes root src/tgt transport | published: filters->targets, ms after final, HOST | build PARTITION (rows match): id tasks sum_ms max_ms |
|---|---|---|---|---|---|---|---|---|---|
| 28 | SEMI/BUILD_PROBE | 4p broadcast [broadcast] | 19092648 | 72.8MB | 1k/4b/4G | c0 bloom 36.4MB 19092648/19092648 dom=0(unknown,keep) bits=16.0 pushed=1 zone=0 reason=- | 36.4MB root3 3/3 HOST staging | 1->1, 7.1ms, HOST | id=20 2t 0.9 0.6; id=22 4t 7.5 7.1; id=31 2t 5.2 3.8 |
| 33 | ANTI/BUILD_PROBE | 4p hash | 190939598 | 728.4MB | - | - | - | - | id=29 2t 5.5 3.5 |

DYNAMIC_FILTER operators (trace): id=25 [GPU_SCAN (id=24) -> DYNAMIC_FILTER (id=25)] tasks=2 rows 1500000000->192658340 keep=0.128 ms=19.0
Compressed-scan decode outcomes (applied?, #join_filters): NOT,jf=0: n=2
PARTITION operators (trace): id=7 tasks=1 rows_in=1 gpus=1 sum_ms=0.0 max_ms=0.0; id=13 tasks=2 rows_in=41998078 gpus=2 sum_ms=0.0 max_ms=0.0; id=20 tasks=2 rows_in=19092648 gpus=2 sum_ms=0.9 max_ms=0.6; id=22 tasks=4 rows_in=19092648 gpus=4 sum_ms=7.5 max_ms=7.1; id=26 tasks=2 rows_in=192658340 gpus=2 sum_ms=0.0 max_ms=0.0; id=29 tasks=2 rows_in=190939598 gpus=2 sum_ms=5.5 max_ms=3.5; id=31 tasks=2 rows_in=19092648 gpus=2 sum_ms=5.2 max_ms=3.8; id=36 tasks=4 rows_in=28 gpus=4 sum_ms=0.0 max_ms=0.0
iter0 vs hot: constructions 1 vs 1, key lines 1 vs 1, key outcomes identical=True
