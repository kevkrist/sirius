# Single-query analysis — q18 SF1000, PR-6 (P3/preserved_remap) go/no-go profile
- Run: `test/tpch_performance/output/tpch_20260826_184413_gj_pr6_q18_trace` (kit regime + `sirius_log_level=trace`, binary = feat/groupjoin-framework @ ef272c01, enable_group_join default on)
- Iteration analyzed: 2026-08-26 18:45:39.252 (warm), duration 974 ms (trace overhead ~5% vs kit's ~0.96 s)
- Decision gate (design §5.4): build P3 only if the fragment+gather share of q18 ≥ ~50 ms

## Top finding: NO-GO — the fusable fragment is ~6 ms, 8x under the threshold

## Operator time attribution (sum of per-operator execution time = 4153 ms; ops overlap, wall = 974 ms)

P3-fusable fragment (what a q18 groupjoin would delete):
| id | operator | time (ms) | rows out |
|---|---|---|---|
| 24 | HASH_JOIN (l_orderkey = o_orderkey, top) | 1.49 | 444,010 |
| 25/26 | PROJECTION ×2 | 0.24 | 444,010 |
| 27 | HASH_GROUP_BY (5-key, VARCHAR-bearing) | 2.19 | 63,430 |
| 28 | PARTITION | 0.01 | 63,430 |
| 29 | MERGE_GROUP_BY | 0.07 | 63,430 |
| — | pipeline gaps #20→#21→#22 | 2.00 | — |
| **total** | | **~6.0** | **0.1% of op time** |

Out-of-scope whale, exactly as design §5.4 predicted (the scan-direct `AGG[l_orderkey; sum(l_quantity)] HAVING` feeder — not a groupjoin shape: no join beneath it, 1.5B distinct keys):
| id | operator | time (ms) |
|---|---|---|
| 4 | HASH_GROUP_BY (feeder, 6B rows → 1.5B groups) | 2912.91 |
| 6 | MERGE_GROUP_BY (feeder) | 452.56 |
| 5 | PARTITION (feeder) | 298.43 |
| 3 | GPU_SCAN (lineitem for feeder) | 56.99 |
| 7 | FILTER (HAVING > 300) | 26.36 |
| **total** | | **3747 ms (90.2%)** |

Also notable: DYNAMIC_FILTER id=21 (membership filter application on the lineitem probe scan) = 311 ms — third-largest operator.

## Conclusion
- P3 (q18 via `preserved_remap` + carried columns): **NO-GO**. Even attributing the top join's entire time (not just its gather) to the fragment, the ceiling is ~6 ms against the ≥50 ms gate. The remap seam stays dormant per §5.4 ("until the membership sibling or q20 justifies it").
- q18's real headroom is the feeder group-by (2.9 s op time producing 1.5B groups). It is not reachable by the groupjoin framework (no join under it; l_orderkey domain ~6e9 → dense state 72–96 GB busts every budget; 1.5B keys ≫ the 128M remap-cap precedent). Candidate for a future dense-int64-key aggregation strategy (radix/sort-based) — separate campaign.

## Reproduce
- parser output dir: this directory (`parse_logs.py` over the run's `sirius/q18/sirius.log`)
- fragment ids from the plan render: pipeline #20 (24→27), #21 (28), #22 (29)
