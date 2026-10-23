# S4 finalization contract

2026-10-23. Final architecture disposition: **approved for the four-GPU validation handoff, with no remaining architecture findings**. The losing concurrency pilot is pruned, bounded HOST/direct publication is retained, and local revision comparisons plus the narrow Q1 oracle correction have passed. Rawls's final review is green with no findings. Parent packaging/publication remains separately owned. Local review changes remain unstaged and uncommitted; the authorized fork snapshot uses a separate Git index/ref, not another worktree, and does not move local HEAD or modify the local index/review diff. Four-GPU acceptance is pending the user's runs; this is not a remote non-regression claim.

## Decision and references

Retain the existing double-buffered direct pipeline plus required bounded HOST fallback. The [four-GPU screen](S4_EXPERIMENT_RESULTS.md) found roughly 48–55% larger-builder slowdown and 47% Q7/Q21 publication slowdown from concurrent ingress, with equivalent work. Its small-case gain was only 3.3%. Reject that pilot without spending held-out runs confirming it. No tree, ring, stripes, alternate reducer, or source-push production implementation is authorized.

Full-partial replication moves 12B versus the existing 6B; the measured matching transfer took 20.86–21.93 ms while the 64 MiB control builder took 9.08–9.27 ms including OR. This screens out that proposal under measured conditions, not on every possible host. Stage timings and physical bottleneck causes remain unmeasured. The optimized reference `ef17c8c55003dbc889e93c24eedf55f127f22c7a` remains historical implementation context; do not transplant rejected mechanisms from it.

| Artifact/reference | Meaning |
| --- | --- |
| Final worktree | `/home/ubuntu/sirius/build/s4-final`, `stacked/df/04-final`, starting at `d925db5dcb909411598b57c566fd9888471d151c`; new cleanup is uncommitted |
| Exact prerequisite | `/home/ubuntu/sirius/build/s4-prerequisite`, detached `78a3e34c66143152ff075729f4c018806c11e67d`, production tree `9f59d68e4cdc7ca47999c3579407f6f5168ea652` |
| Frozen S4 control | Measured production source `9b2e8201787f381b62715c01405f659b925c49a7`, `current_direct`; `d925db5d` carries the returned evidence. Parent pins the reference production tree/configuration before rebuilding it. |
| Dependency | cuCascade `1a9281093a0fe49634594d541d7ce327129f53d6` in every comparison |

Two questions require separate comparisons. **Final versus prerequisite** measures the complete S4 change, including automatic fallback preparation and instrumentation. **Final versus frozen S4 control** checks that pruning preserves the already-screened control. The latter already contains S4 and cannot establish the former's non-regression. Neither comparison is final versus the rejected concurrent pilot.

## Smallest production cleanup

The production owner removes concurrent-ingress branches, its extra streams/events/registration/capacity, the CMake schedule selector, `compiled_publication_schedule()`, and `publication_measurement::schedule`. Remove or revise pilot-only tests and comments, including fault ordinals affected by deleting event construction; retain tests of direct/HOST draining, canonical equality, cancellation, admission, exact accounting, tails and key transitions.

Keep these existing interfaces unchanged:

- `finish(root, stream, scratch_mr)`: the ordinary automatic publication entry point.
- Detail-only `finish_for_experiment(root, stream, scratch_mr, publication_transport, measurement*)`: retain `automatic`, `force_host`, and `require_direct` for correctness and measurement. These select transport diagnostics, not alternate algorithms. Both entries use the same production mechanism; no signature consolidation, new scheduler interface, or adapter layer is needed.
- `inspect_partial`/`inspect_replica` and useful fixed-field measurements/counters: retain where existing correctness or diagnostic consumers need them. Actual transport and planned/submitted-byte distinctions remain useful without a schedule label.

Preserve the current direct source-disjoint halves and global chunk sequence, bounded HOST window rounded to the physical block size, task-owned root scratch without duplicate reservation, and pre-mutation fallback choice. A typed CUDA failure still drains and propagates; it does not select another transport. Preserve all reachable owners when work cannot be joined. Do not weaken these rules while deleting the pilot's additional owners.

Apply Document Code only to comments made inaccurate by these removals or new test-only compatibility constraints. Leave accurate existing comments unchanged; do not replace deleted candidate comments with benchmark history. Production explanatory comments should describe retained ownership/ordering, not the experiment decision.

## Minimal prerequisite comparison seam

**Use a SQL-only test fixture overlay; do not backport S4 diagnostic interfaces or counters into prerequisite `src/`.** Normal SQL executes that revision's actual engine and its production publication/`finish` path. This supplies the necessary end-to-end comparison without manufacturing a baseline `finish_for_experiment`, `inspect_partial`, or measurement record.

Parent owns overlay installation and both fixture CMake registrations. The allowlist is:

- `test/cpp/operator/s4_experiment.cpp`
- `test/cpp/operator/s4_experiment_common.hpp`
- `test/cpp/operator/s4_experiment_sql.cpp`
- `test/cpp/operator/s4_experiment_sql_oracle.hpp` (shared, test-only oracle correction described below)
- New test-target registration `cmake/sirius-s4-baseline-fixture.cmake`
- One test-only include in root `CMakeLists.txt` under the existing test-build gate.

Compile common/main/SQL sources from the final harness against the prerequisite's own `sirius_core`, DuckDB, loader and dependencies. Do not compile `s4_experiment_gpu.cpp` into this baseline target. The same overlay bytes may be compiled against the frozen S4-control reference for the smaller cleanup comparison. No baseline production file or core-target compile definition is changed; record overlay hashes separately from the exact production tree. If a build compatibility issue requires more than this allowlist, report it to the parent rather than silently modifying the baseline.

Freeze these names with fixture/harness owners:

| Name | Contract |
| --- | --- |
| `SIRIUS_S4_BASELINE_SQL=1` | Target-private definition on the test executable only; excludes GPU-mode dispatch, implementation files and dependencies. Harmless shared forward declarations may remain. Never selects engine behavior. |
| `SIRIUS_S4_REVISION_ROLE` | Target-private quoted `baseline` or `final`, used only as artifact metadata. |
| `schema_version=2`, `revision_role` | Samples and identify output use the same two-role protocol. These roles replace algorithm names for new comparisons. |
| `baseline_kind` | Manifest provenance, `prerequisite` or `s4_control`; no third runtime role or separate comparison framework. Each manifest contains one baseline and one final artifact. |
| Identify information | `identity_source=fixture_build_metadata`, actual executable path/linkage, fixed supported-mode list, and `publication_metrics_supported` boolean. Parent manifests supply/verifiably bind revision, tree, overlay/configuration and binary hashes. No capability plugin system. |

Both revisions use the same SQL setup, query text, CPU oracle with the narrowly quantified exception below, normal GPU interception checks, and settled common-counter deltas. Use a small dependent `requires` check in the test-side statistics writer for fields absent in the prerequisite. Unsupported S4-only bytes, transport counts, cap/resource subdivision and prepare-through-completion timing are **null/unsupported**, never zero. Common counters include accumulation starts/completed contributions, actual publication count, filters pushed and existing admission/inventory/domain skips. Match effective cap, coverage, pinning and requested devices as well as common outcomes.

The pre-S4 `accumulation_publication_latency_ns` has its original final-contribution-to-first-push meaning. It is not the S4 preparation interval. Do not compare those timings or claim numerical prerequisite publication bytes/latency from geometry guesses. SQL end-to-end time, exact results and common execution outcomes are the cross-revision comparison. A changed publication outcome must be investigated or reported as changed work, not attributed solely to transfer overhead.

Baseline `builder`/`transport` modes are unavailable. Final-only builder diagnostics keep the existing diagnostic finish call and measured workspace/bytes, with canonical snapshots outside the timer. There is no comparative builder requirement and no cross-revision builder adapter. This avoids throwing away useful final measurements merely to force every test through the thin three-argument wrapper.

### Quantified Q1 oracle correction

The strict final SF100 Q1 preflight failed. A parent-run reproduction using the frozen CLI, identical preparation/configuration, and repeated GPU execution localized the mismatch to `avg_disc` (DOUBLE, zero-based column 8): two cells differed by `1e-17`, approximately `2e-16` relative, reproducibly. All four row identities, counts, DECIMAL sums and other AVG cells were exact. Diagnostic evidence is retained under `build/s4-local-validation/q1-diagnostic/`; this establishes a test-oracle rounding issue for those cells, not a reason to change production execution.

The reviewed correction uses one compact shared helper, `s4_experiment_sql_oracle.hpp`, in every reference/final SQL fixture. It permits approximation only for the recognized Q1 schema's `avg_disc` DOUBLE column (zero-based index 8), after validating the expected column names/order, equal complete CPU/GPU types and shape, and exact unique `(l_returnflag,l_linestatus)` keys. Exact sorting with this verified leading key is sufficient; no approximate multiset matching is introduced. Every other cell, including Q1's other AVG columns, remains exact. Nulls retain their distinct tags. Nonfinite values match only through exact equality. No producer or library runtime change accompanies this test-only correction.

For the eligible finite DOUBLE cell, use the existing DecimalSum fixture convention: `abs(a-b) <= 1e-12 * max(abs(a),abs(b))` **or** `abs(a-b) <= 1e-12`. Both numbers must parse completely. This is a fixed test policy, not a configurable tolerance or a policy for all floating-point output. Any further mismatch is investigated before widening scope.

Identify/sample metadata records the fixed `oracle_policy=exact_except_q1_avg_disc_v1`, `oracle_rel_tol=1e-12`, and `oracle_abs_tol=1e-12`; the harness validates their manifest binding and constants. Keep raw digest algorithms and values unchanged: accepted Q1 digests may differ. The native typed comparator remains the oracle; add no digest-equality requirement or per-cell proof framework. Hash the shared header and add only that header to the existing baseline overlay allowlist.

Focused comparator tests exercise the actual shared seam: they accept the observed differences and reject out-of-tolerance values or mismatches in other Q1 AVG columns, DECIMALs, counts, keys, types, nulls, shape or duplicate row identities. Nonfinite/partial-parse cases cannot pass through tolerance; non-Q1 workloads remain exact. The parent reported the red run failed exactly the three expected positives (the observed difference and the relative/absolute tolerance positives), with the 40 controls passing. The green run then passed 6 cases/43 assertions; all 30 CPU harness tests and the new-source hooks passed. Refreshed artifact pairs were built and validated, and `oracle-library-proof.json` records byte-identical old/new production-library SHA-256 values for all three revisions. Refreshed Q1 now passes across all three: the current runner's four-process prerequisite/final campaign completed and was analyzed, and the frozen S4-control revision's updated native fixture exited zero. Oracle, effective-profile and GPU-interception checks passed; raw CPU/GPU digests remain honestly different where the permitted rounding difference occurs.

The parent also ran Q8/Q14/Q17 against the original frozen strict final fixture: one SF100 process per query, zero warmups and one repetition, with matched configuration. All passed, with no timing claim and no reason to expand the Q1-only exception. The original 96 strict-oracle comparison samples and their manifests remain valid evidence for their recorded artifacts. The exact measured runner/analyzer/build-helper bytes were recovered, hash-verified and preserved in `measured-harness`; do not relabel those campaigns as new-oracle results. Refreshed artifact/protocol identities, focused CPU checks and Q1 validation passed; production is unchanged, so another 94-case production run is not required solely for this helper change. Rawls's final review is green with no findings. No further architecture-driven code, policy expansion or local validation is requested; four-GPU final acceptance remains a subsequent user run.

The parent verified all 1,017 files in the evidence archive against their hashes. Each of the two original strict campaigns was reanalyzed separately with its preserved measured harness, and the new Q1 campaign with the current harness; reports matched after directory-only normalization. This preserves the distinction between old strict-oracle evidence and refreshed Q1 evidence. Authorized publication uses a separate Git index/ref on the agreed fork branch, not a new worktree, preserving local HEAD, index and the unstaged review diff.

## Harness and immutable comparisons

Use the existing runner/analyzer with two-arm `baseline_final` and `baseline_baseline` comparisons. Run separate campaigns/manifests for each `baseline_kind`. Production revision differences are intentional; require equal toolchain/dependencies/effective configuration after narrowly normalizing source/build/install roots and test-only overlay definitions. Do not weaken checks by ignoring arbitrary CMake differences.

Hash production changes, overlay bytes, configuration, fixture/library/extension/CLI and resolved linkage. Native fixtures statically link their revision's production objects; extension/CLI loading is a separate matched-artifact check. Do not label an artifact with the removed `concurrent_ingress` identity to satisfy old tooling. Preserve old schema-v1 evidence unchanged and keep it analyzable using explicit v1 support or its frozen analyzer.

After cleanup, the build helper builds revision roles, not algorithm variants. Freeze all sources before paired builds. Baseline production must still match `9f59d68e...`; the frozen S4-control must still represent its recorded current-direct source/configuration. Parent owns build execution, local GPU scheduling and final evidence assembly.

## Local smoke versus remote acceptance

The parent independently verified local SF100 using generator metadata and Parquet footer counts, including 600,037,902 lineitem and 150,000,000 orders rows. This is real SF100; no tiny-dataset inference is needed. The local host has **two 32 GB GPUs**, unlike the screened **four approximately 46 GB GPUs**. Choose and freeze a matched two-GPU configuration for all local arms, cap **256 MiB**, coverage **0.9**, and explicit pinning/memory budgets. Do not reuse the old S3c 0.45 profile. If memory changes execution, record it rather than silently shrinking the dataset or relabeling fallback.

Minimum local plan, entirely parent-coordinated:

1. Build/identify both baseline kinds and final; verify exact production/overlay identities and matched extension/CLI loading. Run CPU harness/schema checks. No runtime algorithm selector remains in final.
2. Run final focused publication/fault coverage and bounded native direct/HOST exact-oracle cases: roots 0/1, one/two keys, tail and cross-key half reuse, admission failure and cancellation/drain cases already covered. Re-run relevant sanitizer coverage when final changes require it; report precise scope.
3. For **each** SQL comparison, run q4/q7/q21 at verified local SF100: one fresh process-pair cluster, one warmup and three measured repetitions per arm, plus matching baseline/baseline control. Use the same inputs/configuration and randomized arm order. This is operability/parity smoke, not a confidence interval or performance acceptance. If configuration cannot admit the intended workload, preserve that outcome and stop the timing claim.

A local CPU fallback, failed interception/profile check, or wrong-result result is not a performance sample. Zero accumulation publication permits an end-to-end correctness observation but no publication-speed claim. Q4 may legitimately supply this nonpublication control. q7/q21 positive publication must be demonstrated on the final acceptance host; local two-GPU success or failure cannot establish four-GPU memory layout, critical-path behavior, or the 3% guard.

Final **user-run four-GPU handoff** must provide runnable commands and frozen manifests for:

- Final-only canonical direct and forced-HOST diagnostics, held-out 8/128 MiB/tail and one/two-key cases within the combined cap. Keep no-false-negative and full per-replica byte equality checks. Stage A can remain an optional final-host diagnostic; do not repeat a broad topology campaign merely to prune known losing code.
- Final versus exact prerequisite: q4/q7/q21 and all22 TPC-H parity/end-to-end non-regression at declared SF100, with three warmups, five independent process-pair clusters and six paired measured repetitions per cluster. Baseline/baseline controls and effective-profile/interception checks remain mandatory. All22 may subsume its three overlapping queries; do not duplicate the same comparison unnecessarily.
- Separate final versus frozen S4-control cleanup comparison on q4/q7/q21, using the same paired method when asserting a non-regression conclusion. This smaller campaign checks pruning without substituting for prerequisite-versus-final acceptance.

The existing per-query slowdown upper-bound guard is 3%, using process-pair summaries and baseline drift checks. No 10% improvement is required to retain the original direct algorithm, and no 5% advanced-topology gate is relevant after pruning. If noisy, use the one predeclared five-cluster extension; do not selectively pool favorable repeats. No confirmation of the bad pilot or raised-cap topology experiment is required. Unmeasured route/stage/committed-peak data stays explicitly unavailable.

## Ownership, completion and readings

Production developer owns pruning and eligible code comments/tests. Fixture developer owns common/SQL compatibility and final diagnostics. Harness developer owns revision-role pairing, manifests, runner/analyzer and executable handoff guide. Parent owns CMake overlay/application, frozen references, all builds/GPU scheduling and final validation. Architect owns only this document and `S4_ARCHITECTURE.md`; no source edits or build/test/Git mutations are performed by the architect.

Stop this cycle when cleanup is reviewed, local bounded checks are recorded, both comparisons are runnable without baseline production edits, and the final four-GPU bundle is ready. Remote results and any later PR readiness claim remain pending the user's runs. Cleanup stays unstaged/uncommitted unless separately authorized.

Personal readings reused: Ousterhout chapters 2, 4, 5, 8 and 20; C++ Software Design chapters 1 (Guidelines 1–5) and 3 (11–14); C++20 chapters 5 and 7 as supplied. Refreshed their exact prior reading record, architect role, Codebase Design, relevant AGENTS instructions, screen results/tables and stopping criteria. Reread Document Code and its complete 441-line syntax reference; apply its preservation rule to eligible changed comments. New source reads: prerequisite builder/counter declarations, current fixture common/SQL/GPU call sites and measurement dependencies. No new full-book or broad architecture survey claim. These contracts incorporate the production/fixture/harness owners' compatibility findings and the parent's verified local dataset/host facts.
