# TPC-H Performance Testing

Benchmarking tools for comparing DuckDB (CPU) vs Sirius (GPU) on TPC-H queries at various scale factors.

## Prerequisites

1. Build the project:
   ```bash
   pixi run make -j12
   ```

2. Ensure a Sirius config file exists. The binary looks for config in this order:
   1. `SIRIUS_CONFIG_FILE` environment variable (explicit path)
   2. `./sirius.yaml` in the current working directory
   3. `~/.sirius/sirius.yaml` in the user's home directory

3. Ensure parquet data exists (auto-generated if missing via `generate_tpch_data.sh`).
   - On the GB300 machine, the SF1000 dataset is at `/home/nvidia/tpch_parquet_sf1000`.

## Running Benchmarks

All commands run from the **project root** directory.

### Full benchmark with validation (recommended)

`benchmark_and_validate.sh` runs all 22 TPC-H queries, compares Sirius vs DuckDB results for correctness, and produces a timestamped run directory.

```bash
# Basic usage (uses ~/.sirius/sirius.yaml, both engines, dataset from test_datasets/)
./test/tpch_performance/benchmark_and_validate.sh 100

# With explicit options
./test/tpch_performance/benchmark_and_validate.sh \
  --config ~/.sirius/sirius.yaml \
  --parquet-dir /path/to/tpch_parquet_sf1000 \
  --engines "sirius duckdb" \
  1000

# Sirius only (skip DuckDB baseline)
./test/tpch_performance/benchmark_and_validate.sh \
  --config ~/.sirius/sirius.yaml \
  --parquet-dir /path/to/tpch_parquet_sf1000 \
  --engines sirius \
  1000

# Regenerate report from an existing run
./test/tpch_performance/benchmark_and_validate.sh --report runs/<run_dir>
```

Options:
- `--config <path>` — Sirius config file (default: `~/.sirius/sirius.yaml`)
- `--parquet-dir <path>` — parquet dataset directory (default: `test_datasets/tpch_parquet_sf<SF>`)
- `--engines <list>` — space-separated engine list (default: `"sirius duckdb"`)
- `--pinning-mode none|per-query|pinned-hot` — Sirius-only parquet pinning mode. `per-query` pins each query's referenced columns around that query block; `pinned-hot` pins the union of referenced columns once before the single-session run and unpins after all queries.

Each run creates a directory under `runs/<timestamp>_sf<SF>_2iter/` containing:
- `run_info.txt` — git branch/revision, tree clean/dirty, build freshness, hostname, memory, CPUs, GPUs, filesystem read benchmark
- `run_info.patch` — full git diff when tree is dirty
- `sirius_config.yaml` — copy of the Sirius config used
- `sirius/` and `duckdb/` — per-engine logs, per-query results and timings
- `validation.csv` — per-query match/error status
- `comparison.txt` — cold/warm timing table with speedup ratios
- `timings.csv` — long-format iteration runtimes (engine,query,iteration,runtime_s)

**Note:** The DuckDB baseline uses the same Sirius-built binary (`build/release/duckdb`) but with `SIRIUS_CONFIG_FILE` unset so the Sirius extension does not initialize. This means DuckDB runs on CPU using all available cores.

### Running individual queries

`run_tpch_parquet.sh` is the core runner used by all benchmarks. It runs queries in a single DuckDB session with 2 iterations each (cold + warm, back-to-back) and auto-generates missing datasets.

```bash
# Run Sirius on queries 1-22
SIRIUS_CONFIG_FILE=~/.sirius/sirius.yaml \
  ./test/tpch_performance/run_tpch_parquet.sh sirius 100 $(seq 1 22)

# Run DuckDB baseline (no config needed)
./test/tpch_performance/run_tpch_parquet.sh duckdb 100 $(seq 1 22)

# Run specific queries with custom parquet directory
SIRIUS_CONFIG_FILE=~/.sirius/sirius.yaml \
  ./test/tpch_performance/run_tpch_parquet.sh --parquet-dir /data/tpch sirius 100 1 3 6

# Run Sirius with union-pinned hot cache across the query stream
SIRIUS_CONFIG_FILE=~/.sirius/sirius.yaml \
  ./test/tpch_performance/run_tpch_parquet.sh --pinning-mode pinned-hot --iterations 5 sirius 100 $(seq 1 22)
```

Environment variables:
- `SIRIUS_CONFIG_FILE` — path to Sirius config (required for sirius engine; unset automatically for duckdb engine)
- `TIMING_CSV` — path to write per-query timing CSV (optional)
- `OUTPUT_DIR` — directory for structured output (set by `benchmark_and_validate.sh`)

### Generating telemetry

Telemetry is controlled by the Sirius YAML config used for the run. Enable Quent
export and choose the output directory:

```yaml
sirius:
  telemetry:
    enable_quent: true
    output_directory: telemetry_data
    engine_name: siriusDB
```

`run_tpch_parquet_and_generate_telemetry.sh` runs TPC-H queries in Sirius,
labels each `(query, iteration)` pair with `sirius_set_query_label`, and writes
Quent postcard files to `sirius.telemetry.output_directory`.

```bash
pixi run -- ./test/tpch_performance/run_tpch_parquet_and_generate_telemetry.sh \
  --iterations 1 \
  --parquet-dir /data/tpch/sf100/p16/zstd-8/ \
  100
```

The final `100` is the TPC-H scale factor. If no query numbers are provided,
all 22 queries are run; append query numbers to limit the run, for example
`100 1 6 9`.

The script uses `test/tpch_performance/tpch_telemetry_sirius.yaml` by default.
That config only enables telemetry, so pass `--config <path>` when the workload
also needs custom memory, executor, scan-cache, or operator settings:

```bash
pixi run -- ./test/tpch_performance/run_tpch_parquet_and_generate_telemetry.sh \
  --config ~/.sirius/sirius.yaml \
  --iterations 1 \
  --parquet-dir /data/tpch/sf100/p16/zstd-8/ \
  100 1 6 9
```

The custom config is used as-is, so it must include
`sirius.telemetry.enable_quent: true`.

Query labels are optional but make the Quent UI easier to navigate. They can be
set in either of two ways:

```sql
-- Applies to the next Sirius query, including transparent plain-SQL execution.
CALL sirius_set_query_label('tpch_q1_iter1');
SELECT *
FROM lineitem
WHERE l_orderkey < 100;

-- Inline label for an explicit gpu_execution call.
CALL gpu_execution(
  'SELECT * FROM lineitem WHERE l_orderkey < 100',
  query_label = 'tpch_q1_iter1'
);
```

The telemetry helper script uses `sirius_set_query_label` so plain SQL queries
keep the same execution path as the normal TPC-H runner.

Start the Quent analyzer server over the telemetry directory:

```bash
pixi run quent
```

The `quent` Pixi task defaults to `telemetry_data` and runs the telemetry server
with the UI enabled. If the config writes telemetry somewhere else, pass that
path as the task argument:

```bash
pixi run quent /path/to/telemetry_data
```

Open `http://localhost:8080` and select the captured Sirius engine/query.

## Query Files

- `tpch_queries/orig/q*.sql` — Plain SQL queries used by both Sirius and DuckDB runners

## Sirius Configuration

The Sirius config file (e.g. `~/.sirius/sirius.yaml`) controls:
- **GPU memory**: `usage_limit_fraction`, `reservation_limit_fraction`, `downgrade_trigger_fraction`, `downgrade_stop_fraction`
- **Host memory**: `capacity_bytes`, `initial_number_pools`, `pool_size`, `block_size`
  - Initial allocation = `initial_number_pools * pool_size * block_size`
- **Thread pools**: `pipeline`, `task_creator`, `downgrade` thread counts (the scan manager's
  `uring_n_reactors` defaults to `min(pipeline num_threads, 8)`)
- **Operator params**: `scan_task_batch_size`, `hash_partition_bytes`, `concat_batch_bytes`
- **Telemetry**: `telemetry.enable_quent`, `telemetry.output_directory`, `telemetry.engine_name`

### Example config (GB300, SF1000)

```yaml
sirius:
  topology:
    num_gpus: 1
  memory:
    gpu:
      usage_limit_fraction: 0.9
      reservation_limit_fraction: 1.0
      downgrade_trigger_fraction: 0.8
      downgrade_stop_fraction: 0.6
    host:
      capacity_bytes: 471200000000       # ~471 GB
      initial_number_pools: 60           # ~32 GB pre-fault; grows on demand up to capacity_bytes
      pool_size: 512
      block_size: 1048576                # 1 MB
  executor:
    pipeline:
      num_threads: 8                     # uring_n_reactors follows: min(pipeline threads, 8)
    downgrade:
      num_threads: 1
    task_creator:
      num_threads: 6
  operator_params:
    scan_task_batch_size: 5368709120       # 5 GB
    max_sort_partition_bytes: 0            # 0 = auto (33% GPU memory)
    hash_partition_bytes: 5368709120       # 5 GB
    concat_batch_bytes: 5368709120         # 5 GB
  telemetry:
    enable_quent: true
    output_directory: telemetry_data
    engine_name: siriusDB
```

Two sizing rules matter for hot-iteration performance on GB-class machines:

- **Pair pipeline threads with IO reactors.** Hot iterations of a local-disk run are bound by the
  page-cache-to-pinned copies that the scan manager's uring reactor threads execute inline
  (~5 GB/s each), so reactors must scale with the number of scan splits the pipeline can have in
  flight. The default does this automatically (`uring_n_reactors = min(pipeline num_threads, 8)`);
  only set `executor.scan_manager.uring_n_reactors` explicitly to pin a different count. Measured
  on GB300 SF1000: q19 hot time fell from 15.06 s at 1 reactor to 2.98 s at 8 reactors x 8
  pipeline threads; 16x16 was only marginally better than 8x8 on the cache-served suite (5.04 s vs
  5.60 s), which is why the derivation caps at 8.
- **Leave page-cache headroom for the scan working set.** `initial_number_pools x pool_size x
  block_size` of pinned host memory is pre-faulted at startup and never returned to the OS. Size it
  so node-0 RAM minus that pre-fault comfortably exceeds the hot scan working set, or the page
  cache cannot retain the dataset between iterations and hot runs flip unpredictably between
  cache-served (memcpy-bound) and disk-bound modes. On the 498 GB-per-node GB300, the earlier
  `initial_number_pools: 785` (~421 GB pre-fault) left only ~85 GB of cache against a ~98 GB
  SF1000 working set (q19 72 GB + q1 26 GB); 60 pools (~32 GB) leave ample margin, and the pool
  still grows on demand up to `capacity_bytes`.
