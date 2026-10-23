# S4: instructions for the four-GPU experiment agent

Run the initial four-GPU screen, analyze it, and push the evidence back to
`kevkrist/sirius`, branch `experiment/s4-four-gpu-20261023`. The coordinating agent
will fetch your results and decide the next implementation or confirmation step.
This is an experiment branch, not a production release or a request to open a PR.

## 1. Read and establish the starting point

Read this file, the repository's `AGENTS.md`, and [S4_EXPERIMENTS.md](S4_EXPERIMENTS.md)
before running commands. That guide owns the CLI details and experiment protocol.
Read the experiment phases and acceptance criteria in
[S4_ARCHITECTURE.md](S4_ARCHITECTURE.md) before interpreting results. Consult
[S4_IMPLEMENTATION_KICKOFF.md](S4_IMPLEMENTATION_KICKOFF.md) for the bounded pilot's
scope and [S4_FIXTURE_NOTES.md](S4_FIXTURE_NOTES.md) when investigating a native record.

The comparison is `current_direct` versus `concurrent_ingress`. Both include the
required bounded HOST fallback. The candidate changes root ingress scheduling;
Stage A's source-issued writes are transport diagnostics, not another compiled
production candidate. Tree/ring/distributed-OR implementations are not present.

Use a clean checkout of the branch. Record `git rev-parse HEAD` as the measured
source commit before building. Keep that commit checked out until measurements
finish; commit the report afterward. The prerequisite tree
`9f59d68e4cdc7ca47999c3579407f6f5168ea652` is preserved in the branch's merge ancestry.
It combines S3c with the actual migration PR #2302.

Initialize the build submodules from their pinned revisions:

```bash
git submodule update --init --recursive -- \
  duckdb cucascade substrait .claude/claude-tools \
  sirius-duckdb/vcpkg sirius-duckdb/duckdb sirius-duckdb/extension-ci-tools
git cat-file -t 9f59d68e4cdc7ca47999c3579407f6f5168ea652
```

The last command must print `tree`. Record the recursive submodule status.
The historical two-GPU checks are described in [S4_VALIDATION.md](S4_VALIDATION.md)
and [REVIEW.md](REVIEW.md). Their old `/tmp` paths are historical: that environment's
temporary files were lost. The production/fixture source was recovered and its
source hash matched both validated builds exactly; see [S4_PUBLICATION.md](S4_PUBLICATION.md).
Build fresh artifacts on this host and report your own test results.

Completion: clean source, exact source commit recorded, required submodules ready.

## 2. Confirm host and dataset, then build and check correctness

Use the four GPUs assigned by the user. Record their visible ordinals, any
`CUDA_VISIBLE_DEVICES` mapping, UUIDs, models, memory, driver/toolkit, topology,
power/clocks, and concurrent GPU activity. The runner captures machine evidence;
record any unavailable measurements explicitly. Coordinate an idle test window;
keep experiment processes sequential. The historical four-L40S host is not an
assumption about this machine.

Locate the user's actual TPC-H SF100 Parquet directory or DuckDB database. Report
how its scale is known. The runner does not generate/download SF100. If SF100 is
unavailable, run transport/builder with SQL absence recorded and report the missing
dataset; tiny fixture data cannot establish SQL performance. Use default256
(256 MiB cap, 0.9 coverage) and GPU pinning. Keep any raised-cap/SF1000 diagnostic
separate and label it; it cannot establish default-profile value.

Follow **Build the paired artifacts** in `S4_EXPERIMENTS.md`. Build on this host
with `--cuda-architectures native`, or its correct explicit architecture, and a
reasonable job count. Save build stdout/stderr and exit status. The helper must
successfully create `build/s4/build-manifest.json` and verify both schedule identities
and library resolutions. Keep production sources, fixtures, configuration, and
dependencies unchanged between the two builds and throughout measurements.

Run the guide's focused unit and isolated fault tests for both frozen schedules,
sequentially. Also run the Python harness tests from the guide. Save each command,
exit status, test/assertion count, and any skip reason. A failed build, identity
check, oracle, GPU interception check, or correctness test blocks performance
interpretation. Preserve the failure logs and report the blocker. If a source fix
is needed, return the diagnosis rather than silently changing the measured code.

Completion: two verified builds and passing applicable correctness checks, or a
specific blocker with reproducible evidence.

## 3. Run the first screen and analyze it

Run **Run the four-GPU screen** from `S4_EXPERIMENTS.md`: stages
`transport,builder,sql`, four explicit GPU IDs, default256, Q4/Q21/Q7, three warmups,
three pairs, seed 1823. Use a new output directory. Set the real dataset path and
scale; use the same machine configuration for both builds. The guide describes
`--sirius-config` when this machine needs an explicit configuration.

Preserve complete raw records, manifests, frozen SQL/config inputs, loader-smoke
logs, machine snapshots, and `completion.json`. Successful SQL output alone does
not prove GPU execution. Check interception/profile flags and published-byte counts.
A query with no accumulated publication has no publication-speedup observation.

Run the guide's analyzer against the completed screen and save its JSON output.
Summarize all scheduled workloads and unavailable/skipped cases. Inspect both the
baseline/candidate and baseline/baseline controls. Report logical bytes and host
elapsed times as such; a CUDA API or peer-capability matrix does not identify the
physical data route. Do not add overlapping per-device event durations together.

This screen has no selection confidence interval. End this run with a justified
next-step recommendation. Held-out confirmation, all22 regression, any conditional
topology implementation, and final pruning follow after the coordinating agent
reviews the evidence. The guide includes those later commands for reference.

Completion: a complete, analyzable screen with honest limitations, or a preserved
failed campaign and explanation. Keep unsuccessful runs; use a new directory for
any justified retry and explain the change instead of choosing the best run.

## 4. Write the result and preserve the evidence

Create project-level `S4_EXPERIMENT_RESULTS.md` with these sections:

| Section | Required content |
| --- | --- |
| Conclusion | Ready for confirmation, retain control, inconclusive, or blocked; evidence and next action. Separate observations from hypotheses. |
| Reproduction | Measured source commit, prerequisite tree, dependency revisions, exact commands, seed, environment/configuration, dataset identity/scale, artifact hashes, start/end time. |
| Machine | GPU IDs/UUIDs/models, interconnect/topology, peer access, CPU/NUMA, driver/toolkit, power/clocks, competing load, and unavailable measurements. |
| Correctness | Per-build tests and counts/skips, exit statuses, exact bitmap/SQL oracles, GPU interception, profile verification, and extension/library identity checks. |
| Stage A | Every ordered pair and concurrent/root pattern, both issuing directions, bytes, timing/throughput, roots, failures/unavailable access. Distinguish actual-pool evidence from driver capability. |
| Builder | Each workload's root, contributor/key counts, bitmap/chunk sizes, transport, input/ready bytes, scratch/HOST charges, raw timing summaries, candidate/control ratios, and baseline-control variation. |
| SQL | Per query: query timing, publication timing when published, publication count/bytes, admission/skip reason, work equivalence, parity/interception, candidate/control and baseline-control results. |
| Interpretation | What root/direction/bottleneck evidence supports, what it cannot show, and whether a bounded next experiment is justified. Do not claim a four-GPU winner from screening alone. |
| Evidence and limitations | Relative paths and checksums for all returned artifacts; missing datasets/counters, failed runs, deviations, and reproduction steps for blockers. |

Choose a unique run ID and set `S4_EVIDENCE_DIR` to its directory under
`docs/experiments/s4-results/`. Put evidence there: analyzer JSON,
test/build logs, and a compressed archive of each complete campaign directory.
The archive must include the untouched native JSONL records as well as aggregate
samples, requests, machine/config/SQL metadata, loader logs, and completion/failure
records. Include both build manifests and build commands/CMake caches. Add
`SHA256SUMS`. Preserve paths inside manifests; the report should map the originating
host paths to the returned archive locations. Exclude datasets, built binaries,
environment directories, and credentials from the commit.

Completion: another agent can audit every reported result using the committed
summary and raw evidence, without access to this host. A table of selected timings
alone is not sufficient.

## 5. Push the results back

Commit only `S4_EXPERIMENT_RESULTS.md` and the specific evidence directory. Push
that result commit to the same fork branch:

```bash
git add -- S4_EXPERIMENT_RESULTS.md "$S4_EVIDENCE_DIR"
git commit -m "Report S4 four-GPU screening results"
git push https://github.com/kevkrist/sirius.git \
  HEAD:refs/heads/experiment/s4-four-gpu-20261023
```

Use a normal fast-forward push. If the remote moved, fetch and inspect the new
commits before integrating; preserve the recorded measured-source identity.
Return the result commit SHA, branch URL, result-file URL, and a short conclusion.
Do not open a PR or push to upstream/main as part of this experiment handoff.
