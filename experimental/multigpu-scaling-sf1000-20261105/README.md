# Multi-GPU scaling diagnosis: TPC-H SF1000, host-pinned, 4x RTX PRO 6000

Reference bundle from a profiling campaign run on 2026-11-05 against commit `bb557b1a` (main). It answers one
question: why does Sirius scale only 1.79x from 1 to 4 GPUs on this machine, and what would it take to do better?
Nothing here changes engine code. Start with this file, then [30-diagnosis/FINAL.md](30-diagnosis/FINAL.md).

## Summary

- Warm suite, 22 queries, hot mean of iterations 2-4: **1 GPU 35.82 s, 4 GPUs 20.04 s (1.79x)**. Ideal 4-GPU time is
  8.96 s, so the gap to linear is **11.08 s**.
- The GPUs are not idle at 4 GPUs: each one has a task in flight about 95% of the time. The loss is per-task
  inflation. The same work costs 1.9x the executor thread-time at 4 GPUs (132 s vs 252 s), while the GPUs draw about
  135 W of 600 W and use about 11% of memory bandwidth.
- Best measured configuration: `scan_task_batch_size: 4GB` gives 33.04 s on 1 GPU and 16.91 s on 4 GPUs (1.95x).
- At today's wire bytes, this hardware caps the 4-GPU suite near 12.2-12.4 s (about 2.9x).

## Ranked bottlenecks

Costs are seconds of 4-GPU suite wall time. "Verified" is the midpoint of two adversarial reviews per item (top 8);
"synth" is the synthesis estimate before review. About 2 s of the gap stayed unattributed after review.

| # | Bottleneck | Kind | Verified | Synth | Primary evidence |
|---|---|---|---|---|---|
| 1 | Two PCIe Gen5 x16 uplinks feed four GPUs (GPU0+1 behind switch A, GPU2+3 behind switch B) | hardware | 2.9 | 3.48 | [h2d microbench](10-analysis/microbench/h2d_concurrency/results.txt), [X3_link](10-analysis/X3_link.md), g2s vs g2x runs |
| 2 | Process-wide CUDA driver stream-ordered allocator lock; device allocations go straight to `cudaMallocFromPoolAsync`/`cudaFreeAsync` with no caching layer | mechanical | 2.0 | 1.80 | [alloc microbench](10-analysis/microbench/alloc_contention/results.txt), [X1_allocator](10-analysis/X1_allocator.md) |
| 3 | Cross-GPU clones of 627 x P partition fragments, serial per consumer task, host syncs per buffer | design | 1.2 | 1.72 | [X2_exchange](10-analysis/X2_exchange.md), [fam_groupby](10-analysis/fam_groupby.md) |
| 4 | Cross-switch shuffles host-staged (P2P unsupported across switches), two legs serialized per 1 MiB chunk | hardware + design | 0.9 | 1.23 | [p2p microbench](10-analysis/microbench/p2p/results.txt), X2_exchange |
| 5 | Next scan starts only after the previous exchange barrier; uplinks idle during exchanges | design | 0.7 | 1.20 | X3_link section on link-idle time |
| 6 | Exchange policy: P >= N partition floor, BUILD_PROBE min(natural, N), low broadcast/build caps | design + config | 0.7 | 0.76 | X2_exchange, `20-experiments/x_g*_joinpol` |
| 7 | Fixed per-query work outside execution (parse/bind/optimize, result return) | design | 0.34 | 0.34 | [verify notes](30-diagnosis/verify_fixed-per-query-overhead_measurement.md) |
| 8 | Granularity tails: P mod N on GPU0/1, scan-task quantization, single-GPU tails | config | 0.24 | 0.31 | verify_granularity-tails_*.md |
| 9 | Accumulated Bloom publication is all-or-nothing HOST staging | design | <= 0.3 | 0.30 | [bloom_peer_access](10-analysis/bloom_peer_access.md) |
| 10 | One small pinned-host pool for all GPUs (single NUMA node); `device_scalar` syncs wait on other GPUs' events | design | 0.2 | 0.20 | X1_allocator finding X1-4 |

Code locations behind each mechanism are cited file:line in the linked reports. The most-cited ones:
`cucascade/src/memory/reservation_aware_resource_adaptor.cpp:462-475` (allocation path),
`src/op/partition/gpu_partition_impl.cpp:212-221` (P x C deep-copy slices),
`src/op/partition_placement.hpp:59-71` (p % N placement), `src/op/partition/partition_sizing.hpp:85-120` (P >= N floor),
`cucascade/src/cudf/representation_converter_builtins.cpp:589-694` (staged clone legs),
`src/cuda/sirius_dynamic_bloom_filter.cu:1080` (Bloom transport choice),
`src/sirius_context.cpp:1056-1083` (shared small pinned pool).

## Measured configuration effects

| Setting | 1 GPU (s) | 4 GPUs (s) | Scaling |
|---|---|---|---|
| Baseline | 35.82 | 20.04 | 1.79x |
| `scan_task_batch_size: 4GB` | 33.04 | 16.91 | 1.95x |
| `scan_task_batch_size: 8GB` | - | 17.34 | - |
| 4GB batches + 8 GB build and broadcast caps | 31.93 | 16.92 | 1.89x |
| 8 GB build and broadcast caps only | 34.74 | 19.07 | 1.82x |
| `hash_partition_bytes: 4GB` | - | 19.91 | - |
| `hash_partition_bytes: 256MB` | - | 23.66 | - |
| `executor.pipeline.num_threads: 8` | 35.55 | 21.23 | 1.67x |
| Pin tier `gpu` (no host link; GPU memory pressure on Q3/Q10/Q18) | - | 18.26 | - |
| 2 GPUs `{0,1}` (same switch; what `num_gpus: 2` picks) | 35.82 | 29.40 | 1.22x |
| 2 GPUs `{0,2}` (across switches) | 35.82 | 22.21 | 1.61x |
| Quent telemetry off | - | 20.00 | - |

## Remedies, by expected 4-GPU gain

1. Config: `scan_task_batch_size: 4GB`, 4 pipeline threads per GPU, 2-GPU subsets across switches (`gpu_ids: [0, 2]`).
   Measured -3.1 s.
2. Per-GPU user-space sub-allocator between the cucascade reservation adaptor and `cuda_async` (seam:
   `reservation_manager_configurator::set_gpu_memory_resource_factory`), and no P x C deep-copy slices. About 2.3-3.7 s.
3. Exchange redesign: coalesce fragments before cloning, batched and event-ordered clone legs, cost-based broadcast,
   no P >= N floor when a local or broadcast plan exists. About 2-3 s.
4. Cross-pipeline scan lookahead so uploads continue through exchange phases. About 0.7-1 s.
5. Fewer wire bytes: compression plans for customer/part/partsupp/supplier and better `o_comment`/`l_shipmode`
   encodings (-233 GB, 19% of scan bytes). About 1.1 s.
6. Hierarchical Bloom publication (peer copies within each P2P island) and per-device small pinned pools.

## Hardware and regime

- 1 socket Xeon 8559C, 1 NUMA node, 96 cores, 999 GB RAM. 4x RTX PRO 6000 Blackwell Server Edition, 96 GB each.
- PCIe: GPU0+1 behind switch A (root port 44:00.0), GPU2+3 behind switch B (55:00.0), one Gen5 x16 uplink each.
  `nvidia-smi topo -p2p r` reports P2P OK only for 0<->1 and 2<->3; Sirius logs "Pool access unavailable" for the
  other 8 ordered pairs.
- Dataset: tpchgen-rs SF1000 parquet (265 GB) on local NVMe, generated with
  `test/tpch_performance/generate_tpch_data.sh 1000 --format parquet`.
- Session: one DuckDB process; all TPC-H columns pinned with `pin_table(..., tier='host')` via
  `tpch_pin_columns.py pin-all`; lineitem and orders Simpatico-compressed (other tables have no plan, so they are
  pinned uncompressed); 22 queries x 4 iterations; iteration 1 is warm-up.
- Config: the user's `specific_config.yaml` with `memory.host.capacity_bytes: 600GB` (100GB is too small for SF1000
  host pins) and `topology.gpu_ids` per run. Every run's exact config is in `configs/` and in each run directory.

## Directory map

| Path | What it holds |
|---|---|
| [CAMPAIGN.md](CAMPAIGN.md) | Shared reference cited by the reports: regime, tools, hardware facts, baselines, config-experiment results |
| [report.html](report.html) | The published one-page report (artifact page body; open in a browser) |
| `bin/` | Harness and analysis tools (see below) |
| `configs/` | Every Sirius YAML used, generated by `bin/make_config.py` |
| `00-baseline/<run>/` | Un-profiled baselines: `g1_*`, `g2s_a` (GPUs 0,1), `g2x_a` (GPUs 0,2), `g4_*`, `*_noquent` (with 1 s `dmon.txt`). `timings.csv` = query,iter,seconds; `quent/` = flattened per-query summaries |
| `01-nsys/<run>/` | Profiled runs; `nsys/map.csv` maps each report and query to its time window (reports themselves not committed) |
| `05-codemap/` | Read-only code maps of scheduling, host scan, cross-GPU movement, sync/contention, config knobs, and the critic's thread-time decomposition (`CRITIC.md`) |
| `10-analysis/` | Analyst reports: `X1_allocator.md`, `X2_exchange.md`, `X3_link.md`, `fam_*.md` (per query family), `bloom_peer_access.md`; subdirectories hold each analyst's scripts and intermediate tables |
| `10-analysis/microbench/` | Standalone CUDA microbenchmarks with sources and results: host-to-GPU concurrency, P2P, driver allocator contention |
| `20-experiments/<name>/` | Config-flip runs (`x_g1_*` / `x_g4_*`): config, timings, Quent summaries |
| `30-diagnosis/` | `FINAL.md` (verified ranking), `RANKED.md` (synthesis waterfall and per-bottleneck arithmetic), `family_map.csv`, `waterfall.py`, and two `verify_<slug>_{measurement,causality}.md` reviews per top-8 item |

Suggested reading order: this file, `30-diagnosis/FINAL.md`, `30-diagnosis/RANKED.md`, then `10-analysis/X1_allocator.md`,
`X2_exchange.md`, `X3_link.md` for mechanisms, then the `fam_*.md` reports for per-query detail.

## Tools in `bin/`

| Script | Use |
|---|---|
| `run_session.sh <cfg> <out> <iters> time\|nsys [queries]` | One session: views, host pin-all, labeled iterations; `nsys` mode captures iteration 2+ per query |
| `make_config.py <out.yaml> <telemetry_dir> <gpu_ids> [k=v ...]` | Derive a run config from the base config (`k=__DEL__` removes a key) |
| `gpu_lock_run.sh <cmd>` | Serialize GPU use across concurrent jobs (mkdir lock) |
| `run_with_dmon.sh <cfg> <out>` | Un-profiled session with `nvidia-smi dmon` sampling |
| `compare.py <run_dir>...` | Per-query hot-mean table and speedups vs the first run |
| `quent_flatten.py <telemetry_dir> <out_dir>` | Quent ndjson to tasks/task_ops/queries/pipelines/per_query_gpu CSVs |
| `inflation_buckets.py`, `op_inflation.py` | Thread-time inflation 1 vs 4 GPUs by bucket, pipeline and operator |
| `nsys_map_time.py <nsys_run_dir>` | Map nsys reports to Quent query labels by absolute time |
| `nsys_range_breakdown.py <sqlite> <nvtx substr>...` | Per NVTX range: wall vs launched-kernel time, memcpy bytes, top CUDA APIs and OS-runtime calls (`WINDOW=start,end`) |
| `api_latency_compare.py <mapA> <mapB> [queries]` | CUDA API call counts and latencies, run A vs B |
| `baseline_matrix.sh` | Produced `00-baseline/{g4_a,g1_a,g2s_a,g2x_a,g4_b,g1_b}` |
| `nsys_matrix.sh` | Produced `00-baseline/g4_noquent` (its own nsys runs were discarded: missing output directory); `g1_noquent` came from `run_with_dmon.sh` directly |
| `nsys_redo.sh` | Produced the nsys profiles used throughout: `01-nsys/nsys2_g4`, `01-nsys/nsys2_g1` |
| `nsys_missing.sh <name> <gpus> <queries>` | Produced `01-nsys/nsys3_g4` (single-range captures of Q3, Q5, Q8, Q18, Q21) |
| `flip_matrix.sh`, `flip_matrix2.sh` | Produced the `20-experiments/x_*` runs (second script: batch-size and combined follow-ups) |

`configs/specific_config.orig.yaml` is the user's base config; `make_config.py` derives every run config from it.

Scripts use absolute paths from the original campaign directory `/home/ubuntu/sirius-scaling-20261105/`, which
corresponds to this directory. Reports refer to it the same way. Re-running needs the paths edited.

## Not committed

About 15 GB of raw data stayed on the profiling machine under the campaign directory: nsys `.nsys-rep` and `.sqlite`
files, Quent ndjson telemetry, flattened `tasks.csv`/`task_ops.csv`, Sirius logs, session SQL and output, and 17
intermediate nsys text dumps over 500 KB. The committed `map.csv`, summaries and reports cite them by path.

## Measurement notes

- Profiled time inflation is small: about 1.05x at 1 GPU and 1.25x at 4 GPUs. nsys data is used for attribution
  only; every wall-time number comes from un-profiled runs.
- About 75% of host-to-GPU copies show up in nsys as copyKind `Peer-to-Peer` (pinned source registered under another
  GPU's context). Classify host copies by srcKind/dstKind, not copyKind.
- Some nsys capture ranges span several queries; always restrict events to the `map.csv` window.
- 1-GPU Q13 is not fully warm by iteration 2 (5.03/3.92/2.47/2.65 s), which slightly flatters 4-GPU scaling.
- The 8 GB join caps change Q3's join mode at 1 GPU (STANDARD to BUILD_PROBE), so compare join-policy runs at equal
  policy.
- Waterfall costs overlap; they are an attribution of the gap, not independent savings.
