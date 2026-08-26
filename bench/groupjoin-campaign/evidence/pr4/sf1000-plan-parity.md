# PR-4 kit A/B (sf1000): plan parity, fusion/strategy/reservation parity, runtimes

- A (enable_group_join=false via yaml copy, PR-4 binary): `/localhome/local-kkristensen/Code/sirius/test/tpch_performance/output/tpch_20260825_185832_gj_pr4_sf1000_A`
- B (enable_group_join=true/default, same PR-4 binary): `/localhome/local-kkristensen/Code/sirius/test/tpch_performance/output/tpch_20260825_190445_gj_pr4_sf1000_B`
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
| q1 | 0.7812 | 0.7797 | -0.0014 | -0.18% |
| q2 | 0.2244 | 0.1808 | -0.0436 | -19.43% |
| q3 | 0.3256 | 0.3268 | +0.0012 | +0.38% |
| q4 | 0.2301 | 0.2296 | -0.0006 | -0.24% |
| q5 | 0.3517 | 0.3451 | -0.0066 | -1.87% |
| q6 | 0.1588 | 0.1630 | +0.0043 | +2.68% |
| q7 | 0.3943 | 0.3856 | -0.0088 | -2.23% |
| q8 | 0.3290 | 0.3147 | -0.0143 | -4.34% |
| q9 | 0.9258 | 0.9168 | -0.0090 | -0.97% |
| q10 | 0.5745 | 0.5779 | +0.0033 | +0.58% |
| q11 | 0.1155 | 0.0876 | -0.0278 | -24.11% |
| q12 | 0.3203 | 0.3229 | +0.0026 | +0.81% |
| q13 | 0.3912 | 0.3935 | +0.0023 | +0.58% |
| q14 | 0.1822 | 0.1827 | +0.0005 | +0.27% |
| q15 | 0.1822 | 0.1962 | +0.0140 | +7.68% |
| q16 | 0.3742 | 0.3750 | +0.0008 | +0.21% |
| q17 | 0.2501 | 0.2677 | +0.0176 | +7.04% |
| q18 | 0.9813 | 0.9796 | -0.0018 | -0.18% |
| q19 | 0.3419 | 0.3582 | +0.0163 | +4.77% |
| q20 | 0.2735 | 0.2779 | +0.0043 | +1.58% |
| q21 | 0.7950 | 0.7944 | -0.0006 | -0.07% |
| q22 | 0.2002 | 0.1585 | -0.0417 | -20.83% |
| **total** | 8.7028 | 8.6140 | -0.0889 | -1.02% |

## Verdict

PASS: all 22 queries plan/fusion/strategy/reservation identical modulo the rename.
