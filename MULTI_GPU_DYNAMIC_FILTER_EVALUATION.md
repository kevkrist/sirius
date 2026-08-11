# Multi-GPU evaluation of multi-batch dynamic Bloom filters

## Purpose

This runbook compares two optional Bloom-filter designs for a non-broadcast hash join whose build
arrives in multiple batches:

- **Global:** every exact pre-scatter build batch contributes to one global Bloom per key. The
  partials are OR-reduced, strictly replicated to every configured replica space, and consumed
  through the ordinary scan or join-edge dynamic-filter channel.
- **Partition-local:** every post-exchange build fragment contributes to the Bloom for its exact
  hash partition. The matching probe partition applies that Bloom at `CONCAT`; there is no global
  reduction or replica fan-out.

The primary question is whether avoiding global reduction and replication offsets the fact that the
partition-local filter runs after scan, hash partitioning, exchange, and CONCAT input reception. A
secondary question is whether the local bank is useful when a replicated global Bloom exceeds the
per-GPU policy cap but the distributed local bank fits.

The single-GPU SF1000 result in `PARTITION_DYNAMIC_FILTER_PROTOTYPE.md` favors the global design:
global was neutral on Q8 and 19.8% faster than the one-shot-only control on Q10; partition-local was
within noise of the control on both. Physical multi-GPU behavior remains unvalidated.

## Pinned revisions

Two branches on `kevkrist/sirius` define the experiment:

| Purpose | Branch | Required implementation checkpoint |
|---|---|---|
| Global-only reference | `codex/pr1277-multi-partition-dynamic-filter` | `13e0eb4078451f4b9a666e0bcf4a1d90dfb805f8` |
| Comparison superset | `codex/partition-specific-dynamic-filter-prototype` | `85c2dffec6f027437bb91bb054ef7d0aa1118f3d` |

The comparison branch contains both implementations and all fairness corrections. Its eventual tip
also contains this document; the implementation checkpoint above must be its ancestor. The global
checkpoint must be an ancestor of the comparison checkpoint.

Use **one binary built from the comparison branch for all four primary modes**. Switching binaries
between global and local would confound the design comparison with source drift. Build the
global-only checkpoint only for the cross-branch sanity check described below.

```bash
set -euo pipefail
git clone git@github.com:kevkrist/sirius.git sirius-df-mgpu
git -C sirius-df-mgpu fetch origin

GLOBAL_SHA=13e0eb4078451f4b9a666e0bcf4a1d90dfb805f8
LOCAL_CODE_SHA=85c2dffec6f027437bb91bb054ef7d0aa1118f3d

git -C sirius-df-mgpu rev-parse --verify "$GLOBAL_SHA^{commit}"
git -C sirius-df-mgpu rev-parse --verify "$LOCAL_CODE_SHA^{commit}"

git -C sirius-df-mgpu merge-base --is-ancestor "$GLOBAL_SHA" "$LOCAL_CODE_SHA"
git -C sirius-df-mgpu merge-base --is-ancestor \
  "$LOCAL_CODE_SHA" origin/codex/partition-specific-dynamic-filter-prototype

git -C sirius-df-mgpu worktree add --detach ../sirius-df-global "$GLOBAL_SHA"
git -C sirius-df-mgpu worktree add --detach ../sirius-df-comparison "$LOCAL_CODE_SHA"

git -C sirius-df-global submodule update --init --recursive
git -C sirius-df-comparison submodule update --init --recursive
test -z "$(git -C sirius-df-global status --porcelain=v1)"
test -z "$(git -C sirius-df-comparison status --porcelain=v1)"

(
  cd sirius-df-comparison
  pixi run make
)
(
  cd sirius-df-global
  pixi run make
)
test -z "$(git -C sirius-df-global status --porcelain=v1)"
test -z "$(git -C sirius-df-comparison status --porcelain=v1)"
```

Record the actual branch-tip SHA, both checkpoint tests above, and
`git submodule status --recursive`. Do not benchmark an uncommitted source tree.

Run the inventory commands below from the directory containing both worktrees. The preflight block
then enters `sirius-df-comparison`; subsequent repository commands assume that working directory
unless a section explicitly switches to the global-only worktree.

## Machine and dataset prerequisites

Use at least two CUDA GPUs. Prefer identical GPUs with enough host-pinned capacity for SF1000.
SF1000 is the reproduction dataset, but a larger scale or production dataset can be a second
configuration family. Do not regenerate or rewrite data between modes.

Reserve the machine exclusively. No other CUDA process, MIG reconfiguration, profiler, telemetry
collector, or memory-heavy unit test may overlap a timed block. Let temperature and clocks settle
before each block. Record whether persistence mode, application clocks, and power limits were fixed;
do not change them between modes.

Capture at least:

```bash
date -u
uname -a
lscpu
numactl -H
nvidia-smi -L
nvidia-smi topo -m
nvidia-smi --query-gpu=index,name,uuid,pci.bus_id,driver_version,memory.total \
  --format=csv
(cd sirius-df-comparison && pixi run nvcc --version)
pixi --version
git -C sirius-df-comparison rev-parse HEAD
git -C sirius-df-comparison status --short
git -C sirius-df-comparison submodule status --recursive
```

When supported, also capture `nvidia-smi topo -p2p r`, `nvidia-smi topo -p2p w`, and
`nvidia-smi nvlink --status`. Record CPU NUMA placement, the dataset mount and storage type, TPC-H
scale factor, table row counts, and a sorted relative-path/file-size manifest. Keep the exact order
of `CUDA_VISIBLE_DEVICES`: the global path reduces on the first configured replica space (normally
logical GPU 0), so reversing the visible order changes which physical GPU is the root and is a
useful sensitivity test.

Use `CUDA_DEVICE_ORDER=PCI_BUS_ID`. The preferred selection method is a fixed
`CUDA_VISIBLE_DEVICES` list plus `topology.num_gpus` equal to the length of that list. Do not also set
`topology.gpu_ids`. Alternatively, leave all GPUs visible and use `gpu_ids`; never combine the two
selection schemes without accounting for CUDA's device-ID remapping.

Every `performance_test.py` process unconditionally drops the OS page cache once through
`sudo -n /usr/bin/tee /proc/sys/vm/drop_caches`. On the exclusive benchmark machine, verify that
this exact command is permitted without a password before starting:

```bash
printf '3\n' | sudo -n /usr/bin/tee /proc/sys/vm/drop_caches
```

If it fails, configure the narrow sudoers rule documented in `test/tpch_performance/CLAUDE.md` or
stop; do not benchmark modes with different cache-drop behavior.

## Preflight tests

From the comparison worktree, expose at least two GPUs and run:

```bash
cd sirius-df-comparison

CUDA_DEVICE_ORDER=PCI_BUS_ID CUDA_VISIBLE_DEVICES=0,1 \
  pixi run build/release/extension/sirius/test/cpp/sirius_unittest \
  "[dynamic_filter][mgpu]"

CUDA_DEVICE_ORDER=PCI_BUS_ID CUDA_VISIBLE_DEVICES=0,1 \
  pixi run build/release/extension/sirius/test/cpp/sirius_unittest \
  "[dynamic_filter][publisher][accumulator]"

CUDA_DEVICE_ORDER=PCI_BUS_ID CUDA_VISIBLE_DEVICES=0,1 \
  pixi run build/release/extension/sirius/test/cpp/sirius_unittest \
  "[dynamic_filter][publisher][bloom_reduction]"

CUDA_DEVICE_ORDER=PCI_BUS_ID CUDA_VISIBLE_DEVICES=0,1 \
  pixi run build/release/extension/sirius/test/cpp/sirius_unittest \
  "[dynamic_filter][partition_dynamic_filter_bank]"
```

On a machine with at least three visible GPUs, also run
`[dynamic_filter][mgpu][replica][peer_overlap]` and confirm it used three devices rather than
skipping. Save the complete test output. A failure, hang, illegal address, or skipped two-GPU
replica test blocks performance work.

## Freeze one base configuration

Start from a known-good config for the machine. Multi-GPU Parquet must use:

```yaml
sirius:
  topology:
    num_gpus: 2                 # match the visible GPU count for this topology
  executor:
    scan_manager:
      use_sirius_datasource: true
  operator_params:
    scan_task_batch_size: 1073741824
    hash_partition_bytes: 1073741824
    concat_batch_bytes: 1073741824
    max_build_hash_table_bytes: 943718400
    max_broadcast_join_size: 268435456
    enable_dynamic_filter: true
    enable_dynamic_filter_multi_partition: false
    enable_dynamic_filter_partition_specific: false
    max_dynamic_filter_bloom_bytes_per_gpu: 268435456
    enable_dynamic_zone_map_filter: false
    dynamic_filter_domain_coverage_threshold: 0.9
    dynamic_filter_keep_threshold: 0.9
  telemetry:
    enable_quent: false
```

The 1 GiB batch sizes and 900 MiB build-table threshold reproduce the prior SF1000 experiment.
Merge this fragment into the machine's full memory, host, thread-pool, downgrade, and telemetry
configuration; do not discard known-good machine settings. `use_sirius_datasource` is required for
multi-GPU execution.

Create four copies that differ in only these three booleans:

| Mode | `enable_dynamic_filter` | `enable_dynamic_filter_multi_partition` | `enable_dynamic_filter_partition_specific` |
|---|---:|---:|---:|
| A: master off | false | false | false |
| B: one-shot only | true | false | false |
| C: global multi | true | true | false |
| D: partition local | true | false | true |

The two subordinate switches are mutually exclusive. Freeze everything else, including topology,
cap, partition/batch sizes, broadcast and build-table thresholds, memory fractions, thread counts,
pin tier, domain/keep thresholds, and unrelated optimizations. Disable Quent and other tracing for
timed runs. Save `diff -u` output proving that only the three switches differ, and save SHA-256
hashes for all configs.

For the optional global-only cross-branch check, make a compatibility copy of C with only
`enable_dynamic_filter_partition_specific` removed: the global checkpoint predates that key and
rejects it as unknown. Save this additional diff/hash and keep every common setting identical.

Use two configuration families if time permits:

1. **Reproduction/stress:** the 1 GiB/900 MiB settings above on SF1000. This should create enough
   build fragments to explain the mechanism.
2. **Production-natural:** the machine's ordinary tuned sizes. Do not force filter activation in
   this family; discovering whether it naturally occurs is part of the result.

Never change a knob for just one mode. Do not force `concat_all`. If a target join broadcasts or
uses one partition, adjust the shared base settings and repeat activation discovery for all modes.

## Topology matrix

At minimum evaluate N=1 as a control and N=2 on the best-connected pair. On larger systems add:

- N=2 on a cross-root or non-P2P pair, if the physical topology provides one;
- N=4 and N=all;
- the key N=2 pair with reversed `CUDA_VISIBLE_DEVICES` order.

Do not emulate missing P2P by changing production code. If the machine is fully connected, report
host-staged performance as unmeasured; the forced-host-staging unit test is correctness evidence,
not performance evidence.

## Discover qualifying queries before timing

GPU count changes partition selection and cap admission, so do not assume the single-GPU Q8/Q10
set remains complete. Run all 22 TPC-H queries once in global mode and once in local mode for every
topology/config family:

```bash
CUDA_DEVICE_ORDER=PCI_BUS_ID \
CUDA_VISIBLE_DEVICES="$GPUSET" \
SIRIUS_LOG_LEVEL=debug \
pixi run python test/tpch_performance/performance_test.py \
  --input "$TPCH" \
  --data-source parquet \
  --engine gpu \
  --iterations 1 \
  --mode grouped \
  --pin host \
  --queries 1-22 \
  --config "$GLOBAL_CONFIG" \
  --output "$OUT" \
  --name "activation_global_${FAMILY}_n${N}"
```

Repeat with the local config and a distinct name. If the working set cannot be pinned reliably,
use `--pin none` in every run and record that deviation.

A global activation requires both:

- `armed global dynamic Bloom for ... exact build batch(es)` with more than one batch; and
- `published ... global Bloom filter(s) ... after ... exact build contribution(s)` with at least
  one filter and more than one contribution.

A cap skip or an existing one-shot publication does not qualify. Correlate the accumulator arm and
publication with the matching query's debug
`sirius_physical_hash_join id ... partition strategy` record. That record, not the arm message,
must show more than one partition and no `[broadcast]`. Also record whether the planner placed the
global consumer at a scan or a join-edge endpoint; publication alone does not establish how much
pre-exchange work it can prune.

A partition-local activation requires:

- `[partition_dynamic_filter_bank] armed P partition(s)` with P > 1; and
- `sealed F filter(s) across B of P partition(s)` with F > 0.

`B` may be smaller than `P` when build partitions are empty. Instrument per-partition build rows and
require a usable filter for every nonempty build partition rather than literally every partition.

For decisive local results, per-query counter deltas are mandatory. The stats sink is cumulative for
the connection: call `SiriusContext::get_dynamic_filter_stats_snapshot()` immediately before and
after each query iteration, subtract every field, and serialize the delta with query/iteration ID.

Require the D delta to show:

- positive `partition_dynamic_filter_probe_batches`;
- `partition_dynamic_filter_probe_rows_out < partition_dynamic_filter_probe_rows_in`, because the
  probe-batch counter alone also moves when selection finds no usable filter;
- `partition_dynamic_filter_build_fragments > partition_dynamic_filter_partitions_built`, proving
  at least one filter unioned multiple fragments; ideally export fragment counts per partition;
- zero failures, device mismatches, and budget skips; and
- built filters for every instrumented nonempty build partition.

Armed/sealed logs prove construction, not successful application. The canonical Python harness does
not export these snapshots, so by itself it is screening evidence only and cannot support a final D
performance conclusion. Prefer a dedicated successful-selection/application counter if the other
harness can add one. Record readiness waits and skew; neither is inherently a failure.


The primary timed set is the intersection of queries for which C and D both activate under the
same physical settings. Retain a second **local-only-fit** set where C cap-skips but D seals and
applies; it tests the fallback hypothesis but must not be presented as a head-to-head filter timing.
Re-run activation discovery after every topology, cap, or batch-size change.

## Correctness gate

For every qualifying query, topology, and mode, first run one CPU/GPU validation pass:

```bash
CUDA_DEVICE_ORDER=PCI_BUS_ID \
CUDA_VISIBLE_DEVICES="$GPUSET" \
SIRIUS_LOG_LEVEL=info \
pixi run python test/tpch_performance/performance_test.py \
  --input "$TPCH" \
  --data-source parquet \
  --engine both \
  --validation \
  --iterations 1 \
  --mode grouped \
  --pin host \
  --queries "$QUALIFIED" \
  --config "$CONFIG" \
  --output "$OUT" \
  --name "correctness_${MODE}_${FAMILY}_n${N}"
```

In addition to the harness validation, byte-compare each mode's
`sirius/q<N>/result.txt` against A and save all SHA-256 hashes. Investigate any difference even if
the CPU tolerance check accepts it.

Inspect logs for Sirius execution errors, DuckDB fallback, illegal addresses, OOM/downgrade,
publication abort/failure, reservation denial, unknown or missing contributions, local
abandonment, device mismatch, and apply-failed-open warnings. Because the canonical runner does not
set `enable_duckdb_fallback=false`, also run a representative manual smoke query with fallback
disabled, or add that setting in an external driver. Never treat a silently CPU-fallback result as
GPU correctness evidence.

## Timed experiment

Use only the comparison worktree and one build. Run one process per mode with warning-level logging,
Quent/profilers disabled, and seven grouped iterations. Iteration 0 is warm-up and must be discarded.
A valid primary run should emit no warnings, so warning capture adds no mode-dependent message
volume while preserving fail-open diagnostics.

The decisive driver must additionally serialize per-iteration stats deltas and a low-overhead global
accumulator completion record containing its exact contribution count. Require C to publish from
more than one exact contribution in every measured iteration, and require every D iteration to meet
the application/multi-fragment delta gates above. The canonical command below is screening-only
unless the other harness adds those artifacts.

```bash
CUDA_DEVICE_ORDER=PCI_BUS_ID \
CUDA_VISIBLE_DEVICES="$GPUSET" \
SIRIUS_LOG_LEVEL=warn \
pixi run python test/tpch_performance/performance_test.py \
  --input "$TPCH" \
  --data-source parquet \
  --engine gpu \
  --iterations 7 \
  --mode grouped \
  --pin host \
  --queries "$QUALIFIED" \
  --config "$CONFIG" \
  --output "$OUT" \
  --name "${BLOCK}_${MODE}_${FAMILY}_n${N}"
```

For descriptive screening, use the four balanced sequences below, checking GPU idleness and
temperature before each block:

1. A, B, D, C
2. B, C, A, D
3. C, D, B, A
4. D, A, C, B

For inferential confidence intervals, repeat this four-sequence cycle twice (at least eight
independent blocks) and randomize the eight block orders with a recorded seed.

For each query/mode/block, discard iteration 0 and compute the median of iterations 1-6. Define
`R_CB = median(C) / median(B)`, `R_DB = median(D) / median(B)`, and
`R_DC = median(D) / median(C)`; a runtime ratio below 1 is faster. Percent change is
`100 * (R - 1)`.

Report raw samples, median, mean, sample standard deviation, min/max, and coefficient of variation.
For each query, bootstrap paired block ratios by resampling blocks; do not pool different queries as
independent replicates. For a workload aggregate, first sum query medians within each block and then
bootstrap those block-level ratios. A raw-ratio 95% interval must exclude 1 (a percent-change or
log-ratio interval must exclude 0). Four blocks are descriptive only; use at least eight for that
inferential claim.

Measure the noise floor from repeated B controls. Call a difference material only when its paired
95% interval excludes the correct null as above and its magnitude exceeds
`max(3%, 2 * control CV)`.

### Cross-branch sanity check

After the primary experiment, switch explicitly to the global worktree and run one representative
C-mode query with the global-compatible config described above:

```bash
cd ../sirius-df-global
# Run the same performance_test.py command with GLOBAL_COMPAT_CONFIG, then return:
cd ../sirius-df-comparison
```

Its result must match the comparison binary, and its timing should be within the measured control
noise. If not, stop and explain the branch/build drift; do not substitute cross-branch timings for
the one-binary comparison.

## Diagnostic runs, separate from timing

Run Nsight Systems on one representative qualifying query for B, C, and D on each important
topology:

```bash
CUDA_DEVICE_ORDER=PCI_BUS_ID \
CUDA_VISIBLE_DEVICES="$GPUSET" \
SIRIUS_LOG_LEVEL=info \
pixi run python test/tpch_performance/performance_test.py \
  --input "$TPCH" \
  --data-source parquet \
  --engine gpu \
  --iterations 2 \
  --mode nsys-profile \
  --query-timeout 600 \
  --pin host \
  --queries "$REPRESENTATIVE_QUERY" \
  --config "$CONFIG" \
  --output "$OUT" \
  --name "profile_${MODE}_${FAMILY}_n${N}"
```

Run Quent, DCGM, `nvidia-smi dmon`, or other sampling in additional diagnostic runs, never during
the primary timing. Use unique telemetry directories per mode and preserve profiler reports.

Collect or derive the following when observable:

| Global path | Partition-local path | Both paths |
|---|---|---|
| Expected/completed build IDs and rows | Partitions and build fragments per GPU | Scan rows/bytes |
| Configured replica count and scan/join-edge route | Active executors and partition-to-device map | Join strategy/partition count |
| Bloom keys, geometry, bytes | Rows, filters, and bytes per partition/device | Exchange rows/bytes/time |
| Source partial devices and reduction root | Partition skew: max/mean and CV | CONCAT input/output/time |
| Source-to-root bytes, route, and duration | Seal time and readiness waits | Hash-probe rows/time |
| OR-reduction duration | Probe rows in/out per partition | Time to first probe |
| Root-to-target replica bytes, route, duration | Mask/compaction time | HBM peak per GPU |
| Last-contribution-to-publication latency | Device mismatch/fail-open counts | Spill/downgrade/reservations |
| Cap and strict-replica outcome | Cap and selection outcome | Total query time |

Current logs expose global arm/publish/cap/abort, local arm/seal/skew, and some transfer facts. They
do not export the stats snapshot, and there are no distinct accumulator NVTX ranges for reduction
versus replication. Do not invent a breakdown: report the combined publication critical path or
mark fields unavailable. If the other harness can read the context snapshot, that is the preferred
way to expose local probe selectivity and failure counters.

## Cap and route sweeps

For each important topology/query, test:

- cap = 0: multi-Bloom disabled control;
- one byte below the required footprint: must skip and remain correct;
- the default 256 MiB;
- exactly the required representable footprint: equality must admit;
- one safe higher cap, such as 512 MiB or 1 GiB when VRAM permits.

One aligned Bloom is approximately two bytes per build row:

```text
B(rows) = align_up(32 * max(1, ceil(rows / 16)), 256)
global bytes/GPU = admitted_keys * B(global_build_rows)
local worst bytes/GPU = admitted_keys * B(ceil(global_build_rows / partitions))
                        * ceil(partitions / active_GPUs)
```

Use the exact logged/computed allocator-accounted value, not the approximation, for boundary tests.
This sweep should identify any band where the replicated global Bloom skips but distributed local
banks fit. Re-run activation and correctness after every cap change.

Exercise the machine's natural peer-DMA and host-staged routes, plus reversed visible-device order.
Lower memory/reservation limits only in a separate fail-open correctness stress, not in performance
results. A strict global replica failure must result in no global fan-out; a local failure must pass
the affected probe safely. Any false negative, hang, illegal access, or silent fallback fails the
prototype.

## Invalid-run criteria

The following rejection rules apply to primary A/B/C/D performance runs. Cap-boundary and induced
failure diagnostics have explicit expected skips/fail-open outcomes; retain them when behavior
matches that expectation and results remain correct, and reject them when it does not.

Discard a primary run if any of the following occurs:

- another GPU process or test overlaps it;
- source, submodule, dataset, config, device ordering, pin tier, or clocks differ unexpectedly;
- C does not publish a global Bloom from more than one exact contribution in every measured
  iteration;
- D does not meet the per-iteration application, multi-fragment, nonempty-partition, and zero-failure
  delta gates above;
- the join broadcasts, uses one partition, cap-skips, or follows only the ordinary one-shot path;
- result validation fails or any mode silently falls back to CPU;
- a publication abort, local abandonment, device mismatch, illegal address, OOM, unexpected
  downgrade, or apply-failed-open warning occurs;
- profiling, verbose logging, or telemetry overlaps only some timed modes;
- the requested GPU pool/reservation was not actually established.

Keep invalid artifacts and the rejection reason; do not silently delete samples.

## Decision rules

Correctness is a hard gate: CPU and cross-mode parity, no device mismatch/illegal access/hang, the
intended filter active in every measured sample, no unreported fallback, and enough telemetry to
explain placement and memory.

Decide enablement before choosing between prototypes:

1. Global is eligible only when C materially beats B and its replica footprint/publication latency
   is acceptable.
2. Partition-local is eligible only when D materially beats B, the counters prove actual pruning,
   and readiness/CONCAT cost and all hard gates are acceptable. The same D/B improvement and pruning
   requirements apply in a local-only cap band where C skips; successful sealing alone is not value.
3. If neither is eligible, leave the multi-partition extension disabled for that workload.

If both are eligible, compare D/C on each topology. Prefer global when C materially wins or when D/C
is within noise: global is the simpler mechanism and can prune before exchange. Retain local only
when D materially and repeatably wins on a real topology, or when it is the eligible cap-band
fallback. Consider a topology/budget-adaptive policy only when telemetry explains the split, such as
global reduction/replication dominating on a staged route or the replicated global footprint
exceeding the cap while the distributed local bank fits.

If local filter selectivity is high but wall time is not, the likely follow-up is filtering received
probe fragments before CONCAT materialization, or fusing filtering into receive/merge, rather than
adding another post-CONCAT materialization.

## Required report bundle

Preserve:

- hardware/topology/NUMA inventory and visible-device order;
- exact source and submodule SHAs plus build command/output;
- dataset provenance, row counts, and file manifest;
- every YAML, its SHA-256, and the flag-only diffs;
- preflight results and activation evidence;
- raw `runtimes.csv`, logs, result files, CPU/cross-mode hashes, and invalid-run notes;
- Nsight/Quent/DCGM artifacts from separate diagnostic runs;
- analysis code and a summary table.

The summary table should include, per topology/config/query: B, C, and D medians; C/B, D/B, and D/C
with confidence intervals; noise threshold; consumer route class; configured replica count; filter
bytes; publication or seal latency; local probe keep ratio; exchange bytes; peak HBM per GPU;
transfer route; and cap/failure status. End with separate conclusions for the best-connected N=2
pair, all GPUs, a naturally staged route if one exists, and the cap-band fallback study.
