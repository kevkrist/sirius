# S5 four-GPU experiments

Run the supplied S5 implementation on four GPUs and return the evidence on the
same fork branch, `kevkrist/sirius:df/05-domain-evidence`. S5 stacks on S4 commit
`4d4bfdda12a88ec13b7e577b17ca40feea0316c0` (PR #2349).

The question is whether copied pin-domain evidence avoids useful amounts of
filter work, and how the explicit 0.45 profile compares with production 0.9.
The existing production default stays at 0.9. A coverage value of 2.0 disables
only this gate; disabling dynamic filtering is a separate correctness control.

Read [S5_DESIGN.md](S5_DESIGN.md) for the contract and predeclared experiment
decisions, and [S5_VALIDATION.md](S5_VALIDATION.md) for the local evidence and its
limits. Local two-GPU confirmation found a 5.15% Q21 gain at 0.45 versus 0.9;
select the four-GPU follow-up from its own fixed screen, without assuming that
result transfers. Follow the steps below on the supplied code. Preserve failures and
report code, dependency, dataset or configuration changes explicitly.

## 1. Create an isolated checkout

From an existing Sirius clone:

```bash
git fetch git@github.com:kevkrist/sirius.git df/05-domain-evidence
git worktree add --detach ../sirius-s5-four-gpu FETCH_HEAD
cd ../sirius-s5-four-gpu
git submodule update --init --recursive
git rev-parse HEAD
git status --short
nvidia-smi
```

Use a new worktree path if that path already exists. Record the fetched revision,
submodule revisions, GPU models/UUIDs, driver, CUDA toolkit and available RAM.
Check the remote checkout's `AGENTS.md` before proceeding. The benchmark runner
requires exactly four selected, idle GPUs; it preserves `CUDA_VISIBLE_DEVICES`
and rejects competing GPU processes. Reserve the GPUs for the campaign.

## 2. Build and run the focused checks

For four L40S GPUs, configure `89-real`. If the hardware differs, use its actual
CUDA architecture and record it. Run commands through this checkout's Pixi
environment, or a compatible existing Pixi manifest with the chosen path
recorded. Build the binaries on the experiment host for its actual GPUs.

```bash
pixi run cmake --preset release -DSIRIUS_DUCKDB_SOURCE_DIR="$PWD/duckdb" -DCMAKE_CUDA_ARCHITECTURES=89-real
pixi run make CMAKE_BUILD_PARALLEL_LEVEL=32
pixi run python -m unittest discover -s tools/experiments -p test_s5_compare.py -v
pixi run build/release/test/cpp/sirius_unittest '[source_policy],[build_key_domain],[key_admission],[pin_registry_epoch],[domain_evidence],[config_opt][dynamic_filter],Sirius YAML rejects invalid dynamic-filter thresholds,YAML-backed operator and compression settings are DuckDB defaults' --reporter compact --order decl --rng-seed 1824
```

Retain build and test logs. Completion means the requested cases ran and passed;
record every skip separately. The local sanitizer's extended-API diagnostics and
S4 controls are documented in `S5_VALIDATION.md`; do not classify a familiar
message as new without comparing its signature and call path.

## 3. Select the data and verify all queries

Set `S5_DATA` to the existing SF100 Parquet directory. It must contain all eight
TPC-H tables. The runner resolves the actual files, records their identities,
uses the repository's query/column lists, pins only each query's needed columns,
and declares only the six known independent TPC-H keys. Verify those declarations
against this dataset: for customer, orders, part, supplier, nation and region,
record total rows, non-null keys, distinct keys, minimum and maximum. All three
counts must agree. Orders has holes in its key range; range width is not its
domain size. No individual lineitem or partsupp column is declared unique.

```bash
export S5_DATA=/absolute/path/to/tpch_parquet_sf100
export S5_OUT="$PWD/build/s5-four-gpu-results"
mkdir -p "$S5_OUT"
s5_run=(pixi run python tools/experiments/s5_compare.py run
  --root "$PWD" --data "$S5_DATA" --gpus 4
  --config tools/experiments/s5_pinned.yaml)

"${s5_run[@]}" --queries 4 --correctness-only --out "$S5_OUT/smoke"
"${s5_run[@]}" --queries all22 --correctness-only --out "$S5_OUT/all22"
```

Every output directory must be new. The runner checks the matching CPU oracle,
GPU filtering off, and GPU coverage 0.9, 0.45 and 2.0. It verifies types, rows,
settings, GPU interception and topology; it fails on fallback, timeout,
contention or incomplete output. All 22 queries must pass before timing
conclusions. The only floating-point exception is the narrowly defined Q1
`avg_disc` check in the design and runner; all other values remain exact.

## 4. Run the fixed screen and evaluate it

Use the same idle machine, binary and configuration for every arm. The runner
sets the unchanged 256 MiB Bloom cap and disables pin compression in every arm.
The batch sizes in the supplied YAML are decimal MB. DEBUG diagnostics run in
separate processes from INFO timing.

```bash
"${s5_run[@]}" --queries 4,7,10,21 --blocks 2 --warmups 2 --reps 3 \
  --seed 18235 --out "$S5_OUT/screen"
pixi run python tools/experiments/s5_compare.py analyze --out "$S5_OUT/screen"
```

Inspect the raw diagnostics before attributing a timing change to S5. Report
pinned provenance, eligible keys and actual skips for ordinary and accumulated
publication, alongside publications and cap/resource refusals. The no-hint
diagnostic is a separate control. A gate-off cap refusal is not evidence of work
saved by S5. Keep all valid timing samples, including samples with different
optional publication outcomes.

Two screen blocks do not establish a performance confidence claim. Select at
most one evidence-active query/contrast improving by at least 5% in **both**
blocks, plus one observed no-gate control. A suspected regression can receive
the same bounded follow-up instead. If no contrast qualifies, report that
outcome and finish; do not search more queries or thresholds for a winner.

For a qualifying contrast, substitute the actual candidate/reference settings
and selected query IDs below. `--control-query` identifies the no-gate control.
This command fixes five fresh comparison pairs and five same-arm control pairs
per query, with two warmups and six measured repetitions per process:

```bash
"${s5_run[@]}" --queries 21,4 --control-query 4 --confirm 0.45:0.9 \
  --seed 18236 --out "$S5_OUT/confirmation"
pixi run python tools/experiments/s5_compare.py analyze --out "$S5_OUT/confirmation"
```

Apply the design's fixed gain, regression and drift criteria. Use fresh-process
pairs as independent observations. Inconclusive intervals remain inconclusive.
The primary comparison is among settings of the same S5 binary, so it does not
measure total S5-versus-S4 overhead.

## 5. Return the report and raw evidence

Create `S5_4GPU_RESULTS.md` at the project root. Include:

- Code/submodule and binary identities, hardware/software, exact commands,
  configuration, dataset/file manifest and the six key audits.
- Build, focused-test and all-22 correctness outcomes, with failures and skips.
- Actual gate/provenance diagnostics and publication/refusal counts by arm.
- Screen block medians and ratios; the chosen follow-up or the reason none
  qualified. Include confirmation intervals and same-arm drift if run.
- The resulting recommendation for 0.45 on this measured workload. Keep the
  production default at 0.9 and distinguish evidence from conjecture.
- Raw artifact paths and any deviations from this protocol.

Archive the complete experiment output, including manifests, copied runner,
SQL/configs, oracle data, process outputs, engine logs and records. Exclude
datasets, binaries, build products and credentials. Include the focused/build
logs and key-audit output in the results directory before archiving.

```bash
mkdir -p experiments/s5/results
tar -czf experiments/s5/results/four-gpu.tar.gz -C "$S5_OUT" .
sha256sum experiments/s5/results/four-gpu.tar.gz > experiments/s5/results/four-gpu.tar.gz.sha256
```

Before staging, obey the repository's 500 KiB added-file limit. If the full
archive is larger, move it outside the repository and package complete process directories into
separate archives and keep the campaign metadata in another archive, with an
index of original paths and checksums so the full directory can be reconstructed.
For any individual archive still too large, retain it outside Git and push a
durable download location with its checksum in the report. Keep the raw evidence
intact and check the packaged files with the repository hooks before committing.

```bash
git add S5_4GPU_RESULTS.md experiments/s5/results/
git commit -m 'test(dynamic-filter): record S5 four-GPU experiments'
git push git@github.com:kevkrist/sirius.git HEAD:df/05-domain-evidence
```

Use a normal fast-forward push. Tell the originating agent when the results are
pushed; it will fetch, audit the evidence and decide any further implementation
response. Four-GPU acceptance remains pending until that review.
