# S4 final four-GPU validation instructions

Validate the pruned final S4 implementation against the exact pre-S4 prerequisite, then make the separate smaller frozen-control comparison. The retained implementation is the existing direct pipeline plus bounded HOST fallback. The earlier concurrent-ingress pilot was rejected after screening; do not rerun that pilot or implement another topology in this task.

## Establish source and scope

Start from the final checkout supplied by the coordinator. Read repository `AGENTS.md`, [S4_FINALIZATION.md](S4_FINALIZATION.md), and [S4_EXPERIMENTS.md](S4_EXPERIMENTS.md). The experiment guide contains the executable worktree, overlay, build, correctness, campaign and analysis commands. [S4_FIXTURE_NOTES.md](S4_FIXTURE_NOTES.md) defines native records. [S4_EXPERIMENT_RESULTS.md](S4_EXPERIMENT_RESULTS.md) and its archived evidence remain historical screening results, not final acceptance.

Use `experiment/s4-final-validation-20261023` on `https://github.com/kevkrist/sirius.git`. The coordinator verifies that remote snapshot before handing it off, while preserving the local review worktree's HEAD/index/unstaged cleanup. Use the guide's fetch/checkout commands, record the clean measured HEAD, and build fresh remote artifacts from that snapshot.

Record the final commit, clean source status, dependency revisions and start time. Keep those sources fixed until measurements finish. Use isolated persistent reference worktrees. The prerequisite must be exactly `78a3e34c66143152ff075729f4c018806c11e67d`, tree `9f59d68e4cdc7ca47999c3579407f6f5168ea652`; the frozen S4-control reference must be `9b2e8201787f381b62715c01405f659b925c49a7`. Initialize pinned build submodules in every worktree.

Install only the guide's allowlisted SQL fixture overlay in reference worktrees. Baseline production source and core-target definitions must remain unchanged. The helper verifies this before building. If compilation needs a production compatibility edit, preserve the error and return it to the coordinator. Do not silently weaken the baseline.

New manifests and samples use schema v2, `revision_role=baseline|final` and explicit `baseline_kind=prerequisite|s4_control` provenance. Frozen S4 control is already S4: its comparison checks pruning and cannot replace prerequisite-versus-final validation. Schema-v1 records retain their original meaning and use the committed original analyzer as documented in the guide.

## Establish hardware, data and immutable artifacts

Use the user's assigned four GPUs, with explicit CUDA-visible ordinals. Record any visibility mapping, UUIDs/models/memory, PCI topology, peer capability, driver/toolkit, CPU/NUMA affinity, clocks/power and concurrent activity. The runner captures boundary snapshots. Keep processes sequential in an idle window. Driver peer capability is not physical-route evidence or proof of access to actual allocator pools.

Use the actual TPC-H SF100 Parquet directory or DuckDB database. Record generator metadata and independently counted table rows where available; do not infer scale from the directory name. The parent verified its own two-GPU dataset, which does not establish the identity of this host's data. Missing SF100 blocks SQL performance acceptance; tiny fixture data remains explicitly `unknown/tiny`.

Execute the guide's **Build immutable revision artifacts** commands. Use this host's architecture (`native` or its correct explicit architecture), matched dependencies and identical extra CMake flags in every arm. Reusable fetch-source overrides are supported; retain the exact raw options and activated package environment. Build the prerequisite/final pair, then only the control baseline and pair it with the already-frozen final artifacts. Do not rebuild final unnecessarily.

Before timing, run the final focused publication tests and isolated fault executable from the guide, plus applicable original reference correctness checks and the CPU harness tests. Save exact commands, exit statuses, counts and skip reasons. Record any coordinator-requested bounded sanitizer coverage separately. A failed build, identity check, oracle, effective-profile check or GPU-interception check blocks performance interpretation. Preserve failures and report the concrete blocker.

## Run final diagnostics and revision comparisons

Follow **Four-GPU final acceptance** in the guide. Required work is:

1. Final-only builder screen, including positive forced HOST, followed by held-out one/two-key sizes, tails and representative/adverse roots. Preserve full bitmap equality, membership and workspace evidence. The prerequisite has no compatible builder diagnostic; do not invent a comparative baseline. Stage A transport is optional final-host diagnostic evidence, not a new topology campaign.
2. Final versus prerequisite, normal SQL all22 at verified SF100/default256. This subsumes Q4/Q7/Q21. Use three warmups, five fresh independent process pairs and six measured repetitions per process, with randomized arm order and separate baseline/baseline controls.
3. Separate final versus frozen S4 control, Q4/Q7/Q21, using the same process-cluster design when making a non-regression conclusion.

Run the analyzer and retain its JSON report. The per-query slowdown upper-bound guard is 3%; do not average away a regressing query. Five process-pair summaries provide independent observations, not thirty IID repetitions. Baseline-control drift makes a result inconclusive. Use only the documented single five-cluster extension with a fresh seed and linked original evidence when justified; preserve both original and extension results. Do not repeatedly rerun and select favorable outcomes.

The prerequisite lacks S4 publication preparation timing and byte/transport counters. Unsupported fields are null, even when its common publication count is positive. Compare query end-to-end time only with matched common execution/publication/contribution outcomes. Changed work is explicitly excluded from overhead attribution. Never substitute the old final-contribution-to-first-push latency for S4 preparation time. Q4 can be a nonpublication control; final Q7/Q21 must demonstrate positive publication on this acceptance host.

Verify successful normal-SQL GPU interception and CPU parity under `exact_except_q1_avg_disc_v1`. Only Q1's `avg_disc` DOUBLE column permits finite relative or absolute differences up to `1e-12`, with native schema/type/shape and exact unique key checks; all other cells remain exact. The shared `s4_experiment_sql_oracle.hpp` must match across all three fixtures. Identify and SQL samples must bind the fixed `oracle_policy`, `oracle_rel_tol`, and `oracle_abs_tol` values to their manifests. Accepted Q1 raw digests may differ. Do not broaden this policy after another mismatch. A CPU result after failed LOAD, a fallback query, or a profile mismatch is not performance evidence. The loader smoke uses `-bail -no-agent` and must resolve the matching frozen shared library. Native static fixture identity is a separate artifact path, not dynamic shared-library proof.

## Return evidence

Create a new `S4_FINAL_RESULTS.md` and a unique directory under `docs/experiments/s4-results/`. Preserve the historical `S4_EXPERIMENT_RESULTS.md` and its evidence unchanged. Report:

| Section | Required evidence |
| --- | --- |
| Conclusion | Passed guards, inconclusive, changed work, or blocked; exact remaining limits. Separate prerequisite and cleanup comparisons. |
| Reproduction | Final/reference revisions and trees, fixture overlay hashes, package/dependency identity, complete commands/options, seed, config, data identity/scale, artifact hashes and timestamps. |
| Correctness | Per-revision checks/counts/skips, exit statuses, complete oracles, interception/profile evidence and both linkage paths. |
| Final diagnostics | Every builder workload, roots/keys/tails, actual direct/HOST behavior, bytes/workspace, statuses and raw timings. Include optional transport observations without physical-route claims. |
| SQL | Every query in each comparison, cluster counts, median/p90/interval, baseline drift, common-counter equivalence, exclusions and individual 3% guard. |
| Limits | Missing datasets/metrics, fallback/refusal/changed work, failed attempts, unmeasured clocks/routes/stages/allocator peaks and deviations. |

Include the analyzer JSON and compressed complete campaign directories: untouched native JSONL, aggregate samples, requests, frozen SQL/config, loader logs, machine snapshots and completion/failure records. Include all three build manifests, pair manifests, raw commands and per-revision CMake caches. Preserve source artifact hashes from `experiment.json.harness_sources` and include the matching measured harness files. Keep older strict schema-v2 campaigns under their original manifests and measured harness. Provide checksums and a map from original absolute paths to returned archive locations; do not rewrite originating manifests. Verify archive completeness and rerun the matching analyzer on the extracted evidence.

Exclude datasets, built binaries, environment directories and credentials from the returned source/evidence package. Keep unsuccessful runs and explain any retry. Do not overwrite the archived pilot report or claim final acceptance from the local two-GPU checks.

## Commit and push the results back

The user explicitly authorizes the agent on the experiment machine to commit and push **only `S4_FINAL_RESULTS.md` and its new unique evidence directory** to the provided personal-fork final-validation branch on `kevkrist/sirius`. This result return does not require another approval. The coordinator will fetch that result commit; final implementation acceptance and PR decisions remain with the coordinator. The coordinator's local review worktree keeps its HEAD, index and unstaged source changes unchanged.

Return results to `experiment/s4-final-validation-20261023` on `kevkrist/sirius`. Set `S4_RESULT_REMOTE` to its personal-fork remote and `S4_EVIDENCE_DIR` to the new relative directory used for this run. Record the measured source commit before creating the result commit. Check that the push remote is exactly `kevkrist/sirius`, that no unrelated changes are staged, and that the evidence directory is new:

```bash
(
set -e
S4_RESULT_BRANCH=experiment/s4-final-validation-20261023
: "${S4_RESULT_REMOTE:?Name the personal-fork remote}"
: "${S4_EVIDENCE_DIR:?Use this run's new directory under docs/experiments/s4-results}"
git remote -v
case "$(git remote get-url --push --all "$S4_RESULT_REMOTE")" in
  https://github.com/kevkrist/sirius.git|https://github.com/kevkrist/sirius|git@github.com:kevkrist/sirius.git) ;;
  *) echo 'Push remote must be kevkrist/sirius'; exit 1 ;;
esac
case "$S4_EVIDENCE_DIR" in
  docs/experiments/s4-results/*) ;;
  *) echo 'Evidence must be under docs/experiments/s4-results'; exit 1 ;;
esac
test -z "$(git diff --cached --name-only)" || exit 1
test -z "$(git ls-files -- "$S4_EVIDENCE_DIR")" || exit 1
git check-ref-format --branch "$S4_RESULT_BRANCH"
git add -- S4_FINAL_RESULTS.md "$S4_EVIDENCE_DIR"
git diff --cached --stat
git commit -m "Report final S4 four-GPU validation"
git push "$S4_RESULT_REMOTE" "HEAD:refs/heads/$S4_RESULT_BRANCH"
git rev-parse HEAD
)
```

Use a normal fast-forward push, never force-push or push upstream. If the remote advanced, fetch and inspect it before integrating; preserve the recorded measured-source identity and all returned evidence. Send the coordinator the result commit SHA, fork branch URL, result-file URL and brief conclusion so it can fetch and review the results. This authorization covers the remote result commit only, not source edits, an upstream push or a PR.
