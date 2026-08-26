# PR-1 kit A/B: plan parity, fusion/strategy/reservation parity, runtimes

- A (baseline, pre-refactor binary): `/localhome/local-kkristensen/Code/sirius/test/tpch_performance/output/tpch_20260825_001121_gj_pr1_A`
- B (candidate, working-tree build): `/localhome/local-kkristensen/Code/sirius/test/tpch_performance/output/tpch_20260825_014000_gj_pr1_B`
- normalization: DENSE_COUNT_JOIN->GROUP_JOIN, dense_count_join->group_join, timestamps/file:line/task-id stripped, box layout stripped

| query | plan | fusion | strategy | reservation |
|---|---|---|---|---|
| q1 | OK (45) | OK (none) | OK (none) | OK (none) |
| q2 | OK (338) | OK (none) | OK (none) | OK (none) |
| q3 | OK (101) | OK (none) | OK (none) | OK (none) |
| q4 | OK (138) | OK (none) | OK (none) | OK (none) |
| q5 | OK (216) | OK (none) | OK (none) | OK (none) |
| q6 | OK (21) | OK (none) | OK (none) | OK (none) |
| q7 | OK (205) | OK (none) | OK (none) | OK (none) |
| q8 | OK (275) | OK (none) | OK (none) | OK (none) |
| q9 | OK (193) | OK (none) | OK (none) | OK (none) |
| q10 | OK (124) | OK (none) | OK (none) | OK (none) |
| q11 | OK (198) | OK (none) | OK (none) | OK (none) |
| q12 | OK (79) | OK (none) | OK (none) | OK (none) |
| q13 | OK (61) | OK (1) | OK (2) | OK (1) |
| q14 | OK (56) | OK (none) | OK (none) | OK (none) |
| q15 | OK (155) | OK (none) | OK (none) | OK (none) |
| q16 | OK (115) | OK (none) | OK (none) | OK (none) |
| q17 | OK (169) | OK (none) | OK (none) | OK (none) |
| q18 | OK (150) | OK (none) | OK (none) | OK (none) |
| q19 | OK (58) | OK (none) | OK (none) | OK (none) |
| q20 | OK (233) | OK (none) | OK (none) | OK (none) |
| q21 | OK (279) | OK (none) | OK (none) | OK (none) |
| q22 | OK (185) | OK (none) | OK (none) | OK (none) |

## Runtimes (best-of-3, seconds)

| query | A | B | delta | delta% |
|---|---|---|---|---|
| q1 | 0.7745 | 0.7973 | +0.0228 | +2.95% |
| q2 | 0.1917 | 0.1848 | -0.0069 | -3.62% |
| q3 | 0.3267 | 0.3291 | +0.0024 | +0.73% |
| q4 | 0.2225 | 0.2367 | +0.0142 | +6.40% |
| q5 | 0.3550 | 0.3292 | -0.0258 | -7.26% |
| q6 | 0.1607 | 0.1621 | +0.0014 | +0.90% |
| q7 | 0.3796 | 0.3859 | +0.0062 | +1.64% |
| q8 | 0.3427 | 0.3197 | -0.0230 | -6.71% |
| q9 | 0.9036 | 0.9271 | +0.0236 | +2.61% |
| q10 | 0.5772 | 0.5868 | +0.0097 | +1.68% |
| q11 | 0.0904 | 0.1002 | +0.0098 | +10.81% |
| q12 | 0.3156 | 0.3190 | +0.0034 | +1.08% |
| q13 | 0.4003 | 0.3867 | -0.0136 | -3.40% |
| q14 | 0.1772 | 0.1844 | +0.0073 | +4.11% |
| q15 | 0.1922 | 0.2043 | +0.0121 | +6.32% |
| q16 | 0.3730 | 0.3631 | -0.0099 | -2.65% |
| q17 | 0.2654 | 0.2752 | +0.0098 | +3.71% |
| q18 | 0.9725 | 0.9572 | -0.0153 | -1.58% |
| q19 | 0.3450 | 0.3547 | +0.0097 | +2.82% |
| q20 | 0.2748 | 0.2898 | +0.0150 | +5.47% |
| q21 | 0.8025 | 0.8053 | +0.0028 | +0.35% |
| q22 | 0.1890 | 0.1774 | -0.0116 | -6.13% |
| **total** | 8.6320 | 8.6761 | +0.0442 | +0.51% |

## Verdict

PASS: all 22 queries plan/fusion/strategy/reservation identical modulo the rename.
