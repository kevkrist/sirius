# PR-4 kit A/B (sf100): plan parity, fusion/strategy/reservation parity, runtimes

- A (enable_group_join=false via yaml copy, PR-4 binary): `/localhome/local-kkristensen/Code/sirius/test/tpch_performance/output/tpch_20260825_184703_gj_pr4_sf100_A`
- B (enable_group_join=true/default, same PR-4 binary): `/localhome/local-kkristensen/Code/sirius/test/tpch_performance/output/tpch_20260825_185036_gj_pr4_sf100_B`
- normalization: DENSE_COUNT_JOIN->GROUP_JOIN, dense_count_join->group_join, timestamps/file:line/task-id stripped, box layout stripped

| query | plan | fusion | strategy | reservation |
|---|---|---|---|---|
| q1 | OK (45) | OK (none) | OK (none) | OK (none) |
| q2 | DIFF | OK (none) | DIFF | OK (none) |
| q3 | OK (102) | OK (none) | OK (none) | OK (none) |
| q4 | OK (138) | OK (none) | OK (none) | OK (none) |
| q5 | OK (217) | OK (none) | OK (none) | OK (none) |
| q6 | OK (21) | OK (none) | OK (none) | OK (none) |
| q7 | OK (206) | OK (none) | OK (none) | OK (none) |
| q8 | OK (276) | OK (none) | OK (none) | OK (none) |
| q9 | OK (194) | OK (none) | OK (none) | OK (none) |
| q10 | OK (124) | OK (none) | OK (none) | OK (none) |
| q11 | OK (198) | OK (none) | OK (none) | OK (none) |
| q12 | OK (79) | OK (none) | OK (none) | OK (none) |
| q13 | OK (61) | OK (1) | OK (2) | OK (none) |
| q14 | OK (56) | OK (none) | OK (none) | OK (none) |
| q15 | OK (155) | OK (none) | OK (none) | OK (none) |
| q16 | OK (115) | OK (none) | OK (none) | OK (none) |
| q17 | DIFF | OK (none) | DIFF | OK (none) |
| q18 | OK (151) | OK (none) | OK (none) | OK (none) |
| q19 | OK (58) | OK (none) | OK (none) | OK (none) |
| q20 | OK (233) | OK (none) | OK (none) | OK (none) |
| q21 | OK (279) | OK (none) | OK (none) | OK (none) |
| q22 | OK (185) | OK (none) | OK (none) | OK (none) |

## Runtimes (best-of-3, seconds)

| query | A | B | delta | delta% |
|---|---|---|---|---|
| q1 | 0.1255 | 0.1374 | +0.0120 | +9.54% |
| q2 | 0.1843 | 0.1739 | -0.0104 | -5.65% |
| q3 | 0.1068 | 0.1136 | +0.0068 | +6.35% |
| q4 | 0.1024 | 0.1122 | +0.0098 | +9.59% |
| q5 | 0.1640 | 0.1282 | -0.0358 | -21.82% |
| q6 | 0.0558 | 0.0612 | +0.0054 | +9.62% |
| q7 | 0.1984 | 0.1340 | -0.0644 | -32.44% |
| q8 | 0.1885 | 0.1926 | +0.0042 | +2.21% |
| q9 | 0.2266 | 0.1949 | -0.0317 | -13.99% |
| q10 | 0.1587 | 0.1646 | +0.0058 | +3.68% |
| q11 | 0.0869 | 0.0755 | -0.0113 | -13.04% |
| q12 | 0.1218 | 0.1210 | -0.0008 | -0.67% |
| q13 | 0.0849 | 0.0696 | -0.0153 | -17.97% |
| q14 | 0.0842 | 0.0844 | +0.0002 | +0.28% |
| q15 | 0.1076 | 0.1014 | -0.0062 | -5.73% |
| q16 | 0.1438 | 0.1418 | -0.0020 | -1.38% |
| q17 | 0.1776 | 0.1341 | -0.0435 | -24.49% |
| q18 | 0.2402 | 0.2269 | -0.0133 | -5.54% |
| q19 | 0.1122 | 0.1171 | +0.0049 | +4.40% |
| q20 | 0.1926 | 0.1843 | -0.0083 | -4.32% |
| q21 | 0.2983 | 0.2935 | -0.0048 | -1.61% |
| q22 | 0.1018 | 0.0825 | -0.0194 | -19.02% |
| **total** | 3.2629 | 3.0449 | -0.2180 | -6.68% |

## Diffs

### q2 / plan

```diff
--- A/q2/plan
+++ B/q2/plan
@@ -193,76 +193,73 @@
 Input: <- Pipeline #49 [on: CONCAT (id=60), port: default, barrier: PARTIAL]
 Dependencies: Pipeline #49
 Output: -> HASH_JOIN [port: default, barrier: PARTIAL]
-Pipeline #51: HASH_JOIN (id=61) -> PROJECTION (id=62) -> PROJECTION (id=63) -> HASH_GROUP_BY (id=64)
+Pipeline #51: HASH_JOIN (id=61) -> PROJECTION (id=62)
 Input: <- Pipeline #50 [on: HASH_JOIN (id=61), port: default, barrier: PARTIAL]
 Input: <- Pipeline #14 [on: HASH_JOIN (id=61), port: build, barrier: PARTIAL]
 Dependencies: Pipeline #14, Pipeline #50
-Output: -> PARTITION [port: default, barrier: FULL]
-Pipeline #52: PARTITION (id=65)
-Input: <- Pipeline #51 [on: PARTITION (id=65), port: default, barrier: FULL]
+Output: -> GROUP_JOIN [port: counted, barrier: FULL]
+Pipeline #52: GROUP_JOIN (id=63)
+Input: <- Pipeline #51 [on: GROUP_JOIN (id=63), port: counted, barrier: FULL]
 Dependencies: Pipeline #51
-Output: -> MERGE_GROUP_BY [port: default, barrier: FULL]
-Pipeline #53: MERGE_GROUP_BY (id=66) -> PROJECTION (id=67)
-Input: <- Pipeline #52 [on: MERGE_GROUP_BY (id=66), port: default, barrier: FULL]
+Output: -> PROJECTION [port: default, barrier: FULL]
+Pipeline #53: PROJECTION (id=64)
+Input: <- Pipeline #52 [on: PROJECTION (id=64), port: default, barrier: FULL]
 Dependencies: Pipeline #52
 Output: -> PARTITION [port: default, barrier: FULL]
-Pipeline #54: PARTITION (id=68)
-Input: <- Pipeline #53 [on: PARTITION (id=68), port: default, barrier: FULL]
+Pipeline #54: PARTITION (id=65)
+Input: <- Pipeline #53 [on: PARTITION (id=65), port: default, barrier: FULL]
 Dependencies: Pipeline #53
 Output: -> CONCAT [port: default, barrier: PARTIAL]
-Pipeline #55: CONCAT (id=69)
-Input: <- Pipeline #54 [on: CONCAT (id=69), port: default, barrier: PARTIAL]
+Pipeline #55: CONCAT (id=66)
+Input: <- Pipeline #54 [on: CONCAT (id=66), port: default, barrier: PARTIAL]
 Dependencies: Pipeline #54
 Output: -> HASH_JOIN [port: build, barrier: PARTIAL]
-Pipeline #56: COLUMN_DATA_SCAN (id=70)
-Input: <- Pipeline #39 [on: COLUMN_DATA_SCAN (id=70), port: default, barrier: FULL]
+Pipeline #56: COLUMN_DATA_SCAN (id=67)
+Input: <- Pipeline #39 [on: COLUMN_DATA_SCAN (id=67), port: default, barrier: FULL]
 Dependencies: Pipeline #39
 Output: -> PARTITION [port: default, barrier: PARTIAL]
-Pipeline #57: PARTITION (id=71)
-Input: <- Pipeline #56 [on: PARTITION (id=71), port: default, barrier: PARTIAL]
+Pipeline #57: PARTITION (id=68)
+Input: <- Pipeline #56 [on: PARTITION (id=68), port: default, barrier: PARTIAL]
 Dependencies: Pipeline #56
 Output: -> CONCAT [port: default, barrier: PARTIAL]
-Pipeline #58: CONCAT (id=72)
-Input: <- Pipeline #57 [on: CONCAT (id=72), port: default, barrier: PARTIAL]
+Pipeline #58: CONCAT (id=69)
+Input: <- Pipeline #57 [on: CONCAT (id=69), port: default, barrier: PARTIAL]
 Dependencies: Pipeline #57
 Output: -> HASH_JOIN [port: default, barrier: PARTIAL]
-Pipeline #59: HASH_JOIN (id=73) -> FILTER (id=74) -> PROJECTION (id=75) -> TOP_N (id=76)
-Input: <- Pipeline #55 [on: HASH_JOIN (id=73), port: build, barrier: PARTIAL]
-Input: <- Pipeline #58 [on: HASH_JOIN (id=73), port: default, barrier: PARTIAL]
+Pipeline #59: HASH_JOIN (id=70) -> FILTER (id=71) -> PROJECTION (id=72) -> TOP_N (id=73)
+Input: <- Pipeline #55 [on: HASH_JOIN (id=70), port: build, barrier: PARTIAL]
+Input: <- Pipeline #58 [on: HASH_JOIN (id=70), port: default, barrier: PARTIAL]
 Dependencies: Pipeline #55, Pipeline #58
 Output: -> MERGE_TOP_N [port: default, barrier: FULL]
-Pipeline #60: MERGE_TOP_N (id=77) -> RESULT_COLLECTOR (id=78)
-Input: <- Pipeline #59 [on: MERGE_TOP_N (id=77), port: default, barrier: FULL]
+Pipeline #60: MERGE_TOP_N (id=74) -> RESULT_COLLECTOR (id=75)
+Input: <- Pipeline #59 [on: MERGE_TOP_N (id=74), port: default, barrier: FULL]
 Dependencies: Pipeline #59
 === Query Plan DAG ===
 Pipeline #60
-RESULT_COLLECTOR (id=78)
-MERGE_TOP_N (id=77)
+RESULT_COLLECTOR (id=75)
+MERGE_TOP_N (id=74)
 Input(#59): FULL
 Pipeline #59
-TOP_N (id=76)
-PROJECTION (id=75)
-FILTER (id=74)
-HASH_JOIN (id=73)
+TOP_N (id=73)
+PROJECTION (id=72)
+FILTER (id=71)
+HASH_JOIN (id=70)
 type: LEFT
 Input(#55): PARTIAL
 Input(#58): PARTIAL
 Pipeline #55 Pipeline #58
-CONCAT (id=69) CONCAT (id=72)
+CONCAT (id=66) CONCAT (id=69)
 Input(#54): PARTIAL Input(#57): PARTIAL
 Pipeline #54 Pipeline #57
-PARTITION (id=68) PARTITION (id=71)
+PARTITION (id=65) PARTITION (id=68)
 Input(#53): FULL Input(#56): PARTIAL
 Pipeline #53 Pipeline #56
-PROJECTION (id=67) COLUMN_DATA_SCAN (id=70)
-MERGE_GROUP_BY (id=66) Input(#39): FULL
-Input(#52): FULL
+PROJECTION (id=64) COLUMN_DATA_SCAN (id=67)
+Input(#52): FULL Input(#39): FULL
 Pipeline #52 (-> Pipeline #39)
-PARTITION (id=65)
+GROUP_JOIN (id=63)
 Input(#51): FULL
 Pipeline #51
-HASH_GROUP_BY (id=64)
-PROJECTION (id=63)
 PROJECTION (id=62)
 HASH_JOIN (id=61)
 type: INNER
```
### q2 / strategy

```diff
--- A/q2/strategy
+++ B/q2/strategy
@@ -0,0 +1 @@
+[group_join] emitted 47481 group rows (sparse strategy)
```
### q17 / plan

```diff
--- A/q17/plan
+++ B/q17/plan
@@ -36,115 +36,81 @@
 Pipeline #9: MERGE_GROUP_BY (id=12)
 Input: <- Pipeline #8 [on: MERGE_GROUP_BY (id=12), port: default, barrier: FULL]
 Dependencies: Pipeline #8
+Output: -> GROUP_JOIN [port: preserved, barrier: FULL]
+Pipeline #10: GPU_SCAN (id=13) -> DYNAMIC_FILTER (id=14)
+Output: -> GROUP_JOIN [port: counted, barrier: FULL]
+Pipeline #11: GROUP_JOIN (id=15)
+Input: <- Pipeline #10 [on: GROUP_JOIN (id=15), port: counted, barrier: FULL]
+Input: <- Pipeline #9 [on: GROUP_JOIN (id=15), port: preserved, barrier: FULL]
+Dependencies: Pipeline #9, Pipeline #10
+Output: -> PROJECTION [port: default, barrier: FULL]
+Pipeline #12: PROJECTION (id=16)
+Input: <- Pipeline #11 [on: PROJECTION (id=16), port: default, barrier: FULL]
+Dependencies: Pipeline #11
 Output: -> PARTITION [port: default, barrier: FULL]
-Pipeline #10: PARTITION (id=13)
-Input: <- Pipeline #9 [on: PARTITION (id=13), port: default, barrier: FULL]
-Dependencies: Pipeline #9
-Output: -> CONCAT [port: default, barrier: PARTIAL]
-Pipeline #11: CONCAT (id=14)
-Input: <- Pipeline #10 [on: CONCAT (id=14), port: default, barrier: PARTIAL]
-Dependencies: Pipeline #10
-Output: -> HASH_JOIN [port: build, barrier: PARTIAL]
-Pipeline #12: GPU_SCAN (id=15) -> DYNAMIC_FILTER (id=16)
-Output: -> PARTITION [port: default, barrier: PARTIAL]
 Pipeline #13: PARTITION (id=17)
-Input: <- Pipeline #12 [on: PARTITION (id=17), port: default, barrier: PARTIAL]
+Input: <- Pipeline #12 [on: PARTITION (id=17), port: default, barrier: FULL]
 Dependencies: Pipeline #12
 Output: -> CONCAT [port: default, barrier: PARTIAL]
 Pipeline #14: CONCAT (id=18)
 Input: <- Pipeline #13 [on: CONCAT (id=18), port: default, barrier: PARTIAL]
 Dependencies: Pipeline #13
-Output: -> HASH_JOIN [port: default, barrier: PARTIAL]
-Pipeline #15: HASH_JOIN (id=19) -> PROJECTION (id=20) -> PROJECTION (id=21) -> HASH_GROUP_BY (id=22)
-Input: <- Pipeline #11 [on: HASH_JOIN (id=19), port: build, barrier: PARTIAL]
-Input: <- Pipeline #14 [on: HASH_JOIN (id=19), port: default, barrier: PARTIAL]
-Dependencies: Pipeline #11, Pipeline #14
-Output: -> PARTITION [port: default, barrier: FULL]
-Pipeline #16: PARTITION (id=23)
-Input: <- Pipeline #15 [on: PARTITION (id=23), port: default, barrier: FULL]
-Dependencies: Pipeline #15
-Output: -> MERGE_GROUP_BY [port: default, barrier: FULL]
-Pipeline #17: MERGE_GROUP_BY (id=24) -> PROJECTION (id=25)
-Input: <- Pipeline #16 [on: MERGE_GROUP_BY (id=24), port: default, barrier: FULL]
-Dependencies: Pipeline #16
-Output: -> PARTITION [port: default, barrier: FULL]
-Pipeline #18: PARTITION (id=26)
-Input: <- Pipeline #17 [on: PARTITION (id=26), port: default, barrier: FULL]
-Dependencies: Pipeline #17
-Output: -> CONCAT [port: default, barrier: PARTIAL]
-Pipeline #19: CONCAT (id=27)
-Input: <- Pipeline #18 [on: CONCAT (id=27), port: default, barrier: PARTIAL]
-Dependencies: Pipeline #18
 Output: -> HASH_JOIN [port: build, barrier: PARTIAL]
-Pipeline #20: COLUMN_DATA_SCAN (id=28)
-Input: <- Pipeline #6 [on: COLUMN_DATA_SCAN (id=28), port: default, barrier: FULL]
+Pipeline #15: COLUMN_DATA_SCAN (id=19)
+Input: <- Pipeline #6 [on: COLUMN_DATA_SCAN (id=19), port: default, barrier: FULL]
 Dependencies: Pipeline #6
 Output: -> PARTITION [port: default, barrier: PARTIAL]
-Pipeline #21: PARTITION (id=29)
-Input: <- Pipeline #20 [on: PARTITION (id=29), port: default, barrier: PARTIAL]
-Dependencies: Pipeline #20
+Pipeline #16: PARTITION (id=20)
+Input: <- Pipeline #15 [on: PARTITION (id=20), port: default, barrier: PARTIAL]
+Dependencies: Pipeline #15
 Output: -> CONCAT [port: default, barrier: PARTIAL]
-Pipeline #22: CONCAT (id=30)
-Input: <- Pipeline #21 [on: CONCAT (id=30), port: default, barrier: PARTIAL]
-Dependencies: Pipeline #21
+Pipeline #17: CONCAT (id=21)
+Input: <- Pipeline #16 [on: CONCAT (id=21), port: default, barrier: PARTIAL]
+Dependencies: Pipeline #16
 Output: -> HASH_JOIN [port: default, barrier: PARTIAL]
-Pipeline #23: HASH_JOIN (id=31) -> FILTER (id=32) -> UNGROUPED_AGGREGATE (id=33)
-Input: <- Pipeline #19 [on: HASH_JOIN (id=31), port: build, barrier: PARTIAL]
-Input: <- Pipeline #22 [on: HASH_JOIN (id=31), port: default, barrier: PARTIAL]
-Dependencies: Pipeline #19, Pipeline #22
+Pipeline #18: HASH_JOIN (id=22) -> FILTER (id=23) -> UNGROUPED_AGGREGATE (id=24)
+Input: <- Pipeline #14 [on: HASH_JOIN (id=22), port: build, barrier: PARTIAL]
+Input: <- Pipeline #17 [on: HASH_JOIN (id=22), port: default, barrier: PARTIAL]
+Dependencies: Pipeline #14, Pipeline #17
 Output: -> MERGE_AGGREGATE [port: default, barrier: FULL]
-Pipeline #24: MERGE_AGGREGATE (id=34)
-Input: <- Pipeline #23 [on: MERGE_AGGREGATE (id=34), port: default, barrier: FULL]
-Dependencies: Pipeline #23
+Pipeline #19: MERGE_AGGREGATE (id=25)
+Input: <- Pipeline #18 [on: MERGE_AGGREGATE (id=25), port: default, barrier: FULL]
+Dependencies: Pipeline #18
 Output: -> PROJECTION [port: default, barrier: FULL]
-Pipeline #25: PROJECTION (id=35) -> RESULT_COLLECTOR (id=36)
-Input: <- Pipeline #24 [on: PROJECTION (id=35), port: default, barrier: FULL]
-Dependencies: Pipeline #24
+Pipeline #20: PROJECTION (id=26) -> RESULT_COLLECTOR (id=27)
+Input: <- Pipeline #19 [on: PROJECTION (id=26), port: default, barrier: FULL]
+Dependencies: Pipeline #19
 === Query Plan DAG ===
-Pipeline #25
-RESULT_COLLECTOR (id=36)
-PROJECTION (id=35)
-Input(#24): FULL
-Pipeline #24
-MERGE_AGGREGATE (id=34)
-Input(#23): FULL
-Pipeline #23
-UNGROUPED_AGGREGATE (id=33)
-FILTER (id=32)
-HASH_JOIN (id=31)
+Pipeline #20
+RESULT_COLLECTOR (id=27)
+PROJECTION (id=26)
+Input(#19): FULL
+Pipeline #19
+MERGE_AGGREGATE (id=25)
+Input(#18): FULL
+Pipeline #18
+UNGROUPED_AGGREGATE (id=24)
+FILTER (id=23)
+HASH_JOIN (id=22)
 type: LEFT
-Input(#19): PARTIAL
-Input(#22): PARTIAL
-Pipeline #19 Pipeline #22
-CONCAT (id=27) CONCAT (id=30)
-Input(#18): PARTIAL Input(#21): PARTIAL
-Pipeline #18 Pipeline #21
-PARTITION (id=26) PARTITION (id=29)
-Input(#17): FULL Input(#20): PARTIAL
-Pipeline #17 Pipeline #20
-PROJECTION (id=25) COLUMN_DATA_SCAN (id=28)
-MERGE_GROUP_BY (id=24) Input(#6): FULL
-Input(#16): FULL
-Pipeline #16 (-> Pipeline #6)
-PARTITION (id=23)
-Input(#15): FULL
-Pipeline #15
-HASH_GROUP_BY (id=22)
-PROJECTION (id=21)
-PROJECTION (id=20)
-HASH_JOIN (id=19)
-type: INNER
-Input(#11): PARTIAL
 Input(#14): PARTIAL
-Pipeline #11 Pipeline #14
-CONCAT (id=14) CONCAT (id=18)
-Input(#10): PARTIAL Input(#13): PARTIAL
-Pipeline #10 Pipeline #13
-PARTITION (id=13) PARTITION (id=17)
-Input(#9): FULL Input(#12): PARTIAL
-Pipeline #9 Pipeline #12
-MERGE_GROUP_BY (id=12) DYNAMIC_FILTER (id=16)
-Input(#8): FULL GPU_SCAN (id=15)
+Input(#17): PARTIAL
+Pipeline #14 Pipeline #17
+CONCAT (id=18) CONCAT (id=21)
+Input(#13): PARTIAL Input(#16): PARTIAL
+Pipeline #13 Pipeline #16
+PARTITION (id=17) PARTITION (id=20)
+Input(#12): FULL Input(#15): PARTIAL
+Pipeline #12 Pipeline #15
+PROJECTION (id=16) COLUMN_DATA_SCAN (id=19)
+Input(#11): FULL Input(#6): FULL
+Pipeline #11 (-> Pipeline #6)
+GROUP_JOIN (id=15)
+Input(#10): FULL
+Input(#9): FULL
+Pipeline #9 Pipeline #10
+MERGE_GROUP_BY (id=12) DYNAMIC_FILTER (id=14)
+Input(#8): FULL GPU_SCAN (id=13)
 Pipeline #8
 PARTITION (id=11)
 Input(#7): FULL
```
### q17 / strategy

```diff
--- A/q17/strategy
+++ B/q17/strategy
@@ -0,0 +1 @@
+[group_join] emitted 20067 group rows (sparse strategy)
```

## Verdict

FAIL: differences found (see Diffs).
