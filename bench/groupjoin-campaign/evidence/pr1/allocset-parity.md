# PR-1 allocation-set parity: q13 SF1, trace-level reservation logs

- A (pre-refactor binary `baseline/duckdb-baseline`): `/tmp/claude-2524/-localhome-local-kkristensen-Code-sirius/435cb90d-7f5f-4e21-9e99-c912f2e98527/scratchpad/pr1/allocset/log-A`
- B (working-tree binary `build/release/duckdb`): `/tmp/claude-2524/-localhome-local-kkristensen-Code-sirius/435cb90d-7f5f-4e21-9e99-c912f2e98527/scratchpad/pr1/allocset/log-B`

- plan render: IDENTICAL (A 61 entries, B 61 entries)
- fusion decision: IDENTICAL (A 1 entries, B 1 entries)
- reservation requests (pipeline, bytes): IDENTICAL (A 9 entries, B 9 entries)
- reservation clamps: IDENTICAL (A 0 entries, B 0 entries)
- runtime strategy: IDENTICAL (A 2 entries, B 2 entries)
- query result (CSV rows): IDENTICAL (A 43 entries, B 43 entries)

## Fusion decision
```
[sirius_plan_aggregate] Fusing COUNT-join into GROUP_JOIN: RIGHT join, preserved child 1 (key col 0, est 150000 rows), counted child 0 (key col 0, COUNT(col 1), est 1500000 rows)
```
## Reservation requests (pipeline id, bytes)
```
pipeline 0: 4800000 bytes
pipeline 1: 1128000000 bytes
pipeline 2: 295560832 bytes
pipeline 3: 3600000 bytes
pipeline 4: 1344 bytes
pipeline 5: 1344 bytes
pipeline 6: 1344 bytes
pipeline 7: 1344 bytes
pipeline 8: 1344 bytes
```
## Runtime strategy
```
[group_join] dense path: keys in [1, 150000] (range 150000, 32-bit slots), preserved rows 150000 (null keys 0), counted rows 1483918
[group_join] emitted 150000 group rows (dense strategy)
```

## Verdict

PASS: reservation set, plan, fusion, strategy, and results identical modulo the rename.
