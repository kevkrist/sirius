# Full SQL ordering comparison and prefetch diagnostics

The completed SF100 investigation and subsequent production decision are in [RESULTS.md](RESULTS.md).
Compact raw results, configuration, build identities, and campaign scripts are
retained under [results](results/CONTRACT.md). Large binaries and full profiler
traces remain at the absolute paths recorded by the manifests. Archived campaign
scripts retain this machine's paths; change those paths and the dataset location
when reproducing elsewhere.

These tools compare explicit sibling-first and own-first orders, independently of the current production order, using the actual embedded DuckDB/Sirius engine and its normal parquet scan path. They do not modify production sources, installed libraries, or the shared extension. The parent campaign fixes the dataset, queries, config, repetition protocol, correctness oracle, and GPU lease before execution.

The archived campaign ran before production switched to own-first at the user's request. Its sources and measurements remain unchanged. The current failure path preserves issuing siblings even when A's issue fails; the archived own-first timing variant checked A's terminal state earlier. Successful issue order is the same, but the archived timings are not a new measurement of the revised failure path.

## Build and run interface

From the repository root:

```bash
pixi run python bench/deferred-device-read-order/tpch/build_variants.py --output /tmp/tpch-deferred-order-build
```

The builder reuses the configured `sirius_nvtx_startup` full-engine compile/link recipe and current `libsirius_core.a`. It produces four executables:

- `tpch_siblings_first`: current core with the explicit sibling-first batch object linked first.
- `tpch_own_first`: the same core with the explicit own-first batch object linked first (currently byte-identical to production).
- `tpch_siblings_first_diagnostic`: sibling-first order with diagnostic-only batch, datasource, and cache objects.
- `tpch_own_first_diagnostic`: own-first order with identical diagnostics.

The build directory retains generated source snapshots/diffs, full compile/link commands, linker maps, and SHA256 identities for the binaries, core archive, production sources, and authored instrumentation. Only the own `issue_if_open()` call's placement relative to the sibling block changes between the timing variants; both check A's terminal state after issuing siblings. The shared checked transformation in `../prepare.py` rejects an unexpected source structure and records which policy matches production. No microbenchmark backend or injected latency is used here.

The runner interface is:

```bash
SIRIUS_CONFIG_FILE=/absolute/config.yaml pixi run /tmp/tpch-deferred-order-build/tpch_siblings_first WORKLOAD_DIRECTORY NEW_OUTPUT_DIRECTORY 3 3
```

`WORKLOAD_DIRECTORY/setup.sql` contains normal DuckDB setup SQL, such as views over the original parquet files. `queries.tsv` contains `LABEL<TAB>QUERY_FILE`, with paths relative to the workload directory or absolute. Query labels accept letters, digits, `_`, and `-`. Canonical query SQL is passed unchanged to DuckDB.

For each manifest entry the runner creates a new DuckDB connection/context, runs setup outside timing with GPU execution disabled, disables DuckDB fallback, and enables transparent GPU execution. It takes one `context_cold` sample, the requested number of `fresh` samples with `reset_sirius_cache()` before each, and the requested number of `retained` samples without a reset. Use one query per process when an unambiguous process/context boundary is required. `Connection::Query` returns a materialized result; result text serialization occurs after timing.

`--cpu` disables GPU execution for the query, providing independent DuckDB result data. The linked extension may still initialize GPU resources, so CPU-oracle processes also pass through the campaign's GPU lease. `--profile` brackets each query with `profiler_start`/`profiler_stop`; a query label is assigned after profiler start and before the timer. Profiling and diagnostic elapsed times are excluded from performance conclusions.

Outputs are `timings.csv`, one result TSV per `QUERY-REGIME-ITERATION`, and, for diagnostic executables, one JSON file with that same identity. Result TSVs contain column names, logical types, then exact textual values. Backslash, tab, carriage return, and newline are escaped; `\N` denotes NULL. The campaign compares every result with the CPU oracle using exact integer/decimal/string comparison and an explicit floating tolerance.

## Diagnostic interpretation

The diagnostic code tracks datasource-to-cache-handle addresses in generated constructor/destructor bodies without changing any class layout or header definition. A separate weak registry mirrors successful production registration and extraction. The first open await claims a mirror generation once, only if the requested batch belongs to that generation; this prevents concurrent or stale waits from double-counting the denominator. The mirror never consumes or alters the real registry.

Before either order issues any batch, `await` observes A under its batch mutex and the live, still-open peers under their respective mutexes. It never holds two batch mutexes together. Tracker locks are released before acquiring batch locks. Each event includes source path/address, stream, device, ranges, bytes, and a cohort identifier. These snapshots are observations across atomic state reads; they do not guarantee a subsequent stall because completion may race the observation.

The JSON event kinds are:

- `entry`, role `own`: one claimed first-await group. `open_siblings > 0` identifies groups where ordering can matter. `pending` means an overlapping requested chunk was observed loading and that handle's prefetch was observed in flight. `any_sibling_pending` applies the same predicate to the observed open siblings.
- `entry`, role `sibling`: the corresponding peer snapshots before issuance.
- `issue`: state observed immediately before the real read path, labeled `own` or `sibling` by an RAII-restored thread-local await scope.
- `wait`: the exact `await_inflight_prefetch` branch around `handle->wait_until_ready()`, with raw wall-clock `duration_ns`. A positive branch count is separate from a substantial duration; prefetch may complete just before the wait. Calls outside a deferred await use role `other`.

Producer states are `0 initialized`, `1 queued`, `2 preparing`, `3 prepared`, `4 loading`, `5 ready`, and `6 abandoned`. `handle=false` is distinct from those states. `already_issued_awaits` counts member waits that cannot supply a new first-open group.

Reject or explicitly mark incomplete any diagnostic sample with `dropped > 0` or `unclaimed_open_awaits > 0`; do not silently divide partial observations by a complete-cohort denominator. Events are bounded to 262144 per query, and observation failures are caught instead of changing production error behavior. Summed wait durations across threads are inclusive diagnostics, not predicted query savings.

Timing binaries contain none of these diagnostic objects. Link maps verify which `deferred_device_batch::await` implementation reached each executable. Successful GPU execution with fallback disabled, nonzero diagnostic issues, and actual open siblings establish target reachability; a zero-sibling query cannot establish an ordering improvement. The campaign additionally collects semantic/profiler evidence where supported.

## Assigned reading

For this extension of the benchmark task, the developer read the complete `operator-perf-hunt/SKILL.md` and `references/measurement-recipes.md`, then the campaign contract and reviewer's `/tmp/tpch-deferred-order-plan-review.md`. Applied separate attribution/timing runs, fixed balanced A/B repetitions, explicit cold/fresh/retained regimes, correctness and target-path gates, preserved build identities, and parent-owned serialized GPU scheduling.

The prior actual developer-role, C++ Software Design, C++20, Core Guidelines, cuDF documentation, and engine/API reading is recorded in [the original benchmark receipt](../README.md#developer-assigned-reading-receipt) and reused here. Newly inspected the full-engine CMake/link graph, `sirius_nvtx_startup` and extension loader, canonical TPC-H harness connection/cache/query/result paths, `deferred_device_batch` publication/await/registry paths, `sirius_datasource` handle ownership, cache `await_inflight_prefetch`, `find_entry`, producer state, and local cuDF/RMM APIs. `/module-context` remained unavailable per the earlier parent search, so local dependency headers/source supplied API details. No claim is made that entire books were reread.
