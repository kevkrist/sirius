# S4 four-GPU experiment handoff

This bundle measures the existing `current_direct` publication schedule against the experimental `concurrent_ingress` schedule, with the same bounded HOST fallback in both builds. It stops at experiment evidence; it does not select or prune the final production schedule. The comparison prerequisite is tree `9f59d68e4cdc7ca47999c3579407f6f5168ea652`.

## Build the paired artifacts

Run from the repository root in the activated Pixi environment. The build helper configures the two schedules sequentially, installs and copies each build into a new directory, and invokes the manifest commands. It refuses to reuse an existing output directory. Do not edit production or fixture sources during these builds.

```bash
pixi run python scripts/build_s4_experiments.py \
  --output build/s4 --cuda-architectures native --jobs 24
```

Use an explicit CMake architecture instead of `native` when preparing binaries for a different host. The local development host uses `120a-real`; this is not an assumption about the four-GPU host. A successful build writes `build/s4/build-manifest.json` with embedded manifests for `build/s4/current_direct` and `build/s4/concurrent_ingress`.

The helper also freezes `sirius_unittest` and `sirius_cuda_fault_test` under each schedule directory. Run the focused checks sequentially on the four-GPU host before the screen:

```bash
(
  set -e
  for schedule in current_direct concurrent_ingress; do
    pixi run "build/s4/$schedule/sirius_unittest" '[s4_publication]' --order decl
    pixi run "build/s4/$schedule/sirius_cuda_fault_test"
  done
)
```

Inspect both test summaries and stop on failure. Local two-GPU results and their precise sanitizer scope are recorded in [S4_VALIDATION.md](S4_VALIDATION.md).

The native fixture links `sirius_core` statically. Its `identify` mode reports the compiled production schedule without initializing CUDA. The external `libsirius.so`, DuckDB extension and CLI are separate hashed artifacts. Manifest generation records CMake configuration, prerequisite tree, tracked source diff, untracked implementation hashes, dependency revisions, CLI version and resolved shared-library paths. The two manifests must match source and configuration identities except for the schedule flag; fixture and library hashes must differ.

The build helper invokes these supported commands; they are also available for artifacts built by hand:

```bash
pixi run python scripts/run_s4_experiments.py manifest \
  --fixture build/s4/current_direct/sirius_s4_experiment \
  --library build/s4/current_direct/install/lib/libsirius.so \
  --extension build/s4/current_direct/sirius.duckdb_extension \
  --cli build/s4/current_direct/duckdb --schedule current_direct \
  --cmake-cache build/s4/current_direct/CMakeCache.txt \
  --baseline-tree 9f59d68e4cdc7ca47999c3579407f6f5168ea652 \
  --output build/s4/current_direct/manual-manifest.json

pixi run python scripts/run_s4_experiments.py pair-manifests \
  --baseline build/s4/current_direct/manifest.json \
  --candidate build/s4/concurrent_ingress/manifest.json \
  --output build/s4/manual-build-manifest.json
```

Absolute artifact paths are recorded. After moving artifacts to another host/path, regenerate manifests there from the frozen artifacts and accompanying CMake cache; do not edit paths inside an existing manifest. Source/config provenance must describe those actual builds.

## Run the four-GPU screen

Supply the actual SF100 dataset directory or DuckDB database. The runner does not download or generate SF100. `--gpus` names CUDA-visible ordinals and preserves their order; any inherited `CUDA_VISIBLE_DEVICES` mapping is recorded. All subprocesses run sequentially, including baseline/baseline controls.

```bash
read -r -p 'Actual TPC-H SF100 Parquet directory or DuckDB database: ' DATA_PATH
pixi run python scripts/run_s4_experiments.py screen \
  --stages transport,builder,sql --gpus 0,1,2,3 \
  --build-manifest build/s4/build-manifest.json \
  --data-path "$DATA_PATH" --scale 100 \
  --profile default256 --queries 4,21,7 --warmups 3 --pairs 3 \
  --seed 1823 --output build/s4-results/screen
```

For this first handoff, analyze the screen and return the complete result directory before choosing a finalist or running confirmation:

```bash
pixi run python scripts/analyze_s4_experiments.py \
  --results build/s4-results/screen --output build/s4-results/screen-analysis.json
```

The runner writes an immutable `experiment.json` before launching measurements. Stage A covers all 12 ordered pairs, concurrent all-pairs, and inbound/outbound traffic at each root, in both issuing directions. Every transfer uses the native fixture's real owning pools. Unsupported access appears as explicit unavailable/skipped evidence. HOST fallback is measured separately with a forced-HOST builder case; unsupported direct transport is not counted as a direct measurement.

Stage B runs the builder and normal TPC-H SQL with the compiled schedule. The native fixture checks every replica against the complete CPU bitmap OR and checks membership. SQL runs a CPU oracle outside the measured GPU queries and records transparent GPU-interception evidence plus settled publication-counter deltas. The harness enables the existing `SIRIUS_ENABLE_TEST_OPTIONS=1` registration gate before database construction so its internal cap/multipartition `SET` options are available; each process request records this gate. The native fixture verifies the effective profile. This gate does not select the compiled publication schedule. A cap/policy refusal remains an end-to-end query observation; it cannot become a zero-time publication improvement. The fixture's SQL oracle currently uses exact type/null-aware result comparison; a mismatch fails the run and requires investigation.

For each build, a separate CLI process first loads the exact extension with `-bail -no-agent`, explicit configuration and loader diagnostics. A failed `LOAD` aborts the campaign. The log must show initialization of the frozen shared library; a subsequent CPU result cannot hide a failed load.

Generated configuration selects the requested GPU IDs, limits GPU capacity to 40% of each device, and limits HOST capacity to 50% of each NUMA node with two initial 128 MiB pools. Supply `--sirius-config path/to/sirius.yaml` for the intended machine configuration; it is copied and hashed. The native SQL mode checks that its configured GPU IDs match the request. Use the same machine configuration for screen, confirmation and regression. Native tables or Parquet views are pinned on the GPU by default; `--pin-tier host` is an explicit alternative, recorded in the manifest. Compression is disabled in these pin calls. The runner reuses the repository's TPC-H SQL and exact per-query pin-column lists.

## Confirm and check all queries

After examining Stage A and the screen, run the one compiled pilot on held-out builder sizes and fresh SQL repetitions. Each workload/comparison has five independent process pairs. Each process performs three warmups and six measured repetitions. The paired processes receive the same seed and workload; their launch order is randomized. Baseline/baseline controls use separate fresh process pairs with the same procedure. Workload order is randomized within each stage and cluster round.

```bash
pixi run python scripts/run_s4_experiments.py confirm \
  --stages builder,sql --gpus 0,1,2,3 \
  --build-manifest build/s4/build-manifest.json \
  --data-path "$DATA_PATH" --scale 100 --profile default256 --queries 4,21,7 \
  --warmups 3 --process-pairs 5 --pairs-per-process 6 --seed 1824 \
  --stage-a-results build/s4-results/screen --output build/s4-results/confirm

pixi run python scripts/run_s4_experiments.py regression \
  --stages sql --gpus 0,1,2,3 \
  --build-manifest build/s4/build-manifest.json \
  --data-path "$DATA_PATH" --scale 100 --profile default256 --queries all22 \
  --warmups 3 --process-pairs 5 --pairs-per-process 6 --seed 1825 \
  --stage-a-results build/s4-results/screen --output build/s4-results/regression

pixi run python scripts/analyze_s4_experiments.py \
  --results build/s4-results --output build/s4-results/analysis.json
```

Confirmation defaults to the first and last requested GPU as representative/adverse roots. Choose the two roots before running with `--roots 0,3`, using the screen evidence. This is recorded in the frozen workload list. Ten process pairs can be predeclared with `--process-pairs 10`. Alternatively, the single allowed five-cluster extension uses `--extend-from build/s4-results/confirm`, a fresh `--seed 1924`, and a new output directory with the same confirmation arguments. Its manifest references the original manifest/sample hashes and gives the new clusters distinct IDs. Analysis combines only that explicitly linked pair of campaigns, never arbitrary repeat runs or within-process repetitions as independent clusters. An extension of an extension is rejected.

The analyzer reports candidate/baseline log ratios, equal-weight process-pair mean log ratios, median/p90 ratios and raw timing/byte observations. Its two-sided 95% Student-t interval uses the independent process-pair summaries (five or ten), not the 30 within-process pairs. A three-pair screen has no selection interval. SQL publication time is the sum of preparation-through-completion finish-attempt intervals when publication occurred; it is not the older final-contribution-to-first-push counter or the whole query critical path. Missing samples, duplicate identities, changed manifests, bad oracles, non-finite/negative measurements and malformed records are rejected. Baseline-control drift makes the corresponding result inconclusive.

Publication observations require equivalent admitted work, at least 10% median reduction and an interval excluding no improvement. Query regression guards require a slowdown upper bound no greater than 3%, separately for each query. The all22 guard never averages away a regressing query. Performance assessment requires four GPUs, complete referenced Stage A evidence and the default profile. The report always remains an observation report; final topology selection, pruning and final production validation are separate work.

## Workload and dataset distinctions

Let `t=lcm(cuda_allocation_alignment,bloom_block_bytes)`, using the fixture's actual identify record. Every builder case obeys its combined per-GPU cap. `B` is bytes per key, not bytes per source stripe. Production geometry derives `C`; the runner does not substitute its own transfer algorithm.

| Phase | Keys and per-key bytes | Roots |
| --- | --- | --- |
| Default screen | One key: `1 MiB+t`, `64 MiB+t`, `256 MiB-t`; two keys: `64 MiB+t` | All roots at one-key `64 MiB+t`; final requested GPU elsewhere |
| Required HOST screen | Two keys: `64 MiB+t`, forced HOST transport | Final requested GPU |
| Held-out confirmation | One key: `8 MiB+t`, `128 MiB+t`; two keys: `8 MiB+t`, `128 MiB-t` | Predeclared representative and adverse roots |
| Raised-cap reproduction | One key: approximately 1.46 GiB, aligned to `t`; combined cap 2 GiB | Declared profile roots |

The forced-HOST correctness diagnostic is distinct from automatic-transport held-out performance cases. Coverage remains 0.9 in every profile. Raised-cap measurements use a separate result directory and do not establish default-cap S4 value. For optional SF1000 reproduction:

```bash
read -r -p 'Actual TPC-H SF1000 Parquet directory or DuckDB database: ' DATA_PATH_1000
pixi run python scripts/run_s4_experiments.py screen \
  --stages builder,sql --gpus 0,1,2,3 --profile raised2g \
  --build-manifest build/s4/build-manifest.json \
  --data-path "$DATA_PATH_1000" --scale 1000 --queries 4,21,7 \
  --seed 1826 --output build/s4-results-raised/screen
```

Run SF1000 separately with `--profile default256` to expose cap skips. An unavailable dataset is represented by unavailable records for every requested SQL repetition. Omit `--data-path` to deliberately record absence while still running transport/builder stages. Missing individual Parquet tables mark their affected queries unavailable. Unknown/tiny fixture data must use `--scale unknown/tiny`; the scale is always user-declared and never inferred from a path. Dataset identity records absolute files, sizes and modification times; add `--hash-dataset` for content hashes. Input row counts are explicitly unmeasured by Python; native SQL records result row counts. Full SF100 hashing is optional because it reads the entire dataset.

A bounded two-GPU integration smoke, for the parent or user to schedule after builds, is:

```bash
pixi run python scripts/run_s4_experiments.py screen \
  --stages sql --gpus 0,1 --queries 4 --warmups 0 --pairs 1 \
  --build-manifest build/s4/build-manifest.json \
  --data-path test/cpp/integration/data/parquet --scale unknown/tiny --hash-dataset \
  --output build/s4-results-local/sql-smoke
```

This smoke cannot satisfy a four-GPU performance gate. It still runs both compiled schedules and the baseline/baseline control, with a native CPU oracle.

## Result bundle and validation status

Each campaign contains `experiment.json`, `machine-before.json`, `machine-after.json`, `samples.jsonl`, immutable SQL/config inputs, per-process `request.json`, untouched `native.jsonl`, stdout/stderr logs, CLI loader-smoke logs, and `completion.json` with hashes. A failed campaign writes `failure.json`; analysis refuses incomplete campaigns. Existing output paths are never overwritten. Artifact hashes are checked at campaign boundaries and metadata is checked before each process; frozen SQL/config and dataset metadata are checked throughout the run. Runtime library resolution and the native compiled identity are rechecked.

`experiment.json.harness_sources` records `runner`, `analyzer`, and `build_helper`, each with its absolute `path`, `sha256`, and `size` at campaign creation. These identify the Python sources alongside the native build provenance; their paths are informational and need not match across machines.

Machine evidence includes GPU ordinal/UUID/model/PCI bus mapping, driver/toolkit information, clocks/power/concurrent load, topology and driver peer matrices, CPU/NUMA placement and process affinity. The driver peer matrix is not proof of access to real allocator pools; use Stage A's byte-checked transfers for that. Physical-route counters, actual pinned-memory placement, local CUDA stage events and allocator committed peaks remain explicitly unavailable when the native fixture or tools cannot measure them. Local event durations must not be added together as global elapsed time.

The native protocol is owned by [S4_FIXTURE_NOTES.md](S4_FIXTURE_NOTES.md). The implementation reading and ownership record is [S4_HARNESS_NOTES.md](S4_HARNESS_NOTES.md). CPU harness checks are separate from the native GPU checks:

```bash
pixi run python -m unittest discover -s scripts -p test_s4_experiments.py -v
pixi run python scripts/run_s4_experiments.py --help
pixi run python scripts/analyze_s4_experiments.py --help
```

CPU checks have passed in development. No GPU tests, experiments, or builds were run by the Python harness developer. Parent-coordinated native integration and the user's four-GPU evidence remain separately reported; CPU checks do not establish GPU readiness or a speedup.
