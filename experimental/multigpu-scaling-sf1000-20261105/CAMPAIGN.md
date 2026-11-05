# Campaign reference: regime, hardware facts, and measured results

Shared reference for every report in this bundle. Reports cite paths under the original campaign directory
`/home/ubuntu/sirius-scaling-20261105/`, which corresponds to this directory.

## Goal and figure of merit

- Ranked diagnosis of what blocks linear scaling from 1 to 4 GPUs. Diagnosis only; no engine changes.
- Figure of merit: suite total of the mean hot time (iterations 2-4 of 4; iteration 1 is warm-up and JIT) over the
  22 TPC-H queries. Ideal 4-GPU time = 1-GPU time / 4.

## Regime

- Repo `/home/ubuntu/sirius` at `bb557b1a`, prebuilt `sirius-duckdb/build/release`.
- Data: `/opt/dlami/nvme/tpch_parquet_sf1000` (tpchgen-rs, 265 GB parquet).
- Config: `specific_config.yaml` with `memory.host.capacity_bytes: 600GB` and `topology.gpu_ids` per run
  (`bin/make_config.py`). Quent on; its overhead is nil (4-GPU suite 20.04 s on, 20.00 s off).
- Session (`bin/run_session.sh <cfg> <out> <iters> time|nsys`): one DuckDB process; views over the parquet files;
  `CALL pin_table(..., tier='host')` for all TPC-H columns (`tpch_pin_columns.py pin-all`; Simpatico compression for
  lineitem and orders, the other tables have no plan and are pinned uncompressed; pin takes about 114 s); then each
  query for `<iters>` labeled iterations (`sirius_set_query_label('tpch_q<N>_iter<I>')`).
- nsys mode: iteration 1 uncaptured, iteration 2 captured via `profiler_start`/`profiler_stop`
  (`--capture-range=cudaProfilerApi --capture-range-end=repeat`, trace cuda, nvtx, osrt). Some capture ranges span
  several queries; `bin/nsys_map_time.py` maps each query's `sirius::query` window by absolute time against the
  Quent labels and writes `nsys/map.csv`.
- GPU access is serialized with `bin/gpu_lock_run.sh`.

## Tools

- `bin/compare.py <run_dir>...`: per-query hot means and speedups.
- `bin/quent_flatten.py <telemetry_dir> <out_dir>`: flatten Quent ndjson to CSV tables.
- `bin/op_inflation.py <quentA> <quentB> q18_iter3`, `bin/inflation_buckets.py <quentA> <quentB>`: thread-time
  inflation by pipeline, operator and bucket.
- `bin/nsys_range_breakdown.py <report.sqlite> <nvtx substr>...`: per NVTX range, wall vs launched-kernel time,
  memcpy bytes, top CUDA APIs and OS-runtime calls.
- `bin/api_latency_compare.py <mapA> <mapB> [queries]`: CUDA API call counts and latency, run A vs B.

## Hardware facts (measured)

- 1 socket, 1 NUMA node, 96 cores, 999 GB RAM.
- GPUs 0 and 1 sit behind PCIe switch A (root port 44:00.0); GPUs 2 and 3 behind switch B (55:00.0). Each switch
  has one Gen5 x16 uplink.
- P2P is supported only within a switch (0<->1, 2<->3); cross-switch pairs report NS in `nvidia-smi topo -p2p`.
  Sirius logs "Pool access unavailable" for the 8 cross-switch ordered pairs.
- Pinned host-to-GPU aggregate (`10-analysis/microbench/h2d_concurrency`): 1 GPU 52 GB/s; {0,1} 52; {0,2} 102;
  {0,1,2,3} 105 (26 per GPU). GPU-to-host: 57 / 57 / 114 / 114.
- P2P (`10-analysis/microbench/p2p`): intra-switch 53.7 GB/s; cross-switch (driver-staged) 43; 4 intra pairs 209
  aggregate; 4 cross pairs 77 aggregate (19 per pair); 4 intra pairs with host-to-GPU traffic on all GPUs 27 per
  pair (host-to-GPU 109 aggregate); 4 cross pairs with host-to-GPU on all GPUs 11 per pair (host-to-GPU 111).
- Driver stream-ordered allocator (`10-analysis/microbench/alloc_contention`): `cudaMallocFromPoolAsync` and
  `cudaFreeAsync` are serialized process-wide at about 280-385k calls/s whatever the GPU count. 1 GPU x 4 threads:
  340k calls/s, mean 10.9 us. 4 GPUs x 4 threads: 279k calls/s aggregate (17k per thread), mean 56 us, p90 117 us.
  1 GPU x 16 threads: 385k calls/s, mean 38 us. Pool peer-access grants make no difference.
- nsys GPU metrics need admin rights and were unavailable; `nvidia-smi dmon` at 1 s was used instead.

## Baselines (un-profiled, hot mean, seconds)

| Run | GPUs | Suite | x vs 1 GPU |
|---|---|---|---|
| `g1_a` / `g1_b` | {0} | 35.82 / 35.86 | 1.00 |
| `g2s_a` | {0,1}, same switch | 29.40 | 1.22 |
| `g2x_a` | {0,2}, across switches | 22.21 | 1.61 |
| `g4_a` / `g4_b` | {0,1,2,3} | 20.04 / 20.14 | 1.79 |

Noise: suite standard deviation over hot iterations is about 0.02 s at 4 GPUs and 0.6 s at 1 GPU, where Q13 is not
fully warm by iteration 2 (5.03 / 3.92 / 2.47 / 2.65 s).

## Observations used across reports

- Quent: at 4 GPUs each GPU has a task in flight about 95% of query wall time and task counts are balanced, so the
  loss is per-task inflation rather than idle GPUs. Executor thread-time grows from 132 s to 252 s for identical
  work (`05-codemap/CRITIC.md` section 0). Buckets, 1 GPU to 4 GPUs (thread-s): scan prepare 79 to 132; scan op 16
  to 28; cross-GPU clone prepare 0.6 to 25.6; PARTITION op 6 to 30; other ops 28 to 35.
- dmon at 4 GPUs (`00-baseline/g4_noquent/dmon.txt`): per-GPU PCIe receive mean 13-16 GB/s, p90 about 30 GB/s;
  memory-bandwidth utilization about 11%; SM active about 64%; power about 135 W of 600 W.
- nsys, all 22 queries, warm iteration 2, 1 GPU to 4 GPUs: `cudaMallocFromPoolAsync` 911k to 1.20M calls, 7.3 s to
  42.4 s (mean 8 to 35 us); `cudaFreeAsync` 2.7 s to 35.7 s; `cuStreamSynchronize` 95k to 387k calls, 3.8 s to
  21.8 s; `cudaStreamSynchronize` 87 s to 116 s.
- In a 4-GPU Q21 trace, `sirius_physical_partition::execute` spends 10.3 s of wall in ranges whose launched kernels
  take 0.37 s; the rest is allocator calls with OS-level rwlock and mutex waits inside them. Host-staged clones take
  1.22 ms per call vs 0.36 ms for peer clones.
- Accumulated Bloom publication is always host-staged at 4 GPUs (`10-analysis/bloom_peer_access.md`).

## Config experiments (un-profiled, hot mean suite, seconds)

| Name | Change | 1 GPU | 4 GPUs |
|---|---|---|---|
| `x_g*_joinpol` | `max_build_hash_table_bytes` default (8GB), `max_broadcast_join_size: 8GB` | 34.74 | 19.07 |
| `x_g4_hpb256` | `hash_partition_bytes: 256MB` | - | 23.66 |
| `x_g4_hpb4g` | `hash_partition_bytes: 4GB`, `concat_batch_bytes: 4GB` | - | 19.91 |
| `x_g*_batch4g` | `scan_task_batch_size: 4GB` | 33.04 | 16.91 |
| `x_g4_batch8g` | `scan_task_batch_size: 8GB` | - | 17.34 |
| `x_g*_best` | batch4g + joinpol | 31.93 | 16.92 |
| `x_g*_thr8` | `executor.pipeline.num_threads: 8` | 35.55 | 21.23 |
| `x_g4_gpupin` | pin tier `gpu` (no host link) | - | 18.26 |

- joinpol: at 1 GPU only Q3 changes (STANDARD to BUILD_PROBE join), so equal-policy scaling is 1.82x. Q5, Q8, Q9 and
  Q12 each gain 0.2-0.3 s only at 4 GPUs (broadcast replaces a shuffle); Q7 (+0.15 s) and Q14 (+0.07 s) regress.
- hpb256: Q18 1.56 to 3.67 s, Q13 1.09 to 1.80 s. hpb4g: Q18 1.56 to 1.25 s, Q9 +0.11 s.
- batch4g at 4 GPUs: Q1 1.09 to 0.73 s, Q4 0.62 to 0.38, Q18 1.56 to 1.00, Q12 1.07 to 0.78, Q21 1.99 to 1.58.
- gpupin is confounded by GPU memory pressure (downgrade requests; Q3 2.62 s, Q10 1.57, Q18 2.22, unstable
  iterations). Queries without pressure gain a lot (Q2 0.37 to 0.10 s, Q11 0.25 to 0.09, Q17 0.84 to 0.20, Q20 0.77
  to 0.22, Q19 0.96 to 0.47, Q6 0.47 to 0.30), but Q1 only 1.09 to 0.91 s while batch4g with host pins reaches
  0.73 s. At 4 GPUs, per-task software overhead outweighs the link for scan-heavy queries; the link dominates the
  small and dimension-heavy queries that read uncompressed tables.
