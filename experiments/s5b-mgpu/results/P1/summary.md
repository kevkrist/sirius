# P1 summary: base against tip, TPC-H SF1000, host-pinned tables

Generated 2026-11-14T01:06:32+00:00. Latency: median of 5 hot runs in one process per build and GPU count. Base: geometric mean of the two base processes (A/A).

**Overall: FAIL** (GPU counts [4, 1])

## 4 GPU(s): PASS

- geomean tip/base: 1.0096 (bar <= 1.02); noise: A/A geomean base_b/base_a 1.0001, per-pass tip/base geomeans 1.0069..1.0108, per-pass A/A geomeans 0.9993..1.0072
- worst query: q3 at 1.068 (bar <= 1.1); noise: A/A per query median 0.3%, max 2.1%
- confirmed by re-run: none needed
- tip results equal to the CPU only as multisets (row order differs; check ORDER BY ties): ['q11']

| query | base_a ms | tip ms | base_b ms | tip/base | A/A | confirmed | identical | CPU match base/tip |
|---|---|---|---|---|---|---|---|---|
| q1 | 725.4 | 719.8 | 740.6 | 0.982 | +2.1% | - | True | True/True |
| q2 ** | 400.9 | 407.0 | 400.6 | 1.016 | -0.1% | - | True | True/True |
| q3 | 647.0 | 693.3 | 651.0 | 1.068 | +0.6% | - | True | True/True |
| q4 | 433.0 | 429.7 | 433.1 | 0.992 | +0.0% | - | True | True/True |
| q5 | 651.9 | 673.0 | 644.6 | 1.038 | -1.1% | - | True | True/True |
| q6 | 408.0 | 405.8 | 404.8 | 0.999 | -0.8% | - | True | True/True |
| q7 | 715.3 | 713.1 | 714.3 | 0.998 | -0.1% | - | True | True/True |
| q8 | 947.2 | 988.3 | 944.5 | 1.045 | -0.3% | - | True | True/True |
| q9 | 1455.2 | 1456.0 | 1464.0 | 0.998 | +0.6% | - | True | True/True |
| q10 | 1069.2 | 1070.3 | 1065.0 | 1.003 | -0.4% | - | True | True/True |
| q11 | 266.0 | 267.3 | 266.8 | 1.003 | +0.3% | - | True | True/True |
| q12 | 761.9 | 767.7 | 763.2 | 1.007 | +0.2% | - | True | True/True |
| q13 | 1096.6 | 1111.1 | 1104.7 | 1.010 | +0.7% | - | True | True/True |
| q14 | 709.8 | 710.4 | 706.0 | 1.004 | -0.5% | - | True | True/True |
| q15 ** | 589.7 | 590.9 | 591.4 | 1.001 | +0.3% | - | True | True/True |
| q16 | 335.0 | 343.3 | 335.6 | 1.024 | +0.2% | - | True | True/True |
| q17 | 831.8 | 830.7 | 829.9 | 1.000 | -0.2% | - | True | True/True |
| q18 | 1051.3 | 1044.5 | 1033.3 | 1.002 | -1.7% | - | True | True/True |
| q19 | 925.4 | 933.9 | 929.9 | 1.007 | +0.5% | - | True | True/True |
| q20 | 776.3 | 782.3 | 776.6 | 1.008 | +0.0% | - | True | True/True |
| q21 ** | 1606.0 | 1609.5 | 1604.7 | 1.003 | -0.1% | - | True | True/True |
| q22 | 247.5 | 250.1 | 247.4 | 1.011 | -0.0% | - | True | True/True |

Watch list (q2, q15, q21): q2 1.016; q15 1.001; q21 1.003

### 4 GPU(s): memory, base against tip (information only; not a pass bar)

| session | peak GPU memory (process) | OOM retries (all queries) | downgrade GiB (all queries) |
|---|---|---|---|
| base_a | 85.48 GiB | 0 | 0.00 |
| tip | 85.48 GiB | 0 | 0.00 |
| base_b | 85.48 GiB | 0 | 0.00 |

## 1 GPU(s): FAIL

- queries over 1.1 after confirmation: ['q3']
- geomean tip/base: 1.0086 (bar <= 1.02); noise: A/A geomean base_b/base_a 1.0009, per-pass tip/base geomeans 1.0075..1.0096, per-pass A/A geomeans 0.9968..1.0042
- worst query: q3 at 1.140 (bar <= 1.1); noise: A/A per query median 0.2%, max 0.8%
- confirmed by re-run: q3 1.152 -> 1.140
- tip results equal to the CPU only as multisets (row order differs; check ORDER BY ties): ['q11']

| query | base_a ms | tip ms | base_b ms | tip/base | A/A | confirmed | identical | CPU match base/tip |
|---|---|---|---|---|---|---|---|---|
| q1 | 2132.5 | 2133.5 | 2132.6 | 1.000 | +0.0% | - | True | True/True |
| q2 ** | 592.3 | 593.0 | 592.2 | 1.001 | -0.0% | - | True | True/True |
| q3 | 1091.6 | 1259.6 | 1095.1 | 1.152 | +0.3% | 1.140 | True | True/True |
| q4 | 682.1 | 692.0 | 683.6 | 1.013 | +0.2% | - | True | True/True |
| q5 | 1201.8 | 1211.1 | 1205.9 | 1.006 | +0.3% | - | True | True/True |
| q6 | 896.2 | 904.6 | 899.6 | 1.007 | +0.4% | - | True | True/True |
| q7 | 1382.8 | 1384.5 | 1385.0 | 1.000 | +0.2% | - | True | True/True |
| q8 | 1741.6 | 1748.5 | 1741.8 | 1.004 | +0.0% | - | True | True/True |
| q9 | 2914.6 | 2927.3 | 2901.2 | 1.007 | -0.5% | - | True | True/True |
| q10 | 1593.6 | 1596.8 | 1592.0 | 1.002 | -0.1% | - | True | True/True |
| q11 | 407.2 | 409.4 | 407.8 | 1.005 | +0.2% | - | True | True/True |
| q12 | 1574.7 | 1573.2 | 1572.6 | 1.000 | -0.1% | - | True | True/True |
| q13 | 2602.3 | 2602.9 | 2601.7 | 1.000 | -0.0% | - | True | True/True |
| q14 | 1278.9 | 1284.2 | 1276.9 | 1.005 | -0.2% | - | True | True/True |
| q15 ** | 1144.5 | 1143.0 | 1153.2 | 0.995 | +0.8% | - | True | True/True |
| q16 | 462.8 | 462.5 | 462.2 | 1.000 | -0.1% | - | True | True/True |
| q17 | 1766.9 | 1773.9 | 1766.5 | 1.004 | -0.0% | - | True | True/True |
| q18 | 1830.3 | 1842.0 | 1829.7 | 1.007 | -0.0% | - | True | True/True |
| q19 | 1998.9 | 2001.2 | 2007.4 | 0.999 | +0.4% | - | True | True/True |
| q20 | 1567.6 | 1558.0 | 1561.2 | 0.996 | -0.4% | - | True | True/True |
| q21 ** | 2966.3 | 2975.1 | 2968.2 | 1.003 | +0.1% | - | True | True/True |
| q22 | 347.9 | 349.7 | 349.8 | 1.003 | +0.6% | - | True | True/True |

Watch list (q2, q15, q21): q2 1.001; q15 0.995; q21 1.003

### 1 GPU(s): memory, base against tip (information only; not a pass bar)

| session | peak GPU memory (process) | OOM retries (all queries) | downgrade GiB (all queries) |
|---|---|---|---|
| base_a | 85.48 GiB | 0 | 79.65 |
| tip | 85.48 GiB | 0 | 79.65 |
| base_b | 85.48 GiB | 0 | 79.65 |

## P4: Dynamic Filter publication on the multi-GPU tip process

- Publication Sessions logged: 282; with Bloom Filters through HOST staging: 108; left out of the latency figures (0 filters pushed or no latency): 0
- publication latency after the final Contribution, ms: warm: n=47, p50 1.20, p95 32.10, max 44.70 (debug log, includes one-time costs); hot: n=235, p50 1.20, p95 32.20, max 44.80
- Bloom Filter reductions by transfer path (warm runs): {'host_staging': 18}
- The counters `accumulation_publication_latency_ns` and `accumulation_publications_host_staged` are not exposed outside C++ tests; these figures come from the `publication` and `bloom_finish` log lines (Appendix C).

## Processes

| session | status | wall min | setup min | peak GPU GiB | host RSS GiB | min MemAvailable GiB | disk GiB | pinned (compressed) |
|---|---|---|---|---|---|---|---|---|
| base_a_1gpu | ok | 6.8 | 3.1 | 85.5 | 467.9 | 501.2 | 0.0 | 8 (2) |
| base_a_4gpu | ok | 5.3 | 3.1 | 85.5 | 456.3 | 513.2 | 0.0 | 8 (2) |
| base_b_1gpu | ok | 6.8 | 3.1 | 85.5 | 467.9 | 500.9 | 0.0 | 8 (2) |
| base_b_4gpu | ok | 5.3 | 3.1 | 85.5 | 456.3 | 512.8 | 0.0 | 8 (2) |
| base_confirm_1gpu | ok | 3.7 | 3.1 | 23.6 | 454.1 | 515.0 | 0.0 | 8 (2) |
| tip_1gpu | ok | 7.0 | 3.1 | 85.5 | 467.9 | 500.9 | 0.0 | 8 (2) |
| tip_4gpu | ok | 5.3 | 3.1 | 85.5 | 455.8 | 513.5 | 0.0 | 8 (2) |
| tip_confirm_1gpu | ok | 3.7 | 3.1 | 59.7 | 454.1 | 515.1 | 0.0 | 8 (2) |

- GPU throttle reasons other than idle (`gpu_state.parquet`): none
- SM clock range before/after processes, MHz: after 180..2355; before 180..1725

