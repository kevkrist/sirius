# S5 four-GPU results

Four-GPU validation and fixed-screen evaluation for `kevkrist/sirius:df/05-domain-evidence`
(S5: source-bound domain evidence for the dynamic-filter usefulness gate), stacked on
published S4 commit `4d4bfdda12a88ec13b7e577b17ca40feea0316c0` (PR #2349).

## 1. Code, submodule, binary, hardware and software identities

### Source and submodules

- **Worktree**: `/home/ubuntu/sirius-s5-four-gpu`
- **Fetched branch**: `kevkrist/sirius:df/05-domain-evidence`
- **HEAD commit**: `da7b8ab777dda089ae9087ad35cf5d7263180aea`
- **Tree hash (`HEAD^{tree}`)**: `a14106e78858717667e9762454b1437477da14bc` (recorded in `source-tree.txt`)
- **Tracked source diff against `HEAD`**: empty (`0` bytes in `source-diff.patch`)
- **Submodule revisions**:
  - `cucascade`: `1a9281093a0fe49634594d541d7ce327129f53d6`
  - `duckdb`: `88ecd0a49d7d3d838e002b2fbc0ce91ccf74110e`
  - `sirius-duckdb/duckdb`: `88ecd0a49d7d3d838e002b2fbc0ce91ccf74110e`
  - `sirius-duckdb/extension-ci-tools`: `55e9f697b4fb70f357cc1bd4d1893cbf217ee359`
  - `sirius-duckdb/vcpkg`: `cd61e1e26a038e82d6550a3ebbe0fbbfe7da78e3`
  - `substrait`: `922489d3c8c77140b189ad3847554de141378b04` (`substrait/duckdb`: `d8cdaa33fda8df955cc76ef58a280f68f4cd43fa`, `substrait/extension-ci-tools`: `72e76e99cd7fee45a99739cd118ec2db64e034ec`, `substrait/substrait`: `29844f1b58a666f36493c27def553854e81328a7`)
  - `.claude/claude-tools`: `bcfccda6e35b50f356df99c2baae16204901ede3`
  - `experimental/doris/doris`: `ad35a140c7fd0b842f18c23300bac581f7d04326`
  - `experimental/starrocks/brpc`: `7daed0be6bf1f4d64b25554c72de7d9319a35dd7`
  - `experimental/starrocks/starrocks`: `8a8e18642450fba09149c8a3f149abefa043c2b7`

### Built binaries and frozen runner artifacts

| Artifact | Relative path | Size (bytes) | SHA-256 |
|---|---|---:|---|
| DuckDB CLI | `sirius-duckdb/build/release/duckdb` | 82,772,112 | `2fbac6964977d9831e2a34caae399362ce8e175cd40ba383d37bc3451cf7fd9a` |
| Sirius extension | `sirius-duckdb/build/release/extension/sirius/sirius.duckdb_extension` | 1,047,166 | `ab4183bc04ec60f768a75766619b0f0e226d0dadd3e891d54e0cdba7dfaff7b8` |
| Shared library | `build/release/install/lib/libsirius.so.0.0.0` | 129,505,544 | `43426c519c61b71f444756f90cc3c28b0357f7387b2e07e9515ceeffce4b5e10` |
| Experiment runner | `tools/experiments/s5_compare.py` | 43,507 | `754ccbb2adf176c492d7277200d53e536e5ae786af5961f4cafbcf543ebf2ceb` |
| Pin column helper | `test/tpch_performance/tpch_pin_columns.py` | 16,398 | `ce76484cc2e475ed9e60adf9cd60b57dc1851f75dc556a0be75e6dc80c0e910a` |
| Effective YAML config | `tools/experiments/s5_pinned.yaml` (`sirius.yaml`) | 239 | `8c889f183a896fd9c857a8d8f32467b9ba9961d6b0a06f7ada14c7a46be35166` |

`ldd` verification confirmed that `sirius.duckdb_extension` resolves uniquely to this worktree's
`build/release/install/lib/libsirius.so.0.0.0`.

### Hardware and software environment

- **Host OS**: Linux `6.8.0-1066-aws` (`x86_64`)
- **CPU**: Intel(R) Xeon(R) 6975P-C (96 logical CPUs, NUMA node 0: CPUs `0-95`)
- **System RAM**: 362 GiB total (~351 GiB available at campaign start; 0 swap)
- **GPUs (4× idle NVIDIA RTX PRO 4500 Blackwell Server Edition, compute capability `12.0`, 32,623 MiB / 33,689,829,376 bytes each)**:
  - GPU 0: `GPU-194e6349-f1f3-0f73-5a25-2ee6b5c423db` (`0000:63:00.0`)
  - GPU 1: `GPU-fe3ed4a5-7470-2e07-57d7-f52515670cb2` (`0000:65:00.0`)
  - GPU 2: `GPU-17e8986d-1f2b-9dc5-b0da-8496941cfe9a` (`0000:67:00.0`)
  - GPU 3: `GPU-ef7856a1-cd75-205a-92b6-1fa719eef0e3` (`0000:69:00.0`)
- **Interconnect topology**: all 4 GPUs connected via `NODE` (PCIe + host bridge interconnect within NUMA node 0; CPU affinity `0-95`)
- **NVIDIA driver**: `595.91.07`
- **CUDA toolkit**: system `nvcc` `13.2.51`; Pixi environment `nvcc` `13.4.92` with GNU `15.3.0`, `cudf`/`rmm`/`raft`/`cuvs`/`kvikio` `26.08`, `CCCL` `3.4.3.0`, `ucxx` `0.51.01`, `roaring` `4.4.2`, Python `3.14.8`
- **Pixi manifest**: `/home/ubuntu/sirius/pixi.toml` (identical to `/home/ubuntu/sirius-s5-four-gpu/pixi.toml` and `pixi.lock`, invoked via `/home/ubuntu/.pixi/bin/pixi run --manifest-path /home/ubuntu/sirius/pixi.toml`)
- **CUDA architecture configured**: `-DCMAKE_CUDA_ARCHITECTURES=120-real` (matching the actual Blackwell `sm_120` hardware on this host instead of the L40S `89-real` example)

### Effective configuration

`tools/experiments/s5_pinned.yaml` (rendered into `sirius.yaml`):

```yaml
sirius:
  topology:
    num_gpus: 4
  memory:
    gpu:
      usage_limit_fraction: 0.85
  operator_params:
    scan_task_batch_size: 256MB
    concat_batch_bytes: 256MB
    hash_partition_bytes: 512MB
    max_build_hash_table_bytes: 512MB
```

Session settings verified on every GPU process via `settings.json`:
- `enable_duckdb_fallback = false`
- `enable_dynamic_filter = true` (`false` in `df_off`)
- `enable_dynamic_filter_multi_partition = true`
- `max_dynamic_filter_bloom_bytes_per_gpu = 268435456` (256 MiB)
- `dynamic_filter_domain_coverage_threshold` in `{0.9, 0.45, 2.0}`
- `compression = false` on all `CALL pin_table(...)` statements

## 2. Dataset manifest and six TPC-H key audits

### Dataset manifest

The TPC-H SF100 Parquet dataset was generated into `/home/ubuntu/sirius-s5-four-gpu/test_datasets/tpch_parquet_sf100` using `test/tpch_performance/generate_tpch_data.sh 100 --format parquet` (`tpchgen-rs` `v2.0.2`). It contains all eight tables across 14 Parquet files (27,679,630,327 bytes total):

| Table | File path (relative to `S5_DATA`) | Size (bytes) | `mtime_ns` |
|---|---|---:|---:|
| `customer` | `customer/part.0.parquet` | 1,076,302,309 | `1793128476989307364` |
| `lineitem` | `lineitem/part.0.parquet` | 2,864,588,086 | `1793128478738315501` |
| `lineitem` | `lineitem/part.1.parquet` | 2,864,161,741 | `1793128478111312584` |
| `lineitem` | `lineitem/part.2.parquet` | 2,865,268,729 | `1793128478232313147` |
| `lineitem` | `lineitem/part.3.parquet` | 2,864,315,036 | `1793128478221313096` |
| `lineitem` | `lineitem/part.4.parquet` | 2,865,084,199 | `1793128478552314636` |
| `lineitem` | `lineitem/part.5.parquet` | 2,864,185,047 | `1793128478008312104` |
| `nation` | `nation/part.0.parquet` | 2,250 | `1793128471199280358` |
| `orders` | `orders/part.0.parquet` | 2,525,756,275 | `1793128478536314561` |
| `orders` | `orders/part.1.parquet` | 2,525,626,871 | `1793128478228313128` |
| `part` | `part/part.0.parquet` | 508,470,833 | `1793128477610310253` |
| `partsupp` | `partsupp/part.0.parquet` | 3,787,197,945 | `1793128479373318455` |
| `region` | `region/part.0.parquet` | 1,103 | `1793128471203280377` |
| `supplier` | `supplier/part.0.parquet` | 68,669,903 | `1793128471963283934` |

### Six key audits

Audited directly against the SF100 Parquet files with CPU DuckDB (`SIRIUS_DISABLE=1`, recorded in `build-and-tests/tpch-key-audit.json`). No `lineitem` or `partsupp` column is declared unique.

| Table | Key column | Total rows | Non-null keys | Distinct keys | Min key | Max key |
|---|---|---:|---:|---:|---:|---:|
| `customer` | `c_custkey` | 15,000,000 | 15,000,000 | 15,000,000 | 1 | 15,000,000 |
| `nation` | `n_nationkey` | 25 | 25 | 25 | 0 | 24 |
| `orders` | `o_orderkey` | 150,000,000 | 150,000,000 | 150,000,000 | 1 | 600,000,000 |
| `part` | `p_partkey` | 20,000,000 | 20,000,000 | 20,000,000 | 1 | 20,000,000 |
| `region` | `r_regionkey` | 5 | 5 | 5 | 0 | 4 |
| `supplier` | `s_suppkey` | 1,000,000 | 1,000,000 | 1,000,000 | 1 | 1,000,000 |

All six tables satisfy `total_rows == non_null_keys == distinct_keys`. `orders` has sparse keys spanning `1..600,000,000` with an exact domain cardinality of `150,000,000`.

## 3. Commands, build, focused tests and all-22 correctness outcomes

### Exact commands run

```bash
export PATH="/home/ubuntu/.pixi/bin:$PATH"
git fetch git@github.com:kevkrist/sirius.git df/05-domain-evidence
git worktree add --detach /home/ubuntu/sirius-s5-four-gpu FETCH_HEAD
cd /home/ubuntu/sirius-s5-four-gpu
git submodule update --init --recursive

pixi run --manifest-path /home/ubuntu/sirius/pixi.toml cmake --preset release \
  -DSIRIUS_DUCKDB_SOURCE_DIR="$PWD/duckdb" -DCMAKE_CUDA_ARCHITECTURES=120-real
pixi run --manifest-path /home/ubuntu/sirius/pixi.toml make CMAKE_BUILD_PARALLEL_LEVEL=32
pixi run --manifest-path /home/ubuntu/sirius/pixi.toml python -m unittest discover \
  -s tools/experiments -p test_s5_compare.py -v
pixi run --manifest-path /home/ubuntu/sirius/pixi.toml build/release/test/cpp/sirius_unittest \
  '[source_policy],[build_key_domain],[key_admission],[pin_registry_epoch],[domain_evidence],[config_opt][dynamic_filter],Sirius YAML rejects invalid dynamic-filter thresholds,YAML-backed operator and compression settings are DuckDB defaults' \
  --reporter compact --order decl --rng-seed 1824

export S5_DATA=/home/ubuntu/sirius-s5-four-gpu/test_datasets/tpch_parquet_sf100
export S5_OUT="$PWD/build/s5-four-gpu-results"
s5_run=(pixi run --manifest-path /home/ubuntu/sirius/pixi.toml python tools/experiments/s5_compare.py run
  --root "$PWD" --data "$S5_DATA" --gpus 4
  --config tools/experiments/s5_pinned.yaml)

"${s5_run[@]}" --queries 4 --correctness-only --out "$S5_OUT/smoke"
pixi run --manifest-path /home/ubuntu/sirius/pixi.toml python tools/experiments/s5_compare.py analyze --out "$S5_OUT/smoke"

"${s5_run[@]}" --queries all22 --correctness-only --out "$S5_OUT/all22"
pixi run --manifest-path /home/ubuntu/sirius/pixi.toml python tools/experiments/s5_compare.py analyze --out "$S5_OUT/all22"

"${s5_run[@]}" --queries 4,7,10,21 --blocks 2 --warmups 2 --reps 3 \
  --seed 18235 --out "$S5_OUT/screen"
pixi run --manifest-path /home/ubuntu/sirius/pixi.toml python tools/experiments/s5_compare.py analyze --out "$S5_OUT/screen"
```

### Build, focused tests and correctness summary

| Check | Outcome | Failures | Skips | Evidence in archive |
|---|---|---:|---:|---|
| Release build (`sirius_library`, `sirius_unittest`, `sirius-duckdb`) | Passed | 0 | 0 | `build-and-tests/{cmake-configure,make-build}.log` |
| Runner unit tests (`test_s5_compare.py`) | 18 ran, 18 passed (1.015 s) | 0 | 0 | `build-and-tests/test_s5_compare.log` |
| Focused C++ suite (`--rng-seed 1824`) | 71 test cases, 8,088 assertions passed | 0 | 0 | `build-and-tests/focused-tests.log` |
| Q4 4-GPU smoke (`smoke`) | 4 GPU processes + 1 CPU oracle passed (104.20 s); `analyze` passed | 0 | 0 | `smoke/`, `build-and-tests/smoke-audit.json` |
| All-22 4-GPU correctness (`all22`) | 88 GPU processes (`df_off`, `0.9`, `0.45`, `2.0`) matched 22 CPU oracles (2,711.57 s); `analyze` passed | 0 | 0 | `all22/`, `build-and-tests/all22-audit.json` |

Every GPU execution in `smoke`, `all22` and `screen` verified:
- Schema and result parity against the CPU DuckDB oracle (exact match on all queries; Q1 uses the narrow `avg_disc` `1e-12` rule).
- Effective settings (`fallback=false`, expected `enabled`, `multipartition=true`, `cap=268435456`, expected `coverage`).
- `Transparent execution: physical plan replaced with GPU operator` count equal to the number of executions, `topology summary: 4 GPU(s)`, zero fallbacks, zero engine errors, and zero competing GPU processes.

## 4. Gate/provenance diagnostics and publication/refusal counts

### DEBUG gate and provenance records (`screen` diagnostic phase)

Each query ran four DEBUG diagnostic processes (`0.9` with hints, `0.45` with hints, `2.0` with hints, and `0.9` `NOHINT` without hints), plus `df_off`.

| Query | Path & condition | Build rows | Domain | Provenance | Eligible (`hints=True` / `NOHINT`) | Decision at `0.9` / `0.45` / `2.0` / `NOHINT` |
|---|---|---:|---:|---|---|---|
| Q4 | `ordinary` cond `0` | 5,738,701 | 0 | `unknown` | `0` / `0` | `keep` / `keep` / `keep` / `keep` |
| Q7 | `ordinary` cond `0` | 2 | 25 | `pinned_parquet_exact` | `1` / `0` | `keep` / `keep` / `keep` / `keep` |
| Q7 | `ordinary` cond `0` | 1,200,734 | 0 | `unknown` | `0` / `0` | `keep` / `keep` / `keep` / `keep` |
| Q7 | `accumulated` cond `0` | 11,991,354 | 0 | `unknown` | `0` / `0` | `keep` / `keep` / `keep` / `keep` |
| Q7 | `ordinary` cond `0` | 2 | 25 | `pinned_parquet_exact` | `1` / `0` | `keep` / `keep` / `keep` / `keep` |
| Q7 | `ordinary` cond `0` | 80,079 | 0 | `unknown` | `0` / `0` | `keep` / `keep` / `keep` / `keep` |
| Q10 | `ordinary` cond `0` | 5,737,423 | 150,000,000 | `pinned_parquet_exact` | `0` / `0` | `keep` / `keep` / `keep` / `keep` |
| Q10 | `accumulated` cond `0` | 5,737,423 | 0 | `unknown` | `0` / `0` | `keep` / `keep` / `keep` / `keep` |
| Q21 | `accumulated` cond `0` | 73,072,502 | 150,000,000 | `pinned_parquet_exact` | `1` / `0` | `keep` / **`skip`** / `keep` / `keep` |
| Q21 | `ordinary` cond `0` | 1 | 25 | `pinned_parquet_exact` | `1` / `0` | `keep` / `keep` / `keep` / `keep` |
| Q21 | `ordinary` cond `0` | 40,078 | 0 | `unknown` | `0` / `0` | `keep` / `keep` / `keep` / `keep` |
| Q21 | `accumulated` cond `0` | 7,340,729 | 0 | `unknown` | `0` / `0` | `keep` / `keep` / `keep` / `keep` |
| Q21 | `accumulated` cond `0` | 7,074,061 | 0 | `unknown` | `0` / `0` | `keep` / `keep` / `keep` / `keep` |

Key observations from DEBUG diagnostics:
- **Q21** is the only query where the coverage gate fires: its `orders` build has `73,072,502` rows against the copied `150,000,000`-row `pinned_parquet_exact` domain (`48.715%` coverage). With `unique_cols => ['o_orderkey']` (`eligible=1`), `0.45` skips this accumulated Bloom filter (`decision=skip`), avoiding the `146,145,024`-byte-per-GPU allocation across all 4 GPUs. At `0.9` and `2.0`, `decision=keep` and the filter is built and published. In the `NOHINT` control (`hints=False` at `0.9`), `domain=150000000` and `provenance=pinned_parquet_exact` are still observed, but `eligible=0` because no uniqueness hint was declared.
- **Q10** copies `domain=150000000` (`pinned_parquet_exact`) on `orders`, but `eligible=0` because the join key is `o_custkey` (not declared unique).
- **Q7** copies `domain=25` (`pinned_parquet_exact`, `eligible=1` on `n_nationkey` with `2` build rows, `8%` coverage), which stays below both `0.45` and `0.9`.
- **Q4** has `domain=0, provenance=unknown, eligible=0` on its grouped/filtered build key, with zero accumulated filters and identical gate decisions across all arms.

### Publication and refusal counts by arm (`screen`)

Across all 20 preflight/diagnostic processes and all 24 timing processes (120 warmup+measured timing executions total; 5 executions per timing process = 2 warmups + 3 measured reps):

| Query | Arm | Diagnostic accum started / published / refused | Timing accum started / published / refused (per 5-exec process, block 0 / block 1) | Ordinary publications (per exec: pushed>0 / total) | Observed accumulated array bytes per GPU |
|---|---|---|---|---|---|
| Q4 | `0.9`, `0.45`, `2.0` | `0 / 0 / 0` | `0 / 0 / 0` in both blocks | `1 / 1` (5,738,701 rows) | None |
| Q7 | `0.9`, `0.45`, `2.0` | `1 / 1 / 0` | `5 / 5 / 0` in both blocks | `2 / 4` (2 and 80,079 rows push; two drained targets push 0) | `23,982,848` B on 4 GPUs |
| Q10 | `0.9`, `0.45`, `2.0` | `1 / 1 / 0` | `5 / 5 / 0` in both blocks | `1 / 1` (5,737,423 rows) | `11,474,944` B on 4 GPUs |
| Q21 | `0.45` | `2 / 2 / 0` | `10 / 10 / 0` in both blocks | `1-2 / 2` (40,078 rows; 1-row nation target sometimes already drained) | `14,681,600` B + `14,148,352` B (`146,145,024` B skipped) |
| Q21 | `0.9` | `3 / 3 / 0` | `15 / 15 / 0` in both blocks | `1-2 / 2` | `146,145,024` B + `14,681,600` B + `14,148,352` B |
| Q21 | `2.0` | `3 / 3 / 0` | `15 / 15 / 0` in both blocks | `1-2 / 2` | `146,145,024` B + `14,681,600` B + `14,148,352` B |

- **Cap / resource refusals**: `0` across every diagnostic and timing execution in every arm (`accum_refused = 0`, `ordinary_oom = 0`).
- **Transport**: all accumulated Bloom publications on 4 GPUs completed via `peer copies` (`0` host-staged fallbacks).

## 5. Fixed four-GPU screen results and continuation evaluation

### Process medians by block and arm

Each process ran 2 warmups followed by 3 measured repetitions (`--blocks 2 --warmups 2 --reps 3 --seed 18235`). Total screen wall time: `1,417.57` seconds.

| Query | Block | `0.45` median (s) | `0.9` median (s) | `2.0` median (s) | `0.45` measured reps (s) | `0.9` measured reps (s) | `2.0` measured reps (s) |
|---|---:|---:|---:|---:|---|---|---|
| Q4 | 0 | 0.059292 | 0.066766 | 0.063721 | `[0.062492, 0.055407, 0.059292]` | `[0.071367, 0.066766, 0.064937]` | `[0.060794, 0.063721, 0.067422]` |
| Q4 | 1 | 0.060362 | 0.056647 | 0.066550 | `[0.063865, 0.058307, 0.060362]` | `[0.056647, 0.063832, 0.052633]` | `[0.071598, 0.055508, 0.066550]` |
| Q7 | 0 | 0.073642 | 0.074562 | 0.075466 | `[0.078983, 0.073099, 0.073642]` | `[0.074562, 0.080495, 0.071263]` | `[0.072921, 0.079550, 0.075466]` |
| Q7 | 1 | 0.069953 | 0.076880 | 0.073004 | `[0.071239, 0.069953, 0.068992]` | `[0.084265, 0.069814, 0.076880]` | `[0.068885, 0.073004, 0.089100]` |
| Q10 | 0 | 0.156588 | 0.152265 | 0.152390 | `[0.156588, 0.152864, 0.156625]` | `[0.152265, 0.152263, 0.156072]` | `[0.152390, 0.152191, 0.156525]` |
| Q10 | 1 | 0.157502 | 0.154572 | 0.152899 | `[0.156744, 0.158323, 0.157502]` | `[0.155831, 0.153443, 0.154572]` | `[0.152802, 0.153288, 0.152899]` |
| Q21 | 0 | 0.208247 | 0.216035 | 0.230538 | `[0.210415, 0.207416, 0.208247]` | `[0.217540, 0.214756, 0.216035]` | `[0.226704, 0.231470, 0.230538]` |
| Q21 | 1 | 0.224090 | 0.226392 | 0.224471 | `[0.224090, 0.217949, 0.227646]` | `[0.226392, 0.223358, 0.230708]` | `[0.219500, 0.227588, 0.224471]` |

### Screen block median ratios and qualification check

Ratios below `1.0` favor the candidate (`candidate / reference`). By predeclared rule (the [fixed protocol](README.md#fixed-experiment-protocol)), a contrast qualifies for the 5-block confirmation only if it is **evidence-active** (shows an actual coverage-gate difference between the compared arms) and improves by **at least 5% (`ratio <= 0.95`) in both blocks**, or exhibits a suspected regression.

| Query | Contrast (`candidate : reference`) | Evidence-active? | Block 0 ratio | Block 1 ratio | Geometric mean ratio | Meets $\ge 5\%$ in both blocks? |
|---|---|---|---:|---:|---:|---|
| Q4 | `0.9 : 2.0` | No (`0` skips in all arms) | 1.047780 | 0.851186 | 0.944381 | No (and not evidence-active) |
| Q4 | `0.45 : 2.0` | No (`0` skips in all arms) | 0.930488 | 0.907013 | 0.918676 | Not evidence-active (no-gate control; sub-67 ms jitter) |
| Q4 | `0.45 : 0.9` | No (`0` skips in all arms) | 0.888057 | 1.065587 | 0.972780 | No (and not evidence-active) |
| Q7 | `0.9 : 2.0` | No (`0` skips in all arms) | 0.988031 | 1.053090 | 1.020042 | No (and not evidence-active) |
| Q7 | `0.45 : 2.0` | No (`0` skips in all arms) | 0.975838 | 0.958210 | 0.966984 | No (and not evidence-active) |
| Q7 | `0.45 : 0.9` | No (`0` skips in all arms) | 0.987659 | 0.909904 | 0.947985 | No (block 0 is `0.9877`, not evidence-active) |
| Q10 | `0.9 : 2.0` | No (`0` skips in all arms) | 0.999181 | 1.010943 | 1.005045 | No (and not evidence-active) |
| Q10 | `0.45 : 2.0` | No (`0` skips in all arms) | 1.027550 | 1.030108 | 1.028828 | No (and not evidence-active) |
| Q10 | `0.45 : 0.9` | No (`0` skips in all arms) | 1.028392 | 1.018957 | 1.023663 | No (and not evidence-active) |
| Q21 | `0.9 : 2.0` | No (`0` skips at `0.9` and `2.0`) | 0.937091 | 1.008557 | 0.972167 | No (and not evidence-active) |
| **Q21** | **`0.45 : 2.0`** | **Yes (`1` skip at `0.45` vs `0` at `2.0`)** | **0.903310** | **0.998304** | **0.949620** | **No** (block 0 is `-9.67%`, but block 1 is `-0.17%`) |
| **Q21** | **`0.45 : 0.9`** | **Yes (`1` skip at `0.45` vs `0` at `0.9`)** | **0.963951** | **0.989834** | **0.976807** | **No** (block 0 is `-3.60%`, block 1 is `-1.02%`) |

### Continuation decision

**No contrast qualified for the five-block confirmation campaign.**
- Only **Q21** (`0.45 : 0.9` and `0.45 : 2.0`) is evidence-active.
- For `Q21 0.45 : 0.9`, both blocks favored `0.45` (`0.963951` and `0.989834`, geometric mean `0.976807` or `2.32%` faster), but neither block reached the predeclared `5%` (`<= 0.95`) continuation threshold.
- For `Q21 0.45 : 2.0`, block 0 improved by `9.67%` (`0.903310`), but block 1 improved by only `0.17%` (`0.998304`), failing the **both-blocks** requirement.
- No evidence-active contrast showed a suspected regression (`Q21` ratios were `< 1.0` in every block for both `0.45 : 0.9` and `0.45 : 2.0`).
- The [fixed protocol](README.md#fixed-experiment-protocol) required stopping when no contrast qualified. No confirmation run was executed.

## 6. Recommendation for `0.45` vs `0.9` on this four-GPU workload

Keep the production default at **0.9** and retain 0.45 as an experimental profile.
The four-GPU screen establishes the intended work avoidance and correct results,
but does not confirm a latency gain or a 3% nonregression guarantee.

Q21 at 0.45 avoids a 146,145,024-byte Bloom array per GPU (**139.4 MiB per GPU**,
584,580,096 bytes across four GPUs), together with its construction and publication.
This is avoided array allocation, not a measurement of total peak memory or transfer
bytes. Actual INFO timing logs show zero large-array starts at 0.45 versus ten at
0.9 and ten at 2.0, across ten executions per arm including warmups. Both smaller
filters remain, and no cap/resource refusals were observed.

The Q21 0.45/0.9 screen ratios were 0.963951 and 0.989834: a descriptive geometric
reduction of 2.32%, without a confirmation interval. The earlier confirmed 5.15%
two-GPU improvement applies to its own measured environment. These experiments do
not establish total S5-versus-S4 overhead or explain the difference between hosts;
no cross-host scaling or pipeline-overlap conclusion is claimed.

## 7. Raw artifacts and protocol notes

### Protocol deviations and host adaptations

1. **CUDA architecture (`120-real` vs `89-real`)**: The host has four NVIDIA RTX PRO 4500 Blackwell Server Edition GPUs (`sm_120`), not L40S (`sm_89`). As instructed ("If the hardware differs, use its actual CUDA architecture and record it"), CMake was configured with `-DCMAKE_CUDA_ARCHITECTURES=120-real`.
2. **Pixi invocation path**: The worktree uses the repository's identical Pixi manifest via `/home/ubuntu/.pixi/bin/pixi run --manifest-path /home/ubuntu/sirius/pixi.toml` ("or a compatible existing Pixi manifest with the chosen path recorded").
3. **Dataset generation**: Because `/home/ubuntu` on this fresh 4-GPU instance did not have a pre-existing `tpch_parquet_sf100` directory, the reference SF100 Parquet dataset was generated into `/home/ubuntu/sirius-s5-four-gpu/test_datasets/tpch_parquet_sf100` using `test/tpch_performance/generate_tpch_data.sh 100 --format parquet` (`tpchgen-rs` `v2.0.2`) before running the key audit and campaigns.
4. **Archive splitting for the 500 KiB repository hook**: The complete output tree
   (3,915 files) is packaged in [results/](results/) as 162 `.tar.xz` archives,
   each at most 446,400 bytes. [index.json](results/index.json) records the archive
   paths, sizes and hashes; [sha256sums.txt](results/sha256sums.txt) verifies the
   archives. Per-file manifests record hash, byte count, containing archive and
   original extracted path. No separate host-local archive is needed.

### Reconstructing and auditing the full directory from Git

Use the [replay instructions](README.md#replay-the-returned-evidence) to verify
archive checksums, reconstruct the output and rerun the saved-result analyzer.
The archive filenames use `processes/`; their contents and the corrected original
paths in the index/manifests use `runs/`.

### Independent evaluation and source review

The returned snapshot was audited at
`0e963345aa59ec2c70615530d30a3bea6d467c51`. All 162 archive checksums and all 3,915
file hashes/sizes matched. The frozen analyzer reproduced the smoke, all-22 and
screen summaries exactly. GPU activity records contained 4,322 observations across
136 GPU processes, with idle start/end observations and at most one process ID per
run. The reviewer independently reproduced all twelve contrasts from the 24 timing
processes and accepted the decision to stop without confirmation.

A subsequent standards and specification review of S5 against its S4 base found
no actionable code findings. The architect retained S5 at production 0.9; no
additional experiment or implementation change was indicated. This does not
expand the performance claims above.

This final report corrects the originally returned source-tree value, dataset
byte total, MiB conversion and archive-index paths. Raw experiment files, binary
identities, timings and results remain unchanged.
