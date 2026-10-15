# TPC-H deferred device read ordering investigation

Target: the first-wait issuance order in `deferred_device_batch::await`, and its
interaction with `prefetching_cache::await_inflight_prefetch`. Compare repaired
siblings-first production code against the same code with own-first issuance.
No production policy/config/API change is requested or planned.

Base HEAD: `0ef5a30fbe8e6d9f87dae2350af3baa1e97a5a2c`, including the current
uncommitted repairs. Preserve source snapshots, dirty diff and binary hashes.
Hardware: one logical NVIDIA L40S (GPU 0), 46,068 MiB, driver 615.71.09.
Dataset: existing `/data/tpch_parquet_sf100`, original partition layout
(six lineitem files), approximately 26 GB compressed. Queries: repository
TPC-H Q1, Q6, Q14, Q19, unchanged SQL. Unpinned parquet, Sirius io_uring,
Sirius host cache with LRU. Test two separately reported configurations:
`default` leaves the local backend's readahead budget at its current default
of zero (disabled); `prefetch8` explicitly sets `max_readahead_scans: 8`
and `readahead_strategy: opportunistic`. This correction follows source
inspection before any query measurements. The opportunistic worker arms once
on non-scan work, empty task queue, or memory-wait/downgrade events.

Figure of merit: median end-to-end materialized query time in milliseconds,
reported separately per query and fresh/retained Sirius-cache state; sum of
the four query medians per state as secondary aggregate. First context-cold
execution is reported separately and excluded from steady-state medians.
Direct I/O means these are not OS-page-cache cold/hot labels.

Prespecified timing sequence for EACH configuration: S,O,O,S
(S=siblings first, O=own first).
Each block runs each query in its own process: one initial execution, then
three executions with `reset_sirius_cache()` before each, then three executions
retaining the previous cache. This gives 6 samples/policy/query/cache-state,
plus two initial samples/policy/query. Derive noise from baseline within-block
spread and between-block medians; compare paired block deltas. Do not add
timing repetitions based on significance or favorable results.

Diagnostics are separate from timing runs (one context-cold, three fresh and
three retained samples for every configuration/policy/query): direct counters for first await,
open siblings, A's pending-overlapping-prefetch predicate at first-wait entry,
actual prefetch waits and durations by own/sibling, and single-source groups.
Do not count every member future as a distinct first wait. Capture per-query
Quent semantic evidence and Nsight Systems CUDA/NVTX evidence where supported.
Instrumented or profiled elapsed time is never a performance verdict.

Path/correctness gates: disable DuckDB fallback, prove deferred flushes/open
sibling execution, compare all timing and diagnostic results against independent
DuckDB CPU execution of the same files/SQL. Compare decimal/integer/string
values exactly and floating values with explicit tight numeric tolerance.
Zero-sibling queries remain useful reachability evidence but cannot establish
an ordering win. No pin_table or modified dataset layout in the primary test.

Build: reuse existing full embedded DuckDB+Sirius link recipe, current core,
and benchmark-only generated order/instrumentation objects. Keep the production
tree untouched. Preserve all generated source diffs and exact compile/link
commands. Runner/build details and resolved configuration recorded as artifacts.

GPU work serialized under `/tmp/tpch-deferred-order-gpu.lock`; parent schedules
all GPU-consuming tests/profiles. Check no foreign GPU workloads before each
block; record utilization, clocks, temperature and command. Maximum four agents
including parent. Stop after the fixed matrix and independent review, or report
the concrete build/runtime blockers and completed evidence. Stress microbenchmark
from the prior task remains supporting evidence, not the figure of merit.

Manifest fields: run ID, HEAD/dirty diff, source and binary hashes, config hash,
dataset/files, query/SQL hash, iteration, cache regime, policy, instrumentation,
raw timings/results, exit status, logs, telemetry/profiler paths and hardware.

Premeasurement amendment: the original64Mi campaign configuration was calibrated in /tmp/tpch-deferred-order-20261015. It yielded approximately15–20s fresh scans and256ms retainedQ6 scans. Before any timing matrix, use documented1Mi host block size,128blocks/pool,8initial pools, retaining64Gi capacity.64Mi blocks leave one device-miss staging slot per reactor; this is a materially different I/O-concurrency regime, not evidence of an ordering win. Stop the original calibration after completedQ1/Q6; the deliberately interruptedQ14 is recorded failed. Four ABBA blocks bound runtime before seeing any paired timing deltas; only two independent blocks/policy, so small effects remain inconclusive. CPUoracles are reused byte-identically from the original run (GPU execution disabled).
