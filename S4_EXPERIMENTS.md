# S4 final revision comparison handoff

The final implementation retains the existing direct pipeline and bounded HOST fallback. The rejected concurrent-ingress pilot is removed. [S4_FINALIZATION.md](S4_FINALIZATION.md) owns the final comparison contract; [S4_EXPERIMENT_RESULTS.md](S4_EXPERIMENT_RESULTS.md) preserves the earlier screening evidence. The commands below use schema v2 and compare production revisions, not two algorithms built from one revision.

## Freeze references and install the SQL fixture overlay

The final validation snapshot uses `https://github.com/kevkrist/sirius.git`, branch `experiment/s4-final-validation-20261023`. The coordinator verifies the remote head before handing it off. Publishing this separate snapshot preserves the local review worktree's HEAD, index and unstaged cleanup.

Fetch the branch into an isolated checkout and record its clean HEAD:

```bash
S4_RESULT_BRANCH=experiment/s4-final-validation-20261023
git fetch https://github.com/kevkrist/sirius.git "refs/heads/$S4_RESULT_BRANCH" || exit 1
git worktree add --detach build/s4-final-validation FETCH_HEAD || exit 1
cd build/s4-final-validation || exit 1
test -z "$(git status --porcelain)" || exit 1
S4_MEASURED_HEAD="$(git rev-parse HEAD)"
printf '%s\n' "$S4_MEASURED_HEAD"
```

Keep that source fixed through builds and measurements and include `S4_MEASURED_HEAD` in the result report. Build fresh artifacts on the receiving host. The exact prerequisite is commit `78a3e34c66143152ff075729f4c018806c11e67d`, tree `9f59d68e4cdc7ca47999c3579407f6f5168ea652`; the separate cleanup reference is screened S4 control commit `9b2e8201787f381b62715c01405f659b925c49a7`. Production changes are intentional between roles.

These commands create isolated reference worktrees under the final checkout's persistent build directory. On a host where the coordinator already prepared references, use those existing paths instead of recreating them.

```bash
S4_ROOT="$PWD"
S4_PREREQUISITE="$S4_ROOT/build/reference-prerequisite"
S4_CONTROL="$S4_ROOT/build/reference-control"
git rev-parse HEAD
git worktree add --detach "$S4_PREREQUISITE" 78a3e34c66143152ff075729f4c018806c11e67d
git worktree add --detach "$S4_CONTROL" 9b2e8201787f381b62715c01405f659b925c49a7
for source in "$S4_ROOT" "$S4_PREREQUISITE" "$S4_CONTROL"; do
  git -C "$source" submodule update --init --recursive -- \
    duckdb cucascade substrait .claude/claude-tools \
    sirius-duckdb/vcpkg sirius-duckdb/duckdb sirius-duckdb/extension-ci-tools
done
pixi run python - "$S4_PREREQUISITE" "$S4_CONTROL" <<'PY'
from pathlib import Path
import shutil
import subprocess
import sys
sys.path.insert(0, 'scripts')
from run_s4_experiments import BASELINE_OVERLAY, baseline_cmake, source_identity
for directory, kind in zip(sys.argv[1:], ('prerequisite', 's4_control')):
    root = Path(directory)
    for name in BASELINE_OVERLAY:
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(name, target)
    original = subprocess.check_output(['git', 'show', 'HEAD:CMakeLists.txt'], cwd=root, text=True)
    (root / 'CMakeLists.txt').write_text(baseline_cmake(original))
    print(kind, source_identity(root, 'baseline', kind)['head'])
PY
```

The overlay is exactly four native files (`s4_experiment.cpp`, `s4_experiment_common.hpp`, `s4_experiment_sql.cpp`, and `s4_experiment_sql_oracle.hpp` under `test/cpp/operator/`), `cmake/sirius-s4-baseline-fixture.cmake`, and one include under the root test-build gate. The prerequisite inserts that include; frozen S4 control replaces its old fixture include. No reference `src/` file or core-target definition changes. The helper validates these constraints before building. The SQL fixture compiles against each reference's own production objects. Baseline builder and transport modes are unavailable; no baseline builder adapter or copied publication implementation is introduced.

## Build immutable revision artifacts

Build sequentially, with the same compiler, dependencies, architecture and extra CMake options. The helper reuses `build/release` separately inside each source worktree and freezes installed libraries, fixture, extension, CLI, tests, CMake cache and raw build arguments in the new output directory. It rejects existing output directories. `SIRIUS_TPCH_HAVOC=OFF` avoids that separate test target's network license fetch and does not disable these normal TPC-H SQL queries.

```bash
pixi run python scripts/build_s4_experiments.py \
  --baseline-source "$S4_PREREQUISITE" --baseline-kind prerequisite \
  --output build/s4-revisions/prerequisite --cuda-architectures native --jobs 24 \
  --cmake-option=-DSIRIUS_TPCH_HAVOC=OFF

pixi run python scripts/build_s4_experiments.py \
  --baseline-source "$S4_CONTROL" --baseline-kind s4_control --revision-role baseline \
  --output build/s4-revisions/control --cuda-architectures native --jobs 24 \
  --cmake-option=-DSIRIUS_TPCH_HAVOC=OFF

pixi run python scripts/run_s4_experiments.py pair-manifests \
  --baseline build/s4-revisions/control/baseline/manifest.json \
  --final build/s4-revisions/prerequisite/final/manifest.json \
  --output build/s4-revisions/control/build-manifest.json
```

The final artifacts are reused for the smaller control comparison. `--revision-role final` can also build only that role. `--build-dir` is relative to each source; an absolute shared build directory is rejected. `--final-source` defaults to the checkout containing the helper. Additional reusable fetch sources are accepted as repeated `--cmake-option=-DFETCHCONTENT_SOURCE_DIR_NAME=PATH`; keep the same paths/options for every arm. The parent uses this concrete local invocation after its existing builds finish:

```bash
pixi run --as-is --manifest-path /home/ubuntu/sirius/pixi.toml \
  python scripts/build_s4_experiments.py \
  --baseline-source /home/ubuntu/sirius/build/s4-prerequisite \
  --baseline-kind prerequisite --output build/s4-final-pair-oracle \
  --cuda-architectures 120a-real --jobs 24 \
  --cmake-option=-DSIRIUS_TPCH_HAVOC=OFF \
  --cmake-option=-DFETCHCONTENT_SOURCE_DIR_CUCO=/home/ubuntu/sirius/build/release/_deps/cuco-src \
  --cmake-option=-DFETCHCONTENT_SOURCE_DIR_CORROSION=/home/ubuntu/sirius/build/release/_deps/corrosion-src
```

Use the four-GPU host's architecture, not the local `120a-real`, when running remotely. Record the actual activated environment, including any `--as-is` reuse. All raw CMake values remain in the manifests. Comparison normalizes only the known source/build/install roots and omits the historical `current_direct` cache entry solely for the frozen S4-control reference. Other differences fail pairing. Source identities differ deliberately; dependency revisions, SQL fixture source hashes, DuckDB version, geometry and normalized configuration must agree.

The native fixture links `sirius_core` statically. Its `revision_role` and `identity_source=fixture_build_metadata` describe test-target metadata; they do not claim a dynamic production getter. Manifests bind the exact source revision/tree/diff, overlay hashes and executable hash. The shared library, extension and CLI are separately hashed. CLI smoke uses `-bail -no-agent`, explicit bounded configuration and loader diagnostics proving that the extension initializes its frozen shared library. A failed LOAD cannot be hidden by subsequent CPU execution.

For manually built artifacts, manifest creation accepts `--revision-role`, `--source-root`, `--build-dir`, `--baseline-kind`, `--cmake-cache`, `--fixture`, `--library`, `--extension`, `--cli`, and `--output`. Use the helper's saved invocation and actual frozen paths; do not edit paths inside an existing manifest. Artifact relocation requires fresh provenance describing those actual builds.

## Local two-GPU integration

The parent owns GPU scheduling. Run correctness checks on the final artifacts and applicable original tests on each reference before interpreting timings. Final focused checks are:

```bash
pixi run build/s4-revisions/prerequisite/final/sirius_unittest '[s4_publication]' --order decl
pixi run build/s4-revisions/prerequisite/final/sirius_cuda_fault_test
pixi run python -m unittest discover -s scripts -p test_s4_experiments.py -v
```

Use actual verified SF100 when available. The local parent verified the dataset through generator metadata and footer row counts; directory names alone do not establish scale. Supply the same bounded two-GPU configuration and pin tier to both comparisons. The latest local smoke request is one warmup and three measured repetitions per process, one independent pair, plus fresh baseline/baseline controls:

```bash
read -r -p 'Actual verified TPC-H SF100 directory or DuckDB database: ' DATA_PATH
for reference in prerequisite control; do
  pixi run python scripts/run_s4_experiments.py screen \
    --stages sql --gpus 0,1 --queries 4,7,21 --warmups 1 --pairs 3 \
    --build-manifest "build/s4-revisions/$reference/build-manifest.json" \
    --data-path "$DATA_PATH" --scale 100 --profile default256 \
    --seed 1823 --output "build/s4-local/$reference"
  pixi run python scripts/analyze_s4_experiments.py \
    --results "build/s4-local/$reference" \
    --output "build/s4-local/$reference-analysis.json"
done
```

These are operability/parity observations, not a local 3% performance claim. If the parent uses `build/s4-final-pair-oracle`, pass that directory's `build-manifest.json` instead. For fixture-only smoke, `--data-path test/cpp/integration/data/parquet --scale unknown/tiny --hash-dataset` labels the small input accurately. It does not replace SF100 evidence.

The parent completed 96 strict-oracle SF100 Q4/Q7/Q21 samples, including warmups, under `build/s4-final-pair/build-manifest.json` and `build/s4-control-artifact/build-manifest.json`. Both pairs passed manifest validation and share the same frozen final artifact. Those campaigns remain evidence for their original artifacts; their exact measured runner/analyzer/helper are preserved in `build/s4-local-validation/measured-harness/`, with hashes matching both `experiment.json` records. Refreshed oracle artifacts use new directories `build/s4-final-pair-oracle` and `build/s4-control-artifact-oracle`; Q1 must pass through all three refreshed revision fixtures before handoff resumes.

Local runs additionally pass `--sirius-config build/s4-local-validation/local-sf100.yaml`. That configuration selects GPUs 0/1, GPU fraction 0.85, HOST fraction 0.5, 256 MB scan/concat batches, 512 MB hash partition/build budgets, Bloom cap 268435456 and coverage 0.9. It is an explicit local configuration, not the runner's default or an assumption about the four-GPU host. The parent reported 94 focused cases, seven fault cases and four zero-error memchecks.

## Four-GPU final acceptance

Use four explicit CUDA-visible GPU IDs and an idle window. Keep processes sequential. Run final-only builder diagnostics on held-out 8/128 MiB sizes, one/two keys, tails and representative/adverse roots; the screen includes the separate positive forced-HOST case. The native fixture checks every replica byte against the canonical OR and checks inserted-key membership. These are final diagnostics, with no invented baseline builder timings. Stage A remains optional final-host transport evidence.

```bash
pixi run python scripts/run_s4_experiments.py screen \
  --stages builder --gpus 0,1,2,3 --warmups 3 --pairs 3 \
  --build-manifest build/s4-revisions/prerequisite/build-manifest.json \
  --profile default256 --output build/s4-final-results/builder-screen

pixi run python scripts/run_s4_experiments.py confirm \
  --stages builder --gpus 0,1,2,3 --roots 0,3 \
  --build-manifest build/s4-revisions/prerequisite/build-manifest.json \
  --profile default256 --warmups 3 --process-pairs 5 --pairs-per-process 6 \
  --seed 1824 --output build/s4-final-results/builder-heldout

read -r -p 'Actual verified TPC-H SF100 directory or DuckDB database: ' DATA_PATH
pixi run python scripts/run_s4_experiments.py regression \
  --stages sql --gpus 0,1,2,3 --queries all22 \
  --build-manifest build/s4-revisions/prerequisite/build-manifest.json \
  --data-path "$DATA_PATH" --scale 100 --profile default256 \
  --warmups 3 --process-pairs 5 --pairs-per-process 6 --seed 1825 \
  --output build/s4-final-results/prerequisite-all22

pixi run python scripts/run_s4_experiments.py confirm \
  --stages sql --gpus 0,1,2,3 --queries 4,7,21 \
  --build-manifest build/s4-revisions/control/build-manifest.json \
  --data-path "$DATA_PATH" --scale 100 --profile default256 \
  --warmups 3 --process-pairs 5 --pairs-per-process 6 --seed 1824 \
  --output build/s4-final-results/control-sql

pixi run python scripts/analyze_s4_experiments.py \
  --results build/s4-final-results --output build/s4-final-results/analysis.json
```

All22 includes Q4/Q7/Q21; do not duplicate that prerequisite comparison. The control comparison checks pruning and cannot replace prerequisite-versus-final acceptance. Optional Stage A uses `screen --stages transport` with the same build manifest and GPU mapping; it covers ordered pairs, all-pairs and each root in both issuing directions. Unsupported direct access is explicit unavailable evidence. Physical routes and local CUDA stage timings remain unmeasured unless separately collected.

For every SQL workload, the runner randomizes matched baseline/final process order and uses separate fresh baseline/baseline controls. Five independent process pairs each supply six measured repetitions after three warmups. The analyzer computes paired log ratios and a two-sided 95% Student-t interval across process-pair means: five clusters, not thirty IID observations. A one-cluster screen has no selection interval. Baseline-control drift makes a result inconclusive. The per-query slowdown upper bound must be at most 3%; the all22 result cannot average away a regressing query and only the prerequisite comparison can satisfy its guard. A 10% speedup is not required to retain the direct implementation.

The single permitted extension adds five fresh clusters with `--extend-from` pointing to the original completed confirm/regression directory, a fresh `--seed`, unchanged inputs and a new output directory. Alternatively predeclare ten clusters with `--process-pairs 10`. The runner hashes the original evidence and assigns disjoint cluster IDs; analysis combines only that explicit extension. Do not pool arbitrary reruns or select favorable repetitions.

## Measurement and evidence rules

Normal SQL uses the native CPU oracle outside the timed GPU query. The fixture verifies full typed/null-aware results, GPU interception and the effective cap/coverage/enable settings. Existing `SIRIUS_ENABLE_TEST_OPTIONS=1` exposes the internal test options; it does not select engine behavior. Default256 means 256 MiB combined Bloom cap per GPU and coverage 0.9. Generated YAML selects the requested devices, 40% GPU capacity and 50% HOST capacity with two initial 128 MiB pools. `--sirius-config` copies and hashes an explicit machine configuration. GPU pinning is default, `--pin-tier host` is explicit, and compression is disabled. Repository TPC-H SQL and per-query pin-column resources are reused.

The fixed oracle policy is `exact_except_q1_avg_disc_v1`: only Q1's `avg_disc` DOUBLE column (zero-based column 8) permits finite relative or absolute differences up to `1e-12`, after native schema/type/shape and unique exact key checks. Every other cell remains exact; malformed Q1 schemas fail. Identify and SQL records carry `oracle_policy`, `oracle_rel_tol=1e-12`, and `oracle_abs_tol=1e-12`; the runner checks the constants and their manifest binding. Raw digests remain unchanged and accepted Q1 digests may differ. The shared oracle header is hashed across all three revision fixtures. Older strict schema-v2 campaigns remain analyzable under their original metadata and preserved measured harness, without relabeling them as tolerance-policy results.

The prerequisite lacks S4 publication preparation timing and byte/transport telemetry. Those fields remain null with `publication_metrics_supported=false`, even when its real common publication count is positive. Its older final-contribution-to-first-push latency is never compared to S4 preparation timing. End-to-end comparisons require equal common publication/contribution/admission/filter outcomes; changed work is reported with the differing counters and excluded from overhead ratios. Missing publication telemetry alone does not invalidate equal-work query timing. Q4 may have no accumulated publication. Final Q7/Q21 positive publication must be checked on the acceptance host.

Builder sizes obey the combined per-GPU cap, with alignment tail `t=lcm(cuda_allocation_alignment,bloom_block_bytes)`. Screen sizes are one-key 1 MiB+t, 64 MiB+t, 256 MiB-t and two-key 64 MiB+t; confirmation uses one-key 8 MiB+t, 128 MiB+t and two-key 8 MiB+t, 128 MiB-t. The forced-HOST screen uses two keys at 64 MiB+t. Existing raised2g diagnostics remain separately labeled and cannot establish default-profile acceptance.

No dataset is downloaded or generated. Missing datasets/tables produce explicit unavailable SQL records. Scale is user-declared. Dataset identity includes file paths, sizes and mtimes; `--hash-dataset` adds content hashes. Record generator metadata/footer row counts separately when establishing SF100. Python does not infer input row counts; native result row counts are different evidence.

Each campaign preserves immutable `experiment.json`, complete native JSONL, aggregate samples, requests, SQL/config inputs, loader logs, machine snapshots and completion/failure records. `experiment.json.harness_sources` contains path/SHA-256/size for runner, analyzer and build helper. Host evidence includes GPU mapping/UUIDs, topology, driver/toolkit, clocks/power/load and CPU/NUMA affinity; unavailable observations remain explicit. Physical-route, pinned-placement and committed-allocator-peak claims require separate measurements. Keep failed runs and use a new output path for a justified retry.

## Reanalyze the archived schema-v1 screen

Schema v2 intentionally rejects v1 manifests rather than silently relabeling algorithms as revisions. Keep the archived records unchanged. Recover the original analyzer and its runner dependency from the measured source commit:

```bash
mkdir -p build/s4-legacy-tools build/s4-legacy-results
for tool in run analyze; do
  git show "9b2e8201787f381b62715c01405f659b925c49a7:scripts/${tool}_s4_experiments.py" \
    > "build/s4-legacy-tools/${tool}_s4_experiments.py"
done
tar -xzf docs/experiments/s4-results/20261023T150247Z-l40s-screen/screen-campaign.tar.gz \
  -C build/s4-legacy-results
pixi run python build/s4-legacy-tools/analyze_s4_experiments.py \
  --results build/s4-legacy-results/build/s4-results/20261023T150247Z-l40s-screen \
  --output build/s4-legacy-results/reanalysis.json
```

Historical results remain attributed to their original source and hardware. CPU harness checks do not establish GPU correctness or four-GPU acceptance. Final local native/GPU outcomes belong to the parent validation record; remote acceptance remains pending the user's run.
