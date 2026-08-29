# TPC-H SF50 Q1/Q3 Performance Investigation and Reimplementation Notes

This document is the durable handoff for the 2026-08-28 through 2026-08-29 investigation of
non-pinned TPC-H SF=50 Q1 and Q3. It records the benchmark contract, optimized configuration,
Quent and Nsight Systems evidence, accepted general optimizations, rejected experiments, and the
remaining implementation plan. TPC-H is an acceptance workload throughout; no production path may
select an optimization from a query number, table or column name, SQL constant, or exact TPC-H
shape.

The raw evidence is under `/tmp` and is therefore not durable. Preserve the three artifact roots
listed in [Artifact inventory](#artifact-inventory) before the machine is cleaned.

## Status and ownership

The status words in this document are deliberate:

| Status | Meaning |
|---|---|
| **Accepted in Sirius** | General implementation is in the current Sirius history and passed its correctness/performance gates. |
| **Measured prototype** | Demonstrated useful headroom, but must not be shipped in its current form. |
| **Reverted** | Removed because its selector or implementation was query-shaped, despite a measured win. |
| **Proposed** | Design and measurement gate are recorded, but it has not been implemented. |
| **Upstream-only** | The implementation belongs in the cuCascade repository; Sirius should consume it through a published submodule commit. |

Relevant Sirius history at the end of the investigation:

| Commit | Status | Purpose |
|---|---|---|
| `09716f59` | **Accepted in Sirius** | Deduplicate alias-equivalent local grouped-aggregate states. |
| `70e68c85`, `416c63aa` | superseded foundation | Introduce the guarded tiny-domain path and its planner/test coverage. |
| `97b92dd1` | **Reverted** by `415a8cd3` | Fuse the exact Q1 projection shape into its aggregate. This must not be restored as written. |
| `a907db12` | **Accepted in Sirius** | Replace the exact-layout register branch with a general, resource-bounded small-domain aggregate. |
| `69f215db`, `6de3a005` | **Upstream-only prototype integration** | Point Sirius at private cuCascade pinned-pool commits and adjust resource wiring. These are evidence, not the desired ownership model. |

The final measured local tree pointed the cuCascade submodule at private commit `9f767cb`
(`eca1b37` plus an event-query error fix). Because cuCascade is a submodule, that is not the final
integration model. Reimplement the allocator work in the upstream cuCascade PR flow, resolve its collision with
[NVIDIA/cuCascade PR 185](https://github.com/NVIDIA/cuCascade/pull/185), and then update only the
Sirius gitlink and the smallest necessary wiring. Do not maintain a parallel allocator fork inside
Sirius.

## Executive result

The initial configuration sweep closed almost all of Q3's gap and much of Q1's gap. The accepted
aggregate work plus the measured pinned-allocation prototype then made Q3 narrowly faster than the
frozen DuckDB result and reduced Q1 to about 19% behind DuckDB.

All times below are seconds. Every Sirius result is Parquet, grouped mode, and `--pin none`.

| Implementation | Q1 | Q3 | Q1 vs DuckDB | Q3 vs DuckDB |
|---|---:|---:|---:|---:|
| DuckDB frozen hot median | 0.523003 | 0.675181 | — | — |
| Sirius `1ad67e7f`, stock config | 1.333202 | 1.309351 | +154.91% | +93.93% |
| Sirius `1ad67e7f`, optimized config | 0.796109 | 0.690235 | +52.22% | +2.23% |
| Matched pre-change control `415a8cd3` | 0.658797 | 0.690002 | +25.96% | +2.20% |
| Combined general candidate `9be8d29c` | **0.622256** | **0.667125** | **+18.98%** | **-1.19%** |

The first two Sirius rows use the initial five-iteration campaign. The final two use a six-iteration
A/B/B/A campaign and pool ten hot samples per implementation, so use each pair for its intended
comparison rather than treating the whole table as one randomized trial.

Relative to matched `415a8cd3`, the combined candidate improved Q1 by 5.55% and Q3 by 3.32%.
Individual hot medians were consistent:

| Leg | Q1 | Q3 |
|---|---:|---:|
| Control A1 | 0.662249 | 0.691805 |
| Candidate B1 | 0.623860 | 0.669142 |
| Candidate B2 | 0.620651 | 0.665615 |
| Control A2 | 0.658601 | 0.688235 |

All eight A/B/B/A query validations passed. A final rebuilt-`dev` smoke run also passed Q1 with the
configured numeric tolerance and Q3 byte-for-byte.

The final Q1+Q3 sum is 1.289381 s versus DuckDB's 1.198184 s, so it remains 7.61% slower despite
Q3's narrow win.

The important conclusion is asymmetric:

- Q3 is already at parity in this regime. Its largest early problem was an avoidable partitioned
  join and allocation churn, not the final group-by/top-N tail.
- Q1 remains GPU-work-bound. The final hot trace has only 44.77 ms of total GPU-idle gaps, while its
  GPU-activity union is already 561.65 ms, longer than DuckDB's complete 523.00 ms result. Host
  scheduling cleanup alone cannot close Q1.

## Benchmark contract

### Machine and data

- NVIDIA GB10 GPU, driver 580.126.09.
- 20 ARM CPU cores and 119 GiB unified memory.
- Local NVMe ext4 storage.
- Dataset: `test_datasets/tpch_parquet_sf50`, approximately 17 GiB.
- Important cardinalities: `lineitem` 300,005,811 rows, `orders` 75,000,000 rows, and `customer`
  7,500,000 rows.
- Baseline source commit: `1ad67e7f`.
- Data placement: no table pinning; every canonical command uses `--pin none`.

Iteration zero is Sirius-cache-cold but not a controlled OS-cache-cold result. Iterations one onward
are the hot regime. Do not present iteration zero as a storage cold-start comparison unless the OS
cache and device state have also been reset by a declared procedure.

### Timing protocol

Use the repository's [TPC-H performance harness](../../test/tpch_performance/run.md). The canonical
screen command is:

```bash
flock -w 600 /tmp/sirius-perf-gpu.lock \
  pixi run python test/tpch_performance/performance_test.py \
    --input /home/kkristensen/Code/sirius_1/test_datasets/tpch_parquet_sf50 \
    --data-source parquet \
    --mode grouped \
    --iterations 5 \
    --queries 1,3 \
    --pin none \
    --engine gpu \
    --config /tmp/sirius-q1q3-sf50-prefetch-20260828T141828Z/configs/prefetch_1m_scan1536m_build512m.yaml \
    --duckdb-results /tmp/sirius-q1q3-sf50-prefetch-20260828T141828Z/baseline/tpch_20260828_142246_q1q3_sf50_cpu_b1 \
    --name <run-id> \
    --output <artifact-root>
```

For a promotion verdict:

1. Build A and B from clean, recorded source trees and record binary/config hashes.
2. Run independent processes in A/B/B/A order under both the GPU and thermal locks.
3. Use six iterations per leg, discard iteration zero, and pool ten hot samples per
   implementation. Also report each leg median so drift remains visible.
4. Run Q1 then Q3 in every block. Do not change order between variants.
5. Require every `validation.csv` row to succeed and inspect logs for fallback, downgrade, OOM,
   exception, or error markers.
6. Freeze B, then run a holdout A/B pair that was not used to tune it.
7. Use unprofiled runs for timing. Quent and Nsight are mechanism/attribution evidence only.

The exact final driver is
`/tmp/sirius-general-small-domain-20260829-artifacts/run_abba_combined_9be8d29c.sh` and the raw result
root is `/tmp/sirius-general-small-domain-20260829-artifacts/combined-9be8d29c-vs-415a8cd3`.
The script does not acquire the locks itself; invoke it in thermal-then-GPU order:

```bash
flock -w 600 /tmp/sirius-perf-thermal.lock \
  flock -w 600 /tmp/sirius-perf-gpu.lock \
    /tmp/sirius-general-small-domain-20260829-artifacts/run_abba_combined_9be8d29c.sh
```

Q1's initial hot MAD was 10.661 ms, Q3's was 1.277 ms, and the combined figure-of-merit MAD was
8.525 ms. The frozen DuckDB Q3 block MAD was 6.206 ms. Require at least a 25 ms replicated Q3 win
for a new Q3 mechanism; merely crossing one 15 ms baseline gap is not robust evidence.

### DuckDB reference

Generate a fresh CPU reference with the same dataset, query order, and grouped harness, then pass
its result directory to all Sirius runs:

```bash
pixi run python test/tpch_performance/performance_test.py \
  --input /home/kkristensen/Code/sirius_1/test_datasets/tpch_parquet_sf50 \
  --data-source parquet \
  --mode grouped \
  --iterations 5 \
  --queries 1,3 \
  --pin none \
  --engine cpu \
  --name tpch_sf50_cpu_reference \
  --output <artifact-root>
```

The frozen reference used here is
`/tmp/sirius-q1q3-sf50-prefetch-20260828T141828Z/baseline/tpch_20260828_142246_q1q3_sf50_cpu_b1`.
The frozen hot medians are Q1 0.523003 s and Q3 0.675181 s.

## Optimized configuration

This was the best measured complete YAML for the final candidate:

```yaml
sirius:
  topology:
    num_gpus: 1
  memory:
    gpu:
      usage_limit_bytes: 48000000000
      reservation_limit_fraction: 1.0
    host:
      capacity_bytes: 24000000000
      initial_number_pools: 10
      pool_size: 512
      block_size: 1048576
  executor:
    scan_manager:
      enable_prefetch_cache: true
      uring_n_reactors: 1
      local:
        use_odirect: true
        max_n_chunks: 1
      cache:
        inflight_io_chunk_budget: 2048
        eviction_threshold_fraction: 0.6
        min_prefetching_budget_fraction: 0.05
        dispose_after_use: false
    pipeline:
      num_threads: 8
    task_creator:
      num_threads: 4
    downgrade:
      num_threads: 2
      monitor_period: 10ms
  operator_params:
    scan_task_batch_size: 1610612736
    max_sort_partition_bytes: 0
    hash_partition_bytes: 536870912
    concat_batch_bytes: 536870912
    max_build_hash_table_bytes: 536870912
    enable_tiny_domain_grouped_aggregate: true
    enable_compressed_materialization: false
  telemetry:
    enable_quent: false
```

The original config-only winner omits the last two experimental operator flags and is saved at
`/tmp/sirius-q1q3-sf50-prefetch-20260828T141828Z/configs/prefetch_1m_scan1536m_build512m.yaml`.
The final file above is
`/tmp/sirius-general-small-domain-20260829-artifacts/abba-feature-toggle-cabf9676/configs/tiny_on.yaml`.

This is a hardware/workload measurement, not a proposal to make every value a production default.
`enable_tiny_domain_grouped_aggregate` intentionally remains false by default. Sirius resolves a
runtime config from `SIRIUS_CONFIG_FILE`, then `./sirius.yaml`, then `~/.sirius/sirius.yaml`; the
repository has test/example configs but no universal production-default YAML. See
[Configuration](configuration.md).

### What the config changed

The two high-impact general mechanisms were larger scan tasks and a larger build/hash cap:

| Query | Stock tasks | Tuned tasks | Stock kernels | Tuned kernels | Stock GPU-activity union | Tuned union |
|---|---:|---:|---:|---:|---:|---:|
| Q1 | 60 | 22 | 3,682 | 1,233 | 1.2422 s | 0.7238 s |
| Q3 | 78 | 28 | 1,532 | 642 | 1.2465 s | 0.6087 s |

`scan_task_batch_size: 1610612736` reduces task creation, launches, synchronization boundaries, and
small transfers. The 1.5--2 GiB region was best; 256 MiB was substantially worse and values above
2 GiB began to regress Q1.

For Q3, raising `max_build_hash_table_bytes` from the former 90 MB regime to 512 MiB lets the
177,304,200-byte second build remain a single broadcast `BUILD_PROBE` plan with a dynamic filter,
instead of entering `STANDARD` partitioned join. In the mechanism screen this moved Q3 from about
0.7586 s to 0.7080 s and reduced device-to-device traffic from 3.81 GB to 189.8 MB.

The scan prefetch cache retained reusable one-MiB file ranges in pinned host memory. In these local
O_DIRECT runs it behaved primarily as demand retention/reuse; it was not a general proactive local
read-ahead engine. Enabling it at the winning scan size improved Q1 by 14.1% and Q3 by 22.8%. Its
steady measured footprint was approximately 4.49 GiB with no eviction in the hot Q1/Q3 sequence.

Rejected configuration variants were buffered I/O as the default, two or four reactors, four or
sixteen local chunks, 2/4/16/64 MiB cache units, 0.8/0.95 eviction thresholds, 4/6/12 pipeline
threads, AST JIT/materialization, a 256 MiB scan target, and scan targets above 2 GiB. Buffered I/O
occasionally saved roughly 10 ms on hot Q3 but regressed Q1, the combined figure of merit, and cold
runs. A 5 GiB scan target was neutral for Q3 and regressed Q1 by about 89 ms.

## Profile handoff

### Quent

The validated final telemetry run is:

- Data: `/tmp/sirius-general-small-domain-20260829-artifacts/profiles/quent/data`
- Harness output:
  `/tmp/sirius-general-small-domain-20260829-artifacts/profiles/quent/run/tpch_20260829_192525_combined_general_9be8d29c_quent`
- Config: `/tmp/sirius-general-small-domain-20260829-artifacts/profiles/quent/tiny_on_quent.yaml`
- Representative median-duration hot Q1 label: `q1_iter1`
- Engine ID: `01a04efb-ed92-7330-bbbe-6c2ee2e123c2`
- Q1 query ID: `01a04efb-f19e-7753-b0d0-ab229922a9b3`

The telemetry delta applied to the complete optimized YAML was:

```yaml
sirius:
  telemetry:
    enable_quent: true
    enable_batch_events: true
    exporter: postcard
    output_directory: /tmp/sirius-general-small-domain-20260829-artifacts/profiles/quent/data
    engine_name: sirius-general-combined-9be8d29c
```

Change `output_directory` when restoring the capture. The exact harness shape was:

```bash
flock -w 600 /tmp/sirius-perf-gpu.lock \
  pixi run python test/tpch_performance/performance_test.py \
    --input /home/kkristensen/Code/sirius_1/test_datasets/tpch_parquet_sf50 \
    --data-source parquet \
    --mode grouped \
    --iterations 4 \
    --queries 1,3 \
    --pin none \
    --engine gpu \
    --config /tmp/sirius-general-small-domain-20260829-artifacts/profiles/quent/tiny_on_quent.yaml \
    --duckdb-results /tmp/sirius-q1q3-sf50-prefetch-20260828T141828Z/baseline/tpch_20260828_142246_q1q3_sf50_cpu_b1/duckdb \
    --name combined_general_9be8d29c_quent \
    --output /tmp/sirius-general-small-domain-20260829-artifacts/profiles/quent/run
```

If the original analyzer is still running, inspect:

- `http://localhost:18082/profile/engine/01a04efb-ed92-7330-bbbe-6c2ee2e123c2/query/01a04efb-f19e-7753-b0d0-ab229922a9b3/timeline`
- `http://localhost:18082/profile/engine/01a04efb-ed92-7330-bbbe-6c2ee2e123c2/query/01a04efb-f19e-7753-b0d0-ab229922a9b3/operators`

To restore it, follow [Quent Telemetry](quent-telemetry.md) or run the captured server directly:

```bash
pixi run rust/target/debug/sirius-telemetry-server \
  --log-level warn \
  --output-dir /tmp/sirius-general-small-domain-20260829-artifacts/profiles/quent/data \
  --exporter postcard \
  --analyzer-address 127.0.0.1:18082 \
  --collector-address 127.0.0.1:17838
```

Entity filenames in the Postcard directory are storage records, not necessarily the engine/query
IDs used by the UI. Resolve queries from the analyzer using their `q1_iterN`/`q3_iterN` labels; do
not infer UI IDs from filenames.

The Quent capture verifies plan shape, task/pipeline activity, and absence of fallback. It is not a
timing verdict because telemetry changes execution cost.

### Nsight Systems

The final GUI reports and exported SQLite databases are:

| Query | GUI report | SQLite |
|---|---|---|
| Q1 | `/tmp/sirius-general-small-domain-20260829-artifacts/profiles/nsys/tpch_20260829_192420_combined_general_9be8d29c_nsys/sirius/q1/nsys.nsys-rep` | `/tmp/sirius-general-small-domain-20260829-artifacts/profiles/nsys/tpch_20260829_192420_combined_general_9be8d29c_nsys/sirius/q1/nsys.sqlite` |
| Q3 | `/tmp/sirius-general-small-domain-20260829-artifacts/profiles/nsys/tpch_20260829_192420_combined_general_9be8d29c_nsys/sirius/q3/nsys.nsys-rep` | `/tmp/sirius-general-small-domain-20260829-artifacts/profiles/nsys/tpch_20260829_192420_combined_general_9be8d29c_nsys/sirius/q3/nsys.sqlite` |

Open the `.nsys-rep` files in Nsight Systems. For SQL analysis, use the second `sirius::query` NVTX
range (`sirius::query/1`), which is the hot profiled iteration. The profile harness rejects
`--duckdb-results`, so profile path activation separately and keep a normal grouped validation run
as the correctness record.

The capture command was:

```bash
flock -w 600 /tmp/sirius-perf-gpu.lock \
  pixi run python test/tpch_performance/performance_test.py \
    --input /home/kkristensen/Code/sirius_1/test_datasets/tpch_parquet_sf50 \
    --data-source parquet \
    --mode nsys-profile \
    --iterations 2 \
    --queries 1,3 \
    --pin none \
    --engine gpu \
    --config /tmp/sirius-general-small-domain-20260829-artifacts/abba-feature-toggle-cabf9676/configs/tiny_on.yaml \
    --name combined_general_9be8d29c_nsys \
    --output /tmp/sirius-general-small-domain-20260829-artifacts/profiles/nsys
```

Do not add `--duckdb-results` to this invocation.

#### Final Q1 profile

- Profiled runtime: 0.625192 s; hot NVTX wall: 606.422 ms.
- Kernel/copy/memset interval union: 561.652 ms, or 92.6% of wall.
- All uncovered GPU gaps combined: 44.77 ms.
- CUDA APIs: 22,967 calls, 2,332.669 ms summed inclusive time. Stream synchronization accounts for
  1,930.947 ms of that concurrent inclusive sum; it is not equivalent to wall-idle time.
- Kernels: 1,026 launches, 1,746.065 ms summed concurrent duration.
- Copies: 3,843 calls and 67.397 ms summed duration.
- H2D: 2,651.012 MB in 3,326 copies, 51.393 ms on the device timeline.
- `decode_page_data_generic`: 742.658 ms summed kernel time.
- `unsnap_kernel`: 589.673 ms. Decode plus unsnap is 76.3% of summed kernel work.
- The source pipeline materializes approximately 22.808 GB of logical intermediates for an output
  of only about 14 KiB.
- General bounded-register accumulation: 9 launches, 69.428 ms; whole bounded-register family
  71.070 ms.
- The capacity-4/state-6 kernel uses 80 registers/thread, no local or stack memory, a 256-thread
  block, and three resident blocks/SM on GB10.
- The two remaining materialized projections account for 77.569 ms summed correlated GPU work and
  74.905 ms of their own interval union. Removing their fixed trace intervals reduces total GPU
  union by 36.911 ms, a conservative non-additive lower bound.
- There are zero `cudaHostAlloc`, `cudaMallocHost`, and `cudaFreeHost` calls in the hot query window
  with the pinned-pool candidate.

#### Final Q3 profile

- Profiled runtime: 0.672315 s; hot NVTX wall: 653.298 ms.
- GPU-activity union: 596.838 ms, or 91.4% of wall; gaps total 56.46 ms.
- Kernels: 642 launches, 1,225.910 ms summed concurrent duration.
- H2D: 3,637.651 MB in 4,678 copies, 89.061 ms.
- `unsnap_kernel`: 582.540 ms; decode: 470.784 ms. Together they are 85.9% of summed kernel work.
- Transform kernels: 102.623 ms summed.
- Dynamic-filter work: 47.321 ms summed; hash join 17.931 ms; group-by 2.097 ms; top-N 3.223 ms.
- The bounded small-domain aggregate does not activate, as expected.
- There are zero host-pinned allocation/free calls in the hot window with the prototype pool.

Summed API and kernel durations exceed wall time because Sirius overlaps multiple streams and host
threads. Only interval-union and critical-path analysis may be compared directly with wall time.

### DuckDB-side attribution

The initial campaign also captured DuckDB operator CPU time. This was an attribution run, not the
frozen timing row above:

- Q1 wall was 0.624 s with 11.890 CPU-seconds, approximately 19.0 cores busy. `READ_PARQUET`
  accounted for 8.706 CPU-seconds and `HASH_GROUP_BY` for 2.575 CPU-seconds.
- Q3 wall was 0.759 s with 13.715 CPU-seconds, approximately 18.1 cores busy. Its scans accounted
  for 9.764 CPU-seconds and joins for 3.621 CPU-seconds.
- Neither query spilled.

DuckDB's summed CPU time, like Sirius's summed concurrent GPU kernels, must not be compared directly
with wall time. It does show that DuckDB keeps almost all CPU cores productively occupied in its
Parquet scan and group/join paths without a spill. Sirius's remaining Q1 gap is not explained by an
idle host: the Sirius GPU-activity union itself exceeds DuckDB's full frozen wall result and is
dominated by decompression/decode plus materialized arithmetic.

## Sirius-owned general optimizations

### Alias-equivalent grouped-state deduplication

**Status:** accepted in Sirius as `09716f59`; isolated measurement commit `93245640`.

DuckDB's logical AVG decomposition leaves Q1 with a visible SUM and the same SUM used as an AVG
numerator for both quantity and extended price. The old local cuDF group-by computed all eleven
logical states independently. The optimization canonicalizes exact alias-equivalent, fixed-width
requests, computes nine physical states, and reconstructs the two duplicate logical result slots.

This is a general source-identity optimization, not common-value elimination. Two columns that
happen to contain equal values must remain distinct. A reusable implementation needs the following
identity and exclusion contract.

#### Reimplementation contract

For each local aggregation request, construct an identity containing:

- aggregation kind;
- full logical/carrier type, including decimal scale;
- view size and slice offset;
- data-head pointer;
- null-mask pointer.

Only canonicalize the explicit, parameter-free MIN, MAX, COUNT, and SUM family over supported
fixed-width views. Exclude the whole original request if any member is empty, variable-width,
nested, `COLLECT_SET`, parameterized, or otherwise outside the proven set. Whole-request exclusion
keeps the partial schema and merge assumptions simple.

Build a `logical_state -> physical_state` map in original order. Submit only the first member of
each identity class to cuDF. After the local group cardinality is known, deep-copy a physical result
column for every repeated logical slot, preserving original output order, type, decimal scale, and
null mask. Do not alias output column ownership. COUNT output-width decisions must still use each
original request's multiplicity rather than the deduplicated list.

The implementation changes only local request construction and result reconstruction. It must not
change planner expressions, input ownership, stream order, downstream merge schema, retry, or
downgrade behavior.

#### Code and tests

- Implementation:
  [`src/op/aggregate/gpu_aggregate_impl.cpp`](../../src/op/aggregate/gpu_aggregate_impl.cpp)
- Differential tests:
  [`test/cpp/operator/aggregate/test_gpu_merge_impl.cpp`](../../test/cpp/operator/aggregate/test_gpu_merge_impl.cpp)

Required cases are duplicate SUM; nullable `COUNT_VALID` and `COUNT_ALL`; schema/order/type/scale;
sliced aliases; different offsets; different null masks; equal-valued separate allocations; and a
mixed request containing `COLLECT_SET` that forces whole-request exclusion.

The measured focused suite passed 4 cases/42 assertions, the broader grouped-aggregate suite passed
33 cases/250 assertions, and all sixteen timed Q1/Q3 validations passed. The isolated pooled hot
result was Q1 0.787985 -> 0.761868 s (-26.118 ms, -3.31%). Q3 moved
0.687309 -> 0.690505 s (+3.196 ms), inside the noise/hold gate. Nsight attributed the Q1 change to
a reduction in source aggregate-kernel union from 191.230 ms to 153.465 ms with launch count
unchanged.

Full evidence:
`/tmp/sirius-q1q3-nextsteps-20260828/30-implementation/q1-groupby-state-dedup/RESULTS.md`.

### General bounded-register small-domain aggregation

**Status:** accepted in Sirius as `a907db12`; isolated implementation `cabf9676`.

The initial tiny-domain work proved that a small observed group domain should not use a general
hash group-by for every local batch. The accepted implementation selects a register-private or
exact-wide local path from general plan semantics, observed data, and GPU resource limits.

The final tree is general, but its Git ancestry includes `70e68c85`, whose subject and early
implementation were Q1-oriented. A clean review branch should recreate or squash the final general
tree; do not present the intermediate query-shaped history as the desired abstraction.

#### Planner admission

Planner admission in
[`src/planner/sirius_plan_aggregate.cpp`](../../src/planner/sirius_plan_aggregate.cpp) requires:

- one or two grouping expressions with supported byte-packable `INT8`, `UINT8`, or one-byte
  `VARCHAR` representations;
- supported plain/combinable SUM, AVG, COUNT, MIN, or MAX semantics and supported carrier types;
- no DISTINCT, aggregate FILTER, aggregate ORDER BY, grouping functions, or unsupported grouping
  modes;
- `enable_tiny_domain_grouped_aggregate: true`.

Do not admit from a cardinality estimate or a known SQL shape. Planner admission means only that a
runtime attempt is legal; it does not promise that the fast strategy will activate.

#### Runtime admission and algorithm

[`try_tiny_domain_grouped_aggregate`](../../src/include/op/aggregate/tiny_domain_grouped_aggregate_impl.hpp)
must revalidate the actual input before output publication:

1. Validate key and aggregate column types, view offsets, row counts, aggregate kinds, nullability,
   and source mappings. A string key is eligible only when every non-null value is exactly one byte.
2. Deduplicate physical accumulator states by exact input-view identity and aggregate semantics.
   Maintain an arbitrary logical-to-physical map; never assume a particular number or order of
   logical aggregates.
3. Inspect a 64-Ki-row prefix to discover key codes without forcing a complete extra scan on a
   large batch. Continue to validate keys in the main kernel; an unseen/invalid late key fails
   closed.
4. Decline the tiny-domain implementation above 64 observed groups. The bounded-register branch is
   stricter: at most 32 groups, at most eight deduplicated physical states, and at most 32 rounded
   group-state entries.
5. Dispatch capacity bins 1, 2, 4, 8, 16, or 32 rather than compiling an exact query layout. Use
   CUDA occupancy for the concrete kernel and require at least two resident blocks/SM. The direct
   unmasked 64-bit specialization may target three blocks/SM when it has no more than 24 entries.
6. Accumulate signed 64-bit partials in registers, including nullable SUM and COUNT variants. AVG
   uses its SUM and valid-count states through the existing logical mapping.
7. Detect signed overflow and invalid post-launch state before publication. When an `INT64` bounded
   partial overflows, recompute that batch through the exact-wide tiny-domain strategy. If an exact
   retry is not possible or fails, throw; never publish a wrapped partial.
8. Materialize keys and logical aggregate carriers in exactly the original order only after every
   runtime check succeeds.

Unsupported semantics or representations return a fallback reason and leave the caller free to run
ordinary cuDF group-by on the original input. Allocation, CUDA, stream, and memory-resource errors
propagate; they are not semantic fallbacks. The physical operator integration is in
[`src/op/sirius_physical_grouped_aggregate.cpp`](../../src/op/sirius_physical_grouped_aggregate.cpp),
and the CUDA implementation is
[`src/cuda/tiny_domain_grouped_aggregate_impl.cu`](../../src/cuda/tiny_domain_grouped_aggregate_impl.cu).

In the measured Q1 trace the general rules happened to observe four groups, eleven logical states,
and six deduplicated physical states. These numbers are observations, never activation constants.
The selected GB10 kernel used 80 registers/thread, no stack/local memory, and three blocks/SM.

#### Tests and performance

The `[tiny_domain]` suite must cover:

- aliased and equal-valued-but-separate sources;
- nullable columns and arbitrary slices/offsets;
- arbitrary aggregate subsets and output orders;
- each capacity bin and its adjacent decline boundary;
- prefix-discovered and late unseen keys;
- more than 64 groups;
- overflow, cancellation near the overflow boundary, and exact-wide retry;
- one- and two-key forms, including two string keys;
- a planner-level three-key decline;
- unsupported semantics leaving the normal plan unchanged.

The exact final suite passed 16 cases/718 assertions. Run it with:

```bash
pixi run build/release/extension/sirius/test/cpp/sirius_unittest '[tiny_domain]'
```

On the same candidate binary, feature OFF -> ON changed pooled Q1 from 0.7612875 to 0.6656095 s
(-12.57%) and left Q3 within noise (-0.35%). Replacing the older exact register path with the
general implementation was at parity on Q1 (0.6613630 vs 0.6616775 s) and improved Q3 by 0.70% in
that comparison. All sixteen timed validations passed.

Every generic-campaign Q1 result had SHA-256
`3f2acb60dca49625b697464856236e2897a68545bc9e376bab98a06b8e0ac7b4`; Q3 had
`f77cbfbd51ae75e765e02a528bcf33ec6bbb629ff2f4b5a2e16c6c2b44854bcf`.

Evidence and CUDA resource output:

- `/tmp/sirius-general-small-domain-20260829-artifacts/CONTRACT.md`
- `/tmp/sirius-general-small-domain-20260829-artifacts/abba-feature-toggle-cabf9676/RESULTS.md`
- `/tmp/sirius-general-small-domain-20260829-artifacts/tiny-domain-tests.log`
- `/tmp/sirius-general-small-domain-20260829-artifacts/cuobjdump-resource-usage.txt`

## Projection-to-aggregate fusion

### What the reverted prototype proved

**Status:** measured prototype `97b92dd1`, reverted by `415a8cd3`.

Q1 materialized two arithmetic projections before grouped aggregation. The prototype evaluated the
needed arithmetic inside the tiny-domain accumulator and removed eighteen projection ranges. In a
balanced same-binary toggle it changed Q1's mean from 666.063 to 601.562 ms (-64.501 ms, -9.68%)
and median from about 669.5 to 599.0 ms (-70.5 ms). Q3 moved by approximately +3 ms and remained a
hold.

The removed stages represented 82.087 ms of summed correlated GPU work and 80.987 ms of interval
union in that trace. Aggregate-kernel time itself stayed nearly flat, about 55.8 -> 55.5 ms. This is
the key mechanism evidence: simple arithmetic can be absorbed into an already memory-bound
accumulator at very low incremental cost.

The prototype is not acceptable because its planner recognized the exact expression tree, types,
raw six-column layout, and aggregate-state arrangement of TPC-H Q1, and depended on compressed
materialization being off. It must remain reverted. The baseline and fused profiles are:

- `/tmp/sirius-q1q3-nextsteps-20260828/30-implementation/q1-projection-accum/profiles/nsys-baseline/q1.nsys-rep`
- `/tmp/sirius-q1q3-nextsteps-20260828/30-implementation/q1-projection-accum/profiles/nsys-fused/q1.nsys-rep`
- `/tmp/sirius-q1q3-nextsteps-20260828/30-implementation/q1-projection-accum/RESULTS.md`

An ordinary adjacent-projection composition experiment also failed: it regressed pooled Q1 by
20.839 ms because composing trees duplicated the common subexpression
`price * (1 - discount)`. Any replacement needs a DAG and common-subexpression elimination, not
blind expression-tree substitution.

### General reimplementation design

**Status:** proposed and the highest-value next Q1 implementation.

The selector must be structural and operator-general:

1. Find a single-consumer chain `PROJECTION+ -> grouped aggregate`. Do not inspect relation names,
   bindings' semantic roles, query text, exact column count, or expression constants.
2. Resolve grouping and aggregate input references backward through every projection into a typed
   raw-input expression DAG.
3. Initially whitelist reference, constant, ADD, SUBTRACT, and MULTIPLY over proven fixed-width
   integral/decimal carriers. Preserve the AST's return type, precision, scale, rounding, null, and
   overflow semantics at every node.
4. Hash-cons the typed DAG so structurally identical subexpressions have one register value. Include
   operation, type metadata, constant value, and children in identity. Do not equate distinct source
   views merely because their values match.
5. Require grouping keys to be bare supported references for the first version. Feed the existing
   general physical-state descriptors and logical-to-physical mapping; never require Q1's state
   count or order.
6. In the per-row loop, evaluate each reachable DAG node once, carry validity alongside the value,
   apply the existing row predicate if the semantics are supported, and update the register states.
   `COUNT(*)` advances for every accepted row; AVG uses SUM plus `COUNT_VALID`.
7. Put fixed limits on nodes, live registers, groups, states, and occupancy. Decline before execution
   when static limits fail.
8. Preserve the original projection and cuDF aggregate chain as the fallback plan. Runtime
   representation/key checks must finish before any externally visible output is published.
9. Treat OOM, CUDA, stream, and arithmetic failures as errors or exact retries according to the
   existing aggregate contract, not as a silent semantic fallback after partial work.
10. For compressed materialization, decline the fused path per batch when required carrier metadata
    is unavailable. Do not globally disable compressed materialization. A later version should
    consume and restore supported carriers explicitly.

Suggested code boundaries:

- add `planner/tiny_domain_input_fusion.*` to build and validate the structural typed DAG before
  `wrap_hash_group_by`;
- add a POD `tiny_domain_expression_program` shared by the physical operator and CUDA dispatch;
- let `sirius_physical_grouped_aggregate` own both the optional program and the unchanged fallback
  chain;
- add a program-driven kernel path beside
  [`tiny_domain_grouped_aggregate_impl.cu`](../../src/cuda/tiny_domain_grouped_aggregate_impl.cu);
- keep scan-delivery/static-predicate fusion as a separate follow-up so it does not expand the first
  correctness surface.

Required tests must use renamed synthetic tables and multiple unrelated shapes: expression subsets
and reorderings, projection chains, decimals/integrals, nullable inputs, one/4/64/65 groups,
arbitrary aggregate order, CSE proving one computed node, sliced views, overflow/cancellation, and
unsupported operations preserving the original plan. Include randomized differential comparison to
DuckDB or the unfused Sirius path. Q3 must not activate and must not regress.

Promotion requires at least a replicated 40 ms Q1 win in balanced and holdout runs, all validation,
and Nsight evidence that the target projection ranges disappeared without equivalent work appearing
elsewhere. The current final profile prices 74.905 ms of projection interval, while the earlier
prototype measured a 70.5 ms end-to-end median gain. Applied mechanically to the 622.3 ms private-
fork result, that would project roughly 551.8 ms: still about 5.5% behind DuckDB. Fusion is the best
next step, but scan/decode improvement is likely required for Q1 parity.

## Pinned-host allocation reuse and cuCascade PR 185

**Status:** measured upstream-only prototype. The performance result is real evidence from a private
cuCascade fork, not a shippable Sirius change and not a measurement of PR 185.

### Original profile signal

The tuned scans still made short-lived explicit pinned allocations larger than cuCascade's 8 KiB
slab ceiling. In the prior optimized profile, one hot Q1 iteration made 36 `cudaFreeHost` calls and
one Q3 iteration made 28. The measured direct requests were 8,320 through 80,080 bytes with alignment
4 or 8. Allocation and synchronous free happened inside `GPU_SCAN` ranges.

The request histogram is preserved at:
`/tmp/sirius-q1q3-nextsteps-20260828/50-generalization/pinned-host/SIZE_INVENTORY.md`.

The local prototype eliminated `cudaHostAlloc`/`cudaFreeHost` from the entire candidate capture. In
a corrected A/B/B/A run it changed pooled Q1 0.657834 -> 0.628324 s (-4.49%) and Q3
0.692813 -> 0.666549 s (-3.79%). Do not use the quarantined
`balanced-INVALID-stale-control-*` directory; its control binary was stale. The valid root is:

`/tmp/sirius-q1q3-nextsteps-20260828/50-generalization/pinned-host/balanced-a11cada1-vs-415a8cd3`.

Candidate Nsight reports are under:

`/tmp/sirius-q1q3-nextsteps-20260828/50-generalization/pinned-host/nsys-candidate-a11cada1/tpch_20260829_184405_pinned_pool_candidate_nsys2_a11cada1`.

### Local prototype semantics

The prototype is cuCascade commits `eca1b37` and `9f767cb`, integrated by Sirius commits `69f215db`
and `6de3a005`. It changes:

- [`include/cucascade/memory/small_pinned_host_memory_resource.hpp`](../../cucascade/include/cucascade/memory/small_pinned_host_memory_resource.hpp)
- [`src/memory/small_pinned_host_memory_resource.cpp`](../../cucascade/src/memory/small_pinned_host_memory_resource.cpp)
- [`test/memory/test_small_pinned_host_memory_resource.cpp`](../../cucascade/test/memory/test_small_pinned_host_memory_resource.cpp)
- [`src/sirius_context.cpp`](../../src/sirius_context.cpp) for the outer resource installation/logging.

Its policy is general but not necessarily the desired final capacity model:

- power-of-two fixed-resource slab classes from 512 B through 128 KiB;
- separate `CUDF_PINNED_THRESHOLD = 8 KiB` and `MAX_POOLED_SIZE = 128 KiB`, so raising explicit
  request retention does not make ordinary cuDF allocations pinned above the established threshold;
- eligibility from size, alignment, upstream block size/capacity, device, and stream/event state;
- one resource per NUMA node through Sirius's existing topology wiring;
- deallocation records an event on the supplied stream and puts the slab in a pending list;
- allocation returns only fresh storage or a slab whose event is already complete according to
  `cudaEventQuery`;
- on bounded upstream OOM, synchronize one pending same-class slab and reuse it;
- event pools are per device and the event device is derived from the supplied stream, preserving
  cuCascade issue/PR #189;
- over-aligned requests bypass the pool and use the direct `Portable | Mapped` path;
- metadata is reserved before backing storage is published, making expansion exception-safe;
- unexpected `cudaEventQuery` errors are quarantined and propagated by `9f767cb`;
- destruction waits for pending use before returning upstream-owned blocks.

The deterministic overwrite regression in the prototype's cuCascade test near line 392 at
`9f767cb` gates an H2D read, deallocates its host pointer, allocates on the same or a different
stream, and immediately writes from the CPU. It proves that pending storage cannot be returned
merely because a wait was enqueued on the new stream.

The local implementation also has important limitations:

- upstream blocks remain assigned to one class until destruction, so an idle block cannot satisfy a
  different class and can cause cross-class false OOM;
- capacity pressure introduced two Q1 main-thread synchronizations totaling 14.214 ms and three
  extra Q3 synchronizations in the captured second iteration;
- expansion catches `rmm::out_of_memory`, but a physical `rmm::bad_alloc` can still fail without
  reclaiming a pending safe slab;
- 128 KiB was a useful experimental ceiling but needs a general allocator/resource justification;
  it cannot become policy merely because Q1/Q3 requests fit below it.

### Exact collision with PR 185

PR 185 was inspected at head `5df1ea0` on 2026-08-29. Its functional commits are:

- `857ac91` — cache large pinned allocations;
- `de27f15` — make the cache stream-safe;
- `5df1ea0` — revert unrelated allocator documentation.

It branches from `8c45d33`. The local prototype is based on later `1b0e7b6`, which contains the
per-device pinned-slab event work from #189 (and follows #184/#190 compatibility work). Therefore a
wholesale cherry-pick or merge of `5df1ea0` would regress or conflict with later allocator behavior.

Both stacks modify the same header, source, and test files; class state; constructor/destructor;
allocation/deallocation; event pooling; cache/slab expansion; and teardown. They are alternative
implementations of one ownership boundary, not independent optimizations that can be stacked.

| Property | PR 185 at `5df1ea0` | Local `eca1b37`/`9f767cb` |
|---|---|---|
| Small slab maximum | 8 KiB | Classes extend through effective 128 KiB/block-size ceiling |
| Larger request minimum | 16 KiB direct-cache bucket | Next power-of-two fixed-resource class |
| Retention budget | 256 MiB default idle direct-cache cap, constructor-tunable | Bounded by fixed host resource; no cross-class eviction |
| Backing allocation | Direct `cudaHostAlloc` above 8 KiB | Existing NUMA-selected fixed host-tier blocks through ceiling |
| Eviction | Oldest idle entry across large buckets; purge/retry | None until context destruction |
| Accounting | Idle cache cap; live direct allocations outside the cap | Every pooled block consumes configured host-tier capacity |
| Overlapping ownership | Requests >8 KiB through 128 KiB round into 16--128 KiB buckets | The same requests round into 16--128 KiB classes |
| Event base | Predates #189 | Per-device events based on supplied stream |
| Reuse readiness | Enqueue `cudaStreamWaitEvent`, then return pointer | Return only event-complete pointer; sync only under capacity pressure |
| Alignment | Cache selection ignores requested alignment | Unsupported/over-aligned requests use correct direct path |

The direct-cache design has useful properties: bounded idle retention, cross-bucket eviction, no
permanent upstream class fragmentation, FIFO eviction, and purge-before-retry. Its tradeoffs are that
live allocations are not covered by the idle limit, each per-NUMA allocator can multiply the default
allowance, direct CUDA allocations bypass Sirius's fixed host-tier accounting, and direct allocation
does not inherit the chosen NUMA upstream placement.

The PR 185 implementation centers on `large_cache_entry`, `try_take_cached_large_locked`,
`evict_oldest_large_locked`, `purge_large_cache_locked`, and `sync_and_free_large_victim`. Preserve
those general accounting/eviction concepts when rebasing; replace their readiness and failure
semantics as described below.

PR 185's author reports that its own workload reduced allocation/free pairs from 264 to 21, reduced
the full 22-query hot total by 13.9%, and passed 22/22 validations. Those numbers are useful evidence
for the general cache direction, but they were not reproduced in this campaign, use a different
workload/protocol, and cannot be transferred to non-pinned SF50 Q1/Q3 or to a safety-reconciled
implementation.

### Safety changes required before using PR 185

Do not merge PR 185 at `5df1ea0` as-is. Resolve at least these issues upstream:

1. **Host-visible completion.** `cudaStreamWaitEvent` only orders later device work on the receiving
   stream. The allocation API returns a CPU-writable pointer immediately, so its caller can overwrite
   bytes while a prior asynchronous H2D still reads them. Query completion before returning a cache
   hit; allocate fresh storage while capacity exists, and synchronize a pending candidate only under
   declared pressure.
2. **Stream device and per-device events.** Rebase onto and preserve #189. Event creation/query must
   use the device associated with the supplied stream, not whichever CUDA device is current.
3. **Failure propagation.** Assertion-only wait handling is insufficient in release builds.
   Unexpected query/record/synchronization errors must propagate. Storage whose last use cannot be
   proven complete must be quarantined or deliberately retained, never republished or freed.
4. **Teardown.** Wait safely for every pending slab and direct-cache event before returning/freeing
   backing storage.
5. **Alignment.** Include supported alignment in eligibility/bucket identity, or bypass to an
   alignment-correct direct allocation. Test through 4096-byte alignment.
6. **No-allocation deallocation.** Any `noexcept` deallocation path must pre-reserve metadata or
   catch map/deque/vector growth failure; it must not terminate on host metadata OOM.
7. **Accounting.** Expose both idle and live resident bytes. Define whether the configured hard
   budget covers idle only or total resident storage, and ensure multiple NUMA resources cannot
   silently multiply a global budget.

`9f767cb` changes an unexpected event-query result from ordinary “not ready” handling to a CUDA
exception. That branch had compile/audit coverage but no injected event-query failure because there
was no seam; add a CUDA-event shim or fault-injection seam upstream.

### Recommended upstream design

Use PR 185's bounded, power-of-two, evictable cache as the capacity-policy basis, rebased onto a
current cuCascade base, and port the local prototype's completion, device, alignment, exception,
quarantine, and teardown semantics. Keep ordinary upstream slabs for small allocations. Make the
large-cache budget a configuration/resource value derived from general host-memory policy, not a
TPC-H-observed size such as 128 KiB.

An alternative hybrid can use fixed-resource classes through a generally justified threshold and a
direct cache above it, but every byte range must have exactly one owner. Never let the local
16--128 KiB pool and PR 185's 16--128 KiB direct buckets both cache the same requests. If a hybrid is
chosen, apply completion gating and unified accounting to both sides.

Required metrics are request size/alignment, hit/miss/bypass, live/idle/pending bytes, peak bytes,
evictions, wait-reuse count/time, allocation/free calls, NUMA/device, and failure cause. Use those
metrics across multiple workloads to select a default budget. Do not add a fixed Q1/Q3-derived
reserve.

cuCascade owns cache policy, budget/eviction, provenance/alignment, events, reuse safety, failures,
teardown, metrics, and unit/fault-injection tests. Sirius owns NUMA/topology dispatch, installing and
restoring cuDF's process-global pinned resource, passing a general budget if exposed, and logging the
effective policy/metrics. See [Memory Management](memory-management.md) and cuCascade's
[memory management](../../cucascade/docs/memory-management.md) and
[topology/configuration](../../cucascade/docs/topology-and-configuration.md) documents.

### Upstream implementation and validation sequence

1. Rebase/reimplement PR 185 on a current cuCascade base containing #184, #190, and #189.
2. Retain its bucket arithmetic, cache cap, FIFO eviction, introspection, and purge/retry.
3. Replace both slab and direct-cache hit paths with completed-event gating. Use fresh allocation
   while permitted and a declared synchronization policy under pressure.
4. Port stream-device event pools, unexpected-error propagation, alignment handling, exception-safe
   metadata publication, and safe teardown from `eca1b37`/`9f767cb`.
5. Add telemetry and resolve the global/per-NUMA and idle/live accounting contract.
6. From a standalone/upstream cuCascade checkout, build and run the focused and complete suites plus
   racecheck/synccheck:

```bash
pixi run build
pixi run ./build/release/test/cucascade_tests '[small_pinned]'
pixi run ./build/release/test/cucascade_tests
pixi run compute-sanitizer --tool racecheck --error-exitcode 99 \
  ./build/release/test/cucascade_tests '[small_pinned]'
pixi run compute-sanitizer --tool synccheck --error-exitcode 99 \
  ./build/release/test/cucascade_tests '[small_pinned]'
```

Retain both stacks' general tests: bucket sizing/accounting/cap/eviction/bypass/purge; size-class
boundaries; same- and cross-stream gated H2D reuse; concurrency; capacity pressure; active OOM;
overalignment; and pending teardown. Add two-GPU current-device/stream-device mismatch; event query,
record, and synchronization fault injection; metadata-allocation failure; mixed-bucket churn; and a
proof that failed synchronization never republishes or frees storage.

The local report states 18 cases/504 assertions passed twice and racecheck/synccheck found no errors,
but its raw test/sanitizer logs were not retained. Re-run and preserve them rather than treating that
sentence as reproducible test evidence.

7. Integrate a reachable upstream/published commit into Sirius, update the minimal API/log wording,
   and verify a fresh `git submodule update --init --recursive`.
8. Rebuild Sirius and run context/config tests:

```bash
pixi run make
pixi run build/release/extension/sirius/test/cpp/sirius_unittest '[sirius][context]'
pixi run build/release/extension/sirius/test/cpp/sirius_unittest '[sirius][config]'
```

9. Compare clean base (A), local `9f767cb` reference (B), and reconciled upstream implementation (C)
   in balanced A/B/C/C/B/A order. Then repeat Nsight and Quent. Require eligible hot-window
   allocation/free churn to disappear without new critical synchronization.

Only after that experiment may the final Sirius gitlink advance. If the current branch is shared,
clean up `69f215db` and `6de3a005` with forward commits; if it is strictly unpublished, squash them
into the final upstream integration. The combined 0.622256/0.667125 result must be re-established:
it cannot be transferred to PR 185 by assumption.

## Remaining general opportunities

### Q1 priority order

#### 1. Generic projection-DAG/aggregate fusion

Implement the structural design above first. It has the strongest direct end-to-end evidence:
approximately 70.5 ms measured by the reverted prototype and 74.9 ms of projection interval in the
current trace. It also reduces intermediate allocation, transfer, and launch pressure for any
eligible projection/aggregate chain.

#### 2. Encoding- and type-driven scan aggregation

Even perfect projection fusion probably leaves Q1 behind DuckDB. Q1's final trace is dominated by
Parquet decode and Snappy decompression, and `l_extendedprice` alone represented about 58.5% of its
compressed input bytes in the original analysis. All 300 ship-date row groups cross the cutoff and
the dataset has no useful page indexes for that predicate, so row-group pruning or more prefetch
cannot erase most of the value-column work.

An intentionally easier counterfactual that removed essentially the current string-key/grouped
path saved 167.549 ms, yet that reduced Sirius query was still 106.363 ms slower than fresh DuckDB's
complete Q1. Aggregate-only tuning is therefore insufficient.

A general next stage should select from physical encoding/type metadata, not column names:

- consume dictionary/fixed-code small-domain keys without first expanding them to strings;
- restore logical key values at the output boundary, including row-group-local dictionary codes;
- fuse supported static predicates before group discovery;
- decode/convert fixed-width value carriers directly into the typed expression/aggregate program;
- preserve nullability, decimal scale/overflow, slices, and compressed-carrier restoration;
- decline safely on mixed encodings, unsupported pages, or resource limits.

The earlier estimate was 15--35 ms incremental for key-only dictionary handling and 60--110 ms for
value/scan integration, both low confidence until isolated kernel and end-to-end measurements exist.

Keep scan fusion as a second stage after the operator-level expression program. Add an explicit raw
delivery mode whose contract preserves Parquet row-group pruning and original row order, evaluates
the static predicate before discovering groups/updating states, and retains a complete unfused
delivery plan. Initially decline dynamic filters and compressed representations that cannot be
restored exactly. This is a general scan/operator interface change and should not be hidden inside a
Q1-specialized reader.

#### 3. Decode/decompression kernels

Profile-driven upstream candidates are more parallel page decode, encoding-specialized kernels,
larger/coalesced transfer units, and deeper decode/I/O overlap. These are likely cuDF/reader-level
changes. Require isolated kernel evidence and a matched end-to-end result; summed decode durations
are concurrent ceilings, not additive savings.

### Q3 priority order

The final private-fork Q3 median is only 8.06 ms faster than the frozen DuckDB median, comparable to
cross-block variability. Treat it as narrow parity on this protocol, not universal dominance.

#### 1. Dynamic-filter-before-late-payload materialization

The current dynamic Bloom filter is selective but the scan decodes payload columns before applying
it. A diagnostic Q3 variant replacing the late revenue payload moved Sirius from 716.181 to
483.808 ms, a 232.373 ms upper bound. Realization was conservatively estimated at 35--80 ms, centered
at 58.1 ms. The affected payload represented approximately 1.701 GB compressed, 64.9% of projected
`lineitem` input and 49.3% of total projected compressed input. The existing Bloom left roughly
1.1% of rows in the measured diagnostic. Page layout may reduce the gain because sparse survivors
can still touch most pages.

A general implementation should:

1. derive early columns (join/filter keys and static predicates) and late payload columns from
   expression lineage;
2. preserve existing row-group/page pruning;
3. wait for the already-ordered dynamic-filter publication point without introducing a scheduler
   cycle;
4. decode early columns, apply static and published dynamic filters, and construct a stable survivor
   mapping;
5. materialize payload pages/values only for survivors, then restore the original output order and
   schema;
6. fall back before publication when a filter is unavailable, selectivity is too high, page metadata
   cannot support savings, a representation is unsupported, or memory pressure invalidates the
   split plan;
7. instrument physical payload bytes/pages decoded, survivor fraction, filter-ready wait, and
   fallback reason.

Selection must depend on dependencies, filter semantics/readiness, observed or estimated
selectivity, page metadata, and resource limits. It must not recognize `lineitem`, revenue, Q3, or a
date constant.

##### Relationship to Sirius PR 1427

[Sirius PR 1427](https://github.com/sirius-db/sirius/pull/1427), “push dynamic filter below D2H
copy,” is directionally related but does not address this non-pinned Parquet bottleneck directly. At
the inspected WIP head `482127ec`, `scan_operator_input::prepare_for_processing` calls its
`try_serve_filtered_upload` only when `materialization_info` is an existing
`shared_ptr<data_batch>`. The helper then requires an already-decoded, plain HOST-tier representation
with fixed-width, null-free columns. It uploads the filter key, builds survivor indices, and gathers
only surviving values from mapped pinned blocks.

The ordinary non-pinned Parquet path carries `scan_info`; it takes the just-in-time file/prefetch
path and still performs Snappy decompression and Parquet decode. Therefore PR 1427 cannot remove the
decode/unsnap work that dominates this Q3 regime. Its initial commit reports a 14% win on an
uncompressed/pinned-style case, but that is neither this benchmark contract nor a measured result
from this campaign.

Reuse its general ideas—published mask-capable filters, stable survivor indices, a selectivity gate,
and gather/path telemetry—when designing late payload materialization. Move the decision to a
reader/decode boundary where payload pages can actually be skipped, preserve its conservative
fallback, and profile the explicit stream synchronization before adopting it. Merging PR 1427 alone
should not be counted as progress on the non-pinned Q3 gap.

#### 2. Opportunistic boundary prefetch

Repeated partial range boundaries suggested an existing cache promotion/prefill experiment, but the
buffered-I/O proxy saved only 9.604 ms and the directly hideable idle ceiling was about 15 ms. That is
below the 25 ms robust Q3 promotion gate. If revisited, keep it local-IO/general:

- promote a partially used one-MiB boundary after a verified second touch;
- use io_uring for opportunistic fill without delaying demand I/O;
- account physical bytes, cache hits, evictions, cold impact, and memory pressure;
- stop if it does not clear the predeclared gate.

Do not call the existing demand-populated retention cache proactive prefetching, and do not turn
buffered I/O into the default from this signal.

#### 3. Page-parallel decode

The Q3 customer decode grid suggested roughly 15--34 ms of possible end-to-end headroom, with a
33.8 ms model for an idealized 2x kernel speedup. Confidence is low and implementation likely belongs
upstream. Prove page-level parallelism and occupancy in a microbenchmark before changing the reader.

The remaining Q3 hash join/group/top-N phase is only about 20.5 ms in the earlier profile, and
dynamic-filter publication already precedes dependent probes. Rewriting that tail or scheduler order
cannot provide a robust 25 ms Q3 win.

## Experimental trail and rejected approaches

Rejected results are part of the handoff because they prevent repeating attractive but unproductive
work.

| Experiment | Result | Disposition/lesson |
|---|---:|---|
| Wide-private tiny aggregate | Q1 +79.35 ms | Too much per-thread state/resource pressure; reject. |
| Sampled-preflight exact register path | Q1 777.564 -> 668.004 ms (-109.560 ms, -14.1%) | Useful proof. Full preflight -> sampled saved 24.985 ms; preflight union 36.129 -> 0.297 ms and custom kernel 73.660 -> 37.749 ms. Superseded by the general `a907db12` implementation. |
| Exact Q1 projection/accumulator fusion | Q1 median about -70.5 ms | Strong headroom evidence, but query-shaped; reverted and must be structurally reimplemented. |
| Adjacent projection composition | Q1 +20.839 ms | Duplicated a shared expression; use a typed CSE DAG. |
| Decimal AST JIT/materialization | Q1 +13 to +40 ms | Correct, but cuDF interpreted decimal AST was unsupported and JIT was slower; do not retry as a standalone switch. |
| Post-decode CASE key encoding | Q1 +43.103 ms | Added work after paying string decode; only encoding-aware consumption can make this worthwhile. |
| Selectively uncompressed Parquet columns | Sirius Q1 -9.312 ms, Q3 +8.974 ms | Combined figure of merit approximately neutral and layout changes also move DuckDB; reject a Sirius-only codec policy. |
| Buffered local I/O | Q3 about -9.604 ms, Q1 +25.691 ms | Regressed combined/cold behavior; retain O_DIRECT. |
| 5 GiB scan task | Q3 -0.128 ms, Q1 +89.351 ms | No Q3 mechanism and large Q1 regression. |
| More reactors/chunks/threads | No replicated win | Existing one-reactor/one-chunk, 8/4-thread point was best on this system. |
| Larger cache ranges, higher eviction thresholds | No replicated win | One-MiB retention with the measured budget was sufficient; avoid extra footprint. |
| Generic CUDA graphs/memcpy batching | Only a few ms of idle-union headroom | Launch/API inclusive sums overlap active work and overstate end-to-end opportunity. |
| Q1 merge/order/result-tail tuning | About 4.2 ms total tail | Not material to the remaining gap. |

Detailed rejected-result files:

- `/tmp/sirius-q1q3-nextsteps-20260828/30-implementation/q1-tiny-domain/RESULTS.md`
- `/tmp/sirius-q1q3-nextsteps-20260828/30-implementation/q1-tiny-domain/REGISTER_PRIVATE_RESULTS.md`
- `/tmp/sirius-q1q3-nextsteps-20260828/30-implementation/q1-decimal-ast/RESULTS.md`
- `/tmp/sirius-q1q3-nextsteps-20260828/30-implementation/q1-projection-compose/RESULTS.md`
- `/tmp/sirius-q1q3-nextsteps-20260828/30-implementation/q1-layout/RESULTS.md`
- `/tmp/sirius-q1q3-nextsteps-20260828/20-proposals/PROPOSALS.md`

## Verification and promotion gates

For Sirius-owned aggregate/fusion work:

```bash
pixi run make
pixi run build/release/extension/sirius/test/cpp/sirius_unittest '[tiny_domain]'
```

Also run focused projection/expression, grouped-aggregate, plan-shape, context, and configuration
slices touched by a candidate. Run `git diff --check` and the repository's formatting checks on all
changed C++/CUDA files. GPU tests, sanitizer runs, profiles, and benchmarks must hold
`/tmp/sirius-perf-gpu.lock`; performance verdict runs should also hold the thermal lock.

Every general optimization must satisfy:

- no query number/text, TPC-H, table/column name, semantic role, date, or exact expression/state
  fingerprint in production selectors;
- activation on multiple unrelated synthetic shapes;
- exact output schema/order/type/scale/null semantics;
- arbitrary slices and alias/non-alias distinctions;
- documented overflow behavior;
- no partial publication before a runtime decline;
- preserved stream/resource ownership, compressed/late-materialization provenance, and
  retry/downgrade behavior;
- Q1 and Q3 validation against fresh DuckDB;
- matched, unprofiled balanced timing and a separate holdout;
- Quent plan/path proof and Nsight mechanism proof.

The final source build reported a full 549-target pass, `[tiny_domain]` 16 cases/718 assertions, and
all eight A/B/B/A query validations. The final profiled integration commit `9be8d29c` and final
`dev` commit `6de3a005` had identical Git tree
`ce3e0f916760bbd5acebeb4b1e63338f2e02eabf`. That tree includes the private cuCascade gitlink, so
tree identity does not resolve the upstream ownership problem.

## Artifact inventory

All paths are local and ephemeral.

### Configuration campaign

Root: `/tmp/sirius-q1q3-sf50-prefetch-20260828T141828Z`

- `CONTRACT.md` — scope and acceptance contract.
- `REPORT.md` — complete config sweep, results, validation, and initial divergence analysis.
- `configs/` — every tested YAML, including the winning
  `prefetch_1m_scan1536m_build512m.yaml`.
- `baseline/tpch_20260828_142246_q1q3_sf50_cpu_b1` — frozen DuckDB reference.
- `profiles/nsys/tpch_20260828_145703_nsys_stock/sirius/{q1,q3}` — stock Sirius reports/SQLite.
- `profiles/nsys/tpch_20260828_145733_nsys_final/sirius/{q1,q3}` — config-finalist
  reports/SQLite.

The initial campaign recorded 91 successful validation outcomes and no fallback/OOM.

### Optimization campaign and isolated prototypes

Root: `/tmp/sirius-q1q3-nextsteps-20260828`

- `CAMPAIGN.md` — frozen baselines, noise, methodology, and campaign ledger.
- `20-proposals/PROPOSALS.md` — ranked proposal synthesis and measured ceilings.
- `30-implementation/q1-groupby-state-dedup/{CONTRACT.md,RESULTS.md}` — accepted state dedup.
- `30-implementation/q1-groupby-state-dedup/profiles/.../sirius/q1` — isolated dedup Nsight.
- `30-implementation/q1-tiny-domain/` — exact-path prototypes, reports, profiles.
- `30-implementation/q1-projection-accum/` — reverted fusion contract, result, and A/B profiles.
- `30-implementation/q1-projection-compose/` — rejected composition.
- `30-implementation/q1-decimal-ast/` — rejected decimal JIT/materialization.
- `30-implementation/q1-layout/` — rejected layout experiment.
- `40-integration/q1-integrated/RESULTS.md` — intermediate integrated result.
- `50-generalization/projection-groupby/CONTRACT.md` — general fusion acceptance contract.
- `50-generalization/pinned-host/{CONTRACT.md,RESULTS.md,SIZE_INVENTORY.md}` — allocator evidence.
- `50-generalization/pinned-host/nsys-candidate-a11cada1/.../sirius/{q1,q3}` — allocator profile.
- `50-generalization/pinned-host/balanced-a11cada1-vs-415a8cd3` — valid A/B/B/A.
- `50-generalization/pinned-host/balanced-INVALID-stale-control-*` — quarantined; never use.

### Generalized/final integration

Root: `/tmp/sirius-general-small-domain-20260829-artifacts`

- `CONTRACT.md` — general small-domain design contract.
- `abba-feature-toggle-cabf9676/RESULTS.md` — generic aggregate measurements.
- `abba-feature-toggle-cabf9676/configs/{tiny_off.yaml,tiny_on.yaml}` — exact toggles.
- `tiny-domain-tests.log` — final focused Sirius aggregate test log.
- `cuobjdump-resource-usage.txt` — kernel resource evidence.
- `FINAL_REPORT.md` — pre-ownership-correction synthesis. Its allocator “accepted” wording is
  superseded by this document.
- `run_abba_combined_9be8d29c.sh` — exact final A/B/B/A driver.
- `combined-9be8d29c-vs-415a8cd3` — raw final timings/validation.
- `profiles/nsys/tpch_20260829_192420_combined_general_9be8d29c_nsys` — final Q1/Q3 reports,
  SQLite, logs, timings, and metadata.
- `profiles/quent/data` — Postcard telemetry.
- `profiles/quent/run/tpch_20260829_192525_combined_general_9be8d29c_quent` — validated harness
  result/logs.
- `profiles/quent/tiny_on_quent.yaml` — telemetry-enabled final config.

The final rebuilt hashes recorded by the campaign were:

- DuckDB: `df7115a267f55ea5a952ba60c42ffb62a08d09aeb02b86c0d42f8397d3e1ef64`
- Sirius extension: `dc7ddb027511d26aa311550a6d7d55d173008b8d517f986e6f85d0b982dd8a1a`

### Reproduction caveats

- `--pin none` disables table pinning. It does not disable the pinned-host compressed-range cache or
  explicit pinned staging allocations.
- Grouped Q1 then Q3 lets Q1 warm some `lineitem` state. Standalone Q3 was about 3% different in the
  config-era measurements; preserve query order.
- Iteration zero is Sirius-cache cold with uncontrolled OS cache, not guaranteed storage-cold.
- Quent and Nsight perturb timing. Use them only to verify paths and move attribution.
- Concurrent summed kernel/API durations are not wall time; use interval union.
- The final Q3 lead is small relative to cross-block variation.
- The allocator result is for local `9f767cb`, not PR 185 or a reconciled implementation.
- Raw cuCascade unit/sanitizer logs and event-query injected-fault evidence were not preserved and
  must be regenerated.
- `/tmp` artifacts, local worktrees, and analyzer processes will disappear. Archive them with
  checksums if this investigation needs to survive the host.

## Recommended implementation order

1. Preserve the raw artifacts.
2. Before changing the gitlink, preserve private cuCascade range `1b0e7b6..9f767cb` as a Git bundle
   or `git format-patch` series outside the submodule worktree. Otherwise the two prototype commits
   may become unreachable and later garbage-collected.
3. Recreate/squash the final general aggregate tree so the review history contains no query-shaped
   production implementation; retain `09716f59` and the final generalized `a907db12` behavior.
4. Reconcile the allocator with PR 185 entirely in upstream cuCascade, then benchmark a published
   commit before updating Sirius's gitlink.
5. Implement generic typed projection-DAG/aggregate fusion and require a replicated >=40 ms Q1 win.
6. Add encoding/type-driven scan integration for Q1 if fusion alone cannot reach parity.
7. For further Q3 headroom, prioritize dependency-driven dynamic-filter-before-payload decode.
8. Profile every promoted tree again with both Quent and Nsight, but make the final decision from
   unprofiled balanced/holdout runs.

Related architecture references:
[Scan](scan.md), [Operators](operators.md),
[Physical Plan Generation](physical-plan-generation.md),
[Expression Evaluator](expression-executor.md),
[Compressed Materialization](compressed-materialization.md),
[Optimizations](optimizations.md), and [Quent Telemetry](quent-telemetry.md).
