# S4 validation record

This is the historical first-handoff record. Current cleanup, fresh local validation and the final four-GPU handoff are recorded in [S4_LOCAL_FINALIZATION_VALIDATION.md](S4_LOCAL_FINALIZATION_VALIDATION.md) and [S4_FINALIZATION.md](S4_FINALIZATION.md). Original paths and outcomes below are retained as history.

Status: implementation in progress; four-GPU experiments pending. No topology winner is claimed.

## Combined prerequisite baseline

The comparison tree and Git/index arrangement are recorded in [S4_BASELINE.md](S4_BASELINE.md).
The isolated worktree was configured in the existing Pixi environment with CUDA architecture
`120a-real`, reusing the existing cuco and Corrosion source caches. Native build targets are
`sirius_library` and `sirius_unittest` (24 parallel jobs).

Local host: two NVIDIA RTX PRO 4500 Blackwell Server Edition GPUs, 32,623 MiB each.
Both were idle before validation. Parent alone schedules GPU work.

- CMake configure: passed; log `/tmp/sirius-s4-configure.log`.
- Full native baseline build: passed; log `/tmp/sirius-s4-baseline-build.log`.
- Isolated migration CUDA fault tests: passed, 228 assertions in 4 test cases; log
  `/tmp/sirius-s4-baseline-fault-tests.log`. Command:
  `pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml build/release/test/cpp/sirius_cuda_fault_test --order decl`.

These baseline checks precede S4 source edits. S4-specific results will be recorded separately.

- Focused cumulative GPU suite: passed, 5,572 assertions in 89 test cases; log
  `/tmp/sirius-s4-baseline-tests.log`. Selection:
  `[dynamic_filter][multi_partition],[in_task_publication],[pool_peer_access],[fused_membership],[fused_consumer]`
  with `--order decl`. Includes normal-SQL multi-partition parity, two-GPU publication,
  allocation/refusal/retirement, actual pool grants, and S3c fused consumer checks.
- Frozen native baseline artifacts: `build/s4-prerequisite-baseline/`, with SHA256SUMS.
- Matching baseline DuckDB wrapper build: passed; log
  `/tmp/sirius-s4-baseline-extension-build.log`.

The production developer's edit gate was released only after the native baseline suite passed.

- Matching CLI/extension smoke: passed with explicit
  `SIRIUS_CONFIG_FILE=test/cpp/integration/integration-2gpu.yaml`, `-bail -no-agent -csv`.
  Normal SQL returned `1429,7146429`; log
  `/tmp/sirius-s4-baseline-extension-smoke-configured.log`.
  `ldd` resolved `libsirius.so.0` to the frozen baseline install. The initial default-config
  attempt failed during LOAD with a 32,005,337,907-byte GPU allocation refusal; its subsequent
  CPU result is excluded. The smoke establishes wrapper/load compatibility; GPU interception
  and parity evidence comes from the native integration tests, not this result alone.

## S4 control: initial native fixture checks

Control library and native fixture build passed after qualifying global CUDA namespaces exposed
by the new copy-helper include. The reviewer identified a constructor-failure lifetime issue;
stream registration now follows successful owner construction, and fault regression coverage
is included in the expanded tests (runtime result pending).

Parent-run initial checks, all exit zero with `status=ok` and byte/CPU oracles passed:

- Builder force-host: GPUs 0,1; root 1; two keys; B=2,097,408 and C=2,097,152 bytes;
  four replicas checked. Physical HOST charge and root scratch were each 2,097,152 bytes.
  `/tmp/sirius-s4-host-smoke.jsonl`.
- Builder require-direct: same geometry; root 0; exact replicas and no false negatives.
  `/tmp/sirius-s4-direct-smoke.jsonl`.
- Actual-pool transfers: simultaneous all-pairs destination-issued reads and root-outbound
  source-issued writes; 1,048,832 bytes per edge; every destination and immutable source verified.
  `/tmp/sirius-s4-transport-{read,write}-smoke.jsonl`.
- Normal SQL Q4, Q21 and Q7 on the repository's tiny Parquet fixtures: CPU-result parity and
  `gpu_intercepted=true`. These produced no accumulated publication; publication timing is null
  and the reason is recorded. These are runner/correctness checks, not S4 performance evidence.
  `/tmp/sirius-s4-sql-q4-smoke-internal.jsonl`, `/tmp/sirius-s4-sql-q21-smoke.jsonl`,
  `/tmp/sirius-s4-sql-q7-smoke.jsonl`.

The first SQL attempt exposed missing test-option registration. Existing internal SQL options
require `SIRIUS_ENABLE_TEST_OPTIONS=1`, as in unittest.cpp. The native fixture now scopes that
gate around DB construction and verifies the effective profile; the runner records it. This
gate does not select the publication schedule, which remains compiled into each binary.

The local driver reports peer read/write support between GPUs 0 and 1 over NODE connectivity;
actual-pool direct copy success above provides the relevant data-path evidence. Physical copy
routing and four-GPU performance remain unmeasured. Initial samples are not used to select a winner.

- Expanded control native build passed. First normal run: 94 cases, 92 passed, 2 failed;
  failures were pre-S4 pipeline expectations that direct scratch refusal necessarily skips
  publication, and that only one scratch size is requested. Those tests are being updated to
  distinguish smaller HOST fallback from refusal of both transports. Log:
  `/tmp/sirius-s4-current-tests.log`. This is not a green suite claim.
- Initial extended fault suite exposed a test interception gap: cuDF dispatches CUDA batch calls
  inside its shared library, beyond a link-time CUDA symbol wrapper. The isolated target now wraps
  Sirius's `device_copy_batch::enqueue` boundary and delegates to the real implementation.
  The corrected suite **passed 835 assertions in 7 cases**, including staged failures/delays,
  direct failures/delays, constructor-event failure and existing migration regressions.
  Log: `/tmp/sirius-s4-current-fault-tests-2.log`. Injected status tests prove safe successful
  draining; they do not emulate a real unjoinable device loss or a partially accepted descriptor batch.

## Final control validation before freezing paired builds

- Corrected focused suite: **94 cases, 6,970 assertions, all passed**;
  `/tmp/sirius-s4-control-final-tests.log`.
- Corrected fault suite: **7 cases, 835 assertions, all passed**;
  `/tmp/sirius-s4-current-fault-tests-2.log`.
- Extra native edge cases passed: one GPU/no transfer resources, a noncontributing replica
  GPU, empty input, and Q4 with fixture-owned option setup and `profile_verified=true`.
  Records: `/tmp/sirius-s4-fixture-edge-smokes/`.
- Repository pre-commit hooks passed on the S4 source delta (rumdl run separately in the shared
  Pixi environment); Markdown check passed for nine design/validation/module files.
- Harness developer reports 20 CPU tests passed, including a complete fake-fixture campaign,
  pairing/validation failures and provenance recording. These are CPU harness checks, not GPU evidence.
- Index comparison against prerequisite tree remains empty; HEAD remains
  `9c329e31f65be938bf9de4fc823bd29abf0359cc`. No commits or S4 staging occurred.

Paired artifact creation is now running through the delivered build helper, with output
`build/s4/` and log `/tmp/sirius-s4-paired-build.log`. Candidate, sanitizer and actual paired-runner
validation remain pending; no four-GPU performance result is implied by the control checks.

## Frozen paired builds and candidate validation

The build helper completed both schedules and produced `build/s4/build-manifest.json`.
Log: `/tmp/sirius-s4-paired-build-2.log`. The first attempt was correctly rejected by
the manifest because the wrapper retained a cached `sirius_DIR` from the prerequisite
build. The helper now clears that cache entry on each wrapper configure; both wrappers
relinked and resolve their own frozen install. The rejected output is preserved separately
in `build/s4-rejected-wrapper` and supplies no measurement evidence.

The reviewer independently checked artifact hashes, wrapper RPATHs, matching source and
configuration identities, and the compiled schedule. Exactly one production translation
unit has `SIRIUS_S4_CONCURRENT_INGRESS=1`: `sirius_dynamic_bloom_filter.cu`.

- Candidate focused tests: **94 cases, 6,970 assertions, all passed**;
  `/tmp/sirius-s4-candidate-tests.log`.
- Candidate isolated fault tests: **7 cases, 835 assertions, all passed**;
  `/tmp/sirius-s4-candidate-fault-tests.log`. This includes the reviewer-found
  late-construction ingress-owner regression in the candidate build.
- Parent reran the Python harness checks: **20 tests passed**;
  `/tmp/sirius-s4-harness-final-tests.log`.

Compute Sanitizer 2026.3 memcheck used `--error-exitcode 86 --report-api-errors explicit`.
All six invocations exited zero and reported **0 errors**:

- Both compiled schedules, each with require-direct and force-HOST builder publication:
  GPUs 0,1; root 1; two keys; B=8,388,864 bytes; 4,096 rows per contributor;
  no warmup and one measured repetition. Complete-byte and membership oracles passed.
- Candidate `[s4_publication_fault]`: **3 cases / 607 assertions**, including delayed
  copies, failure draining, and late preparation failure.
- Candidate late-wait cancellation and completed-wrapper retention: **2 cases / 54 assertions**.

Exact commands, exit statuses, raw logs, and fixture JSONL are in
`/tmp/sirius-s4-sanitizer/`, indexed by `results.json`. This is bounded two-GPU coverage;
it does not establish four-GPU concurrent-ingress performance or exhaustive device-loss behavior.

The complete local paired runner is now executing transport, builder, and tiny-data SQL
with both schedules and baseline/baseline controls. Its output is
`build/s4-results/local-smoke`; runner log `/tmp/sirius-s4-paired-smoke.log`.
Final integration and four-GPU handoff verdict follow after that campaign and analysis.

## Actual paired runner and analysis

The first real campaign completed successfully on the local two-GPU host:

```bash
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/run_s4_experiments.py screen \
  --stages transport,builder,sql --gpus 0,1 \
  --build-manifest build/s4/build-manifest.json \
  --data-path test/cpp/integration/data/parquet --scale unknown/tiny --hash-dataset \
  --profile default256 --queries 4,21,7 --warmups 1 --pairs 1 --seed 1823 \
  --output build/s4-results/local-smoke
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml python scripts/analyze_s4_experiments.py \
  --results build/s4-results/local-smoke --output build/s4-results/local-analysis.json
```

Both commands exited zero. Both frozen extension/CLI loader checks passed.
The campaign contains **50 native processes and 100 samples** (one warmup and one
measurement per process), all `status=ok`, with passing full oracles:

| Mode | Samples, including warmups | Measured samples | Evidence |
| --- | ---: | ---: | --- |
| Transport | 28 | 14 | All ordered pairs, all-pairs, each root inbound/outbound, both issuing directions |
| Builder | 48 | 24 | Both schedules, baseline/baseline controls, direct and forced HOST, tails and keys |
| SQL | 24 | 12 | Q4/Q21/Q7, both comparisons, exact CPU parity, verified GPU interception |

All 24 SQL samples verified GPU interception and profile settings. None published
an accumulated filter on the tiny dataset: publication durations are null. The
analyzer excludes those from publication gains, reports the local screen as
exploratory, and does not pass the four-GPU or all22 regression gates. No topology
or speedup is selected from this integration run.

Raw bundle: `build/s4-results/local-smoke/`; report: `build/s4-results/local-analysis.json`;
runner log: `/tmp/sirius-s4-paired-smoke.log`.

## Transfer and review state

Source is exported without commits in `/tmp/sirius-s4-handoff.tar.gz`: prerequisite
patch, unstaged S4 patch, new files, reconstruction README, and file hashes. The
prerequisite patch was applied in a separate fresh checkout of S3c and reproduced
tree `9f59d68e4cdc7ca47999c3579407f6f5168ea652`; the S4 patch applied cleanly.
SHA-256 comparison also reproduced all **37 changed/new files** (18 tracked and
19 new) byte-for-byte in that checkout. The source archive includes its own checksums.
`/tmp/sirius-s4-validation.tar.gz` carries local raw evidence without build binaries.

The implementation remains uncommitted on `stacked/df/04-publication` in
`/tmp/sirius-s4-publication`. The original user worktree is unchanged.
The independent review verdict is in [REVIEW.md](REVIEW.md). The next required
external step is the user's four-GPU screen from [S4_EXPERIMENTS.md](S4_EXPERIMENTS.md).
Final topology selection, conditional advanced-topology pilots, pruning, and final
production confirmation remain after that evidence, within the same planned S4 PR.

The reviewer completed source, manifest, and raw-result inspection and marked the
first four-GPU handoff ready. Final Markdown and diff-whitespace checks passed.
No four-GPU run or final performance decision has been performed locally.
