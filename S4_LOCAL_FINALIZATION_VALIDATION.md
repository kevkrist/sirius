# S4 finalization: local validation

2026-10-23. The concurrent-ingress pilot is removed. The retained implementation is the existing double-buffered direct pipeline plus bounded HOST fallback. Final four-GPU acceptance remains pending; see [S4_FINALIZATION.md](S4_FINALIZATION.md) for the decision and comparison contract.

## Revisions and environment

- Final source: uncommitted changes in `build/s4-final`, branch `stacked/df/04-final`, starting at `d925db5dcb909411598b57c566fd9888471d151c`.
- Prerequisite: `78a3e34c66143152ff075729f4c018806c11e67d`, exact production tree `9f59d68e4cdc7ca47999c3579407f6f5168ea652`.
- Screened direct control: `9b2e8201787f381b62715c01405f659b925c49a7` with its original `current_direct` setting.
- Each baseline has only the allowlisted SQL fixture overlay and test registration. No baseline production source was edited. The prerequisite correctly reports that S4 publication metrics are unavailable.
- Two RTX PRO 4500 Blackwell Server Edition GPUs, 32,623 MiB each, NODE connectivity, driver 595.91.07. Shared Pixi environment; GCC 15.3, CUDA compiler 13.4.92, architecture `120a-real`; cuCascade `1a9281093a0fe49634594d541d7ce327129f53d6`.

All three native builds and matching DuckDB wrappers passed. Both revision-pair manifests passed source, dependency, normalized configuration, binary and resolved-linkage checks. The two comparisons reuse the same frozen final binary. Artifact directories are `build/s4-final-pair/` and `build/s4-control-artifact/`; build logs are under `build/s4-local-validation/` in the final worktree.

## Correctness and failure handling

| Fresh parent-run check | Result |
| --- | --- |
| Prerequisite cumulative GPU suite | 89 cases, 5,576 assertions passed |
| Prerequisite isolated CUDA fault suite | 4 cases, 228 assertions passed |
| Final cumulative GPU suite | 94 cases, 6,970 assertions passed |
| Final isolated CUDA fault suite | 7 cases, 835 assertions passed |
| Python harness checks | 28 tests passed |
| Changed C++/CMake repository hooks | Passed |

The cumulative selection was `[dynamic_filter][multi_partition],[in_task_publication],[pool_peer_access],[fused_membership],[fused_consumer]`, with `--order decl`. Baseline logs are in `build/s4-prerequisite/build/s4-local-validation/`; final logs are in `build/s4-final/build/s4-local-validation/`, relative to the original repository.

Six valid native builder cases passed complete per-replica bitmap equality, membership and workspace checks: direct roots 0/1; forced HOST roots 0/1; one GPU using ordinal 1; and reordered devices 1,0 with a noncontributing replica. Cases include one/two keys, a tail, and reuse of both scratch halves across keys. Raw JSONL and commands are in `build/s4-local-validation/native/`. An initial request for a noncontributing *root* was rejected by the fixture's final-contributor precondition; its log is retained separately and is not a passing runtime case.

Four Compute Sanitizer memcheck invocations exited zero and reported zero errors:

- Direct and forced-HOST builder publication, each with two GPUs, root 1, two keys and 8,388,864 bytes per key.
- S4 publication fault/delay cases: 3 cases, 607 assertions.
- Late-wait cancellation and completed-wrapper retention: 2 cases, 54 assertions.

The sanitizer used `--error-exitcode 86 --report-api-errors explicit`. This bounded coverage does not emulate an unjoinable device loss or establish four-GPU performance.

## SF100 comparisons

Local generator metadata and actual Parquet footer counts agree at SF100, including 600,037,902 lineitem and 150,000,000 orders rows. The fixed local configuration uses both GPUs at 85% capacity, HOST capacity fraction 0.5, 256 MB scan/concat batches and 512 MB hash partitions/build tables. The Bloom cap is 256 MiB and coverage is 0.9 in every arm; the older S3c 0.45 profile is not used.

Each local comparison uses q4/q7/q21, one process-pair cluster, one warmup and three measured repetitions per arm, plus baseline/baseline drift controls. These are bounded execution and correctness checks, not a confidence interval or the final 3% acceptance guard. Builds and other GPU tests finished before SQL timings began.

Both comparisons completed: 24 native processes, 96 samples including warmups, and 72 measured samples. Every result passed exact CPU parity, GPU-interception and effective-profile checks. Both matched CLI/extension loader checks passed. The aggregate payloads match the untouched native JSONL, apart from the runner's additional `harness` metadata. Common execution/publication counters match in all measured pairs; no changed-work query pairs were excluded.

Observed median paired query-time changes (positive means slower):

| Query | Final versus prerequisite | Final versus screened direct control |
| --- | ---: | ---: |
| Q4 | -0.70% | -0.28% |
| Q7 | +0.60% | +0.29% |
| Q21 | +0.23% | +0.59% |

Baseline/baseline median paired drift ranged from -1.16% to -0.38%. There is only one independent process pair per workload/comparison, so these observations have no confidence interval and establish neither the 3% guard nor four-GPU acceptance. No large regression appeared in these local samples.

Final Q7 published one filter totaling 23,982,720 bytes; Q21 published three totaling 174,974,624 bytes. Q4 published none. The prerequisite's unsupported publication byte/timing metrics remain null; its actual common publication counts agree. The control comparison has publication metrics in both arms. Results are in `prerequisite-analysis.json`, `control-analysis.json` and their corresponding campaign/audit files under `build/s4-local-validation/`.

The source reviewer independently checked both artifacts and both completed campaigns. The next acceptance step remains the user-run four-GPU campaign in [S4_REMOTE_EXPERIMENTS.md](S4_REMOTE_EXPERIMENTS.md).

## Q1 oracle correction before handoff

An additional SF100 Q1 preflight exposed a strict string-comparison failure. A matched CLI diagnostic reproduced it twice: only two `avg_disc` DOUBLE cells differed by `1e-17` (about `2e-16` relative, one ULP). Keys, counts, decimal sums and other averages matched exactly. The original strict fixture also passed Q8, Q14 and Q17, so no broader floating-point exception was introduced.

The corrected fixture uses policy `exact_except_q1_avg_disc_v1`: only the recognized Q1 `avg_disc` DOUBLE column permits finite differences within a fixed relative or absolute `1e-12` tolerance. Schema, names, shape, unique exact grouping keys, row multiplicity, NULLs and all other cells remain exact. This follows the repository's existing decimal-AVG comparison precedent. Raw digests retain the actual values and can differ for that permitted comparison. The policy and both tolerances are recorded in fixture identification and SQL records; the runner requires them to agree across paired artifacts.

The focused regression first failed with the old exact comparator: 3 expected failed assertions, with all 40 negative/exact controls passing. The correction passed all 43 assertions in 6 cases. The updated Python harness passed 30 tests; repository source hooks passed. The preserved red header, test logs, diagnostic CSVs and failed original Q1 JSONL remain in `build/s4-local-validation/`.

New artifacts are in `build/s4-final-pair-oracle/` and `build/s4-control-artifact-oracle/`. Every production shared library is byte-identical to its originally measured counterpart, verified in `oracle-library-proof.json`; only the test fixture/protocol changed. The original 96-sample comparisons retain their original immutable binaries/manifests and exact measured harness bytes in `measured-harness/`, rather than being relabeled as measurements of the corrected fixtures.

Q1 passed with all three corrected revision fixtures. The current runner completed four prerequisite/final processes with passing oracle, profile and GPU-interception checks; the screened-control fixture passed its separate one-sample check. Original raw digests remain unequal for the permitted last-bit difference. These zero-warmup correctness preflights are not performance acceptance samples. The Q1 failure is resolved without modifying production code or relaxing any other query's oracle.

The [archived evidence](docs/experiments/s4-results/20261023T182408Z-local-finalization/README.md) contains all 1,017 raw files and original artifact metadata, with per-file checksums. Extraction and reanalysis reproduced both strict-campaign reports and the corrected Q1 report. The final source review found no blocking issue; local finalization is complete, and four-GPU acceptance remains pending.
