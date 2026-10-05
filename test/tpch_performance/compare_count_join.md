# Comparing dense count-join and eager aggregation

Run from the repository root with a build containing PR #1581 and DENSE_COUNT_JOIN.
The Python DuckDB package must match the extension's build, as for the existing
Python benchmark harness.

```bash
pixi run python test/tpch_performance/compare_count_join.py \
  --sf 50 --tpch-db /path/to/tpch_sf50.duckdb \
  --output /tmp/count-join-sf50
```

The database is opened read-only. Alternatively, use `--tpch-dir /path/to/parquet`
with `customer.parquet` and `orders.parquet`, or directories of parquet files
named `customer/` and `orders/`. Without either option, the script generates
synthetic data with SF-sized row counts: SF50 has 7.5 million customers and
75 million orders. That synthetic dataset includes unmatched customers, NULL
join keys, and NULL counted values; it is not reference TPC-H data.

For a small smoke run:

```bash
pixi run python test/tpch_performance/compare_count_join.py \
  --sf 0.1 --pairs 4 --output /tmp/count-join-smoke
```

The benchmark projects the customer key and the two required order columns into
new parquet files. For TPC-H inputs it applies q13's comment filter during this
preparation. It then pins those projected inputs to GPU memory once per process.
File decoding, comment filtering, pinning, and result validation are outside the
timed region. This isolates the two join/aggregation execution paths; it does not
measure full q13 from storage.

Both variants execute the same SQL fragment: `COUNT(o_orderkey)` over a LEFT JOIN,
grouped by the customer join key. The `dense` variant enables DENSE_COUNT_JOIN and
disables eager aggregation; the `eager` variant does the reverse. The eager force
option stays off, so the rewrite must pass its normal benefit checks.

Two workloads keep output small:

- `core`: the number of customer groups and sum of their counts above the matching
  fragment. These two scalar results avoid transferring millions of group rows.
- `q13`: the customer-count distribution and final sort from q13, using the
  prefiltered orders. Use `--workload core` or `--workload q13` to select one.

Every result is compared exactly with DuckDB CPU execution. CPU fallback is
disabled for GPU runs. Separate logged runs must confirm eager rewrites or
dense-count fusion, pinned scans of both inputs, and execution of the dense
histogram strategy. A sparse execution fails the run unless `--allow-sparse` is
specified. The raw strategy counts remain in the evidence file.

Timing runs disable logging and telemetry. They collect one initial sample and
two warmups for each algorithm, then ten hot samples each in alternating AB/BA
pairs. The report uses hot medians and records sample variability. Wall time
includes planning, GPU execution, and fetching the compact result; it excludes
changing settings and comparing results. A shared GPU lock serializes benchmark
processes that use that lock.

The output directory includes `REPORT.md`, `timings.csv`, CPU reference results,
path evidence and logs, SQL, configuration, binary/data/script hashes, and dataset
provenance. Pass `--config` for a base YAML configuration; the script copies it and
disables telemetry. Reuse a previous run's projected files with
`--prepared-data /tmp/count-join-sf50/data`. Output directories must be new.

## Measured SF50 results

Measured on 2026-10-05 with NVIDIA GB10, driver 580.126.09, DuckDB 1.5.6,
and engine commit `2bb638942ce2e79bf955053c4fde414f29ef26a6` (PR #1581).
The existing TPC-H SF50 database supplied 7,500,000 customers and
74,224,989 orders after q13's comment filter. Both inputs were GPU-pinned.
The configuration used one GPU, a 50% GPU memory usage limit, a 4 GiB host
tier, an 8 GiB disk tier, four pipeline threads, and one downgrade thread;
other settings used engine defaults.

| Workload | Dense median ms | Eager median ms | Eager / dense | Dense variability | Eager variability |
|---|---:|---:|---:|---:|---:|
| Core join/count | 53.761 | 180.024 | 3.349x | 2.2% | 0.9% |
| q13 distribution/sort | 56.781 | 182.795 | 3.219x | 1.7% | 1.1% |

The medians use ten hot samples each, following two warmups. Variability is
sample standard deviation divided by mean. The
[raw samples](count_join_sf50_gb10.csv) also retain the initial samples, excluded
from these summaries. All query results matched CPU execution exactly.
Each logged validation confirmed two dense-count fusions and two dense histogram
tasks for the dense variant, or two eager rewrites for the eager variant; both
variants confirmed two pinned scans of each input. No sparse strategy ran.

On this matching shape, eager aggregation taking precedence costs the advantage
of the fused histogram path. These timings exclude preparation, comment filtering,
and pinning, and do not establish the preferred algorithm for other key domains,
memory budgets, or hardware.
