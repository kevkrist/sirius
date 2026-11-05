# Host-pinned scan path: multi-GPU scaling code map (area = host_scan)

Read-only analysis of how host-pinned (`pin_table(..., tier='host')`, Simpatico compression on
lineitem/orders) scans run on 4x RTX PRO 6000. The machine has 1 NUMA node and two PCIe switches
({0,1} and {2,3}), each with one Gen5 x16 uplink. The repo is at `bb557b1a`.

Evidence sources: code (file:line below), the orchestrator smoke run
`00-baseline/smoke_g4` (Quent telemetry for q1/q6 at 4 GPUs, pin log), and the H2D microbench
`10-analysis/microbench/h2d_concurrency/results.txt`.

Scripts (scratchpad):
- `/tmp/claude-1000/-home-ubuntu-sirius/69d4bd72-ffc6-4ef4-9539-2768e3641f5c/scratchpad/bytes.py` (per-query H2D bytes)
- `/tmp/claude-1000/-home-ubuntu-sirius/69d4bd72-ffc6-4ef4-9539-2768e3641f5c/scratchpad/quent_tasks.py` (per-pipeline, per-GPU task stats)
- `/tmp/claude-1000/-home-ubuntu-sirius/69d4bd72-ffc6-4ef4-9539-2768e3641f5c/scratchpad/quent_ops.py` (per-operator phase times)

## 1. How the path works (facts)

| Step | Where | What happens |
|---|---|---|
| Pin | `src/pin_table.cpp:727-860` `materialize_pin_to_host` | Each 1 GB coalesced batch (`scan_task_batch_size` of the *full pinned column set*) is decoded on a GPU, compressed on the GPU (when a plan exists), then D2H'd into the single HOST space's 1 MiB-block pool. With 1 NUMA node, every chunk records `HOST:0`. |
| Chunk counts (smoke log) | `00-baseline/smoke_g4/log` | lineitem: 627 chunks, 144.2 GB compressed. part: 21.6 GB raw. supplier: 1.5 GB. partsupp: 11.2 GB. customer: 25.3 GB (all raw, because the `*_disabled.txt` plans are disabled). orders: about 14 GB compressed plus about 80 GB of raw `o_comment` (`identity` plan). Total pinned footprint is about 300 GB. |
| Serve | `src/scan_manager/sirius_scan_manager.cpp:332-360` | Each query gets per-chunk projections. Compressed chunks use `select_columns`. Raw chunks use `host_data_representation::slice`. Only the projected columns' bytes cross PCIe (`compressed_table_io.cpp:1026-1063`, a subset fetch). |
| Zone maps | `sirius_scan_manager.cpp:3725-3800` | Prune only whole chunks. The pin coalescer interleaves row groups (comment at `pin_table.cpp` near line 255), and TPC-H dates are not clustered, so in practice nothing is pruned for date filters. |
| Publish | `load_balancing_scan_batch_coalescer.cpp:195-290` `drain_cached_provider` | Pushes every chunk split into the `split_connector` with no backpressure. |
| Task creation and placement | `src/creator/task_creator.cpp:701-847` | The creation loop drains all splits into tasks immediately. For a resident HOST chunk, `preferred_device_id = _numa_affinity.next_gpu(host_numa, gpus_of(numa))` (lines 804-811). This is a global round-robin cursor per NUMA key (`src/memory/numa_affinity.hpp:29-41`), shared by all pipelines and queries. The ready-device hint is ignored. |
| Scheduling | `src/pipeline/task_scheduler.cpp:302-333` | A preference is binding: no stealing. A device with no matching task emits `publish_executor_awaiting_task`. |
| H2D (compressed) | `src/compression/compression_converters.cpp:196-220` -> `compressed_representation.cpp:62-86` | One `cudaMemcpyAsync` per leaf buffer, split at 1 MiB host-block boundaries, on the task stream. Decode runs on the GPU (Simpatico) on up to 4 lanes and is joined and waited before return (`compressed_scan.cpp:643-731`). |
| H2D (raw host) | `cucascade/src/cudf/representation_converter_builtins.cpp:1186-1240` | Pre-sync, then one `cudaMemcpyBatchAsync` (with 1 MiB segments), then `final_sync`. |
| Prepare is synchronous | `src/pipeline/gpu_pipeline_task.cpp:725-737` | `prepare_for_processing` followed by `stream.sync()`. Quent `Preparing` therefore equals H2D+decode wall time. |
| Overlap engine | `memory_prefetcher`, `sirius_scan_manager.cpp:~2155` and configuration.md | Disabled by default, and it refuses to run when the query has more than one eligible GPU. |

Decompression is on the GPU. It cuts lineitem PCIe bytes about 3.3x versus raw (my estimate is 3.4 TB raw vs 1.1 TB as pinned for the suite).

## 2. Per-query H2D bytes (estimate)

The estimate uses `tpch_pin_columns.QUERY_COLUMNS`, the plan-file compression ratios, average TPC-H string widths, and SF1000 row counts. Each table is counted once per query. Repeated scans are not counted: Q21 lineitem x3, Q17/Q18 lineitem x2, Q11 partsupp x2, Q22 customer x2. Together these add roughly 120 GB.

| q | H2D GB | lineitem GB | floor 1 GPU (52 GB/s) | floor 4 GPU (105 GB/s) |
|---|---|---|---|---|
| 1 | 45.5 | 45.5 | 0.87 | 0.43 |
| 2 | 20.5 | 0 | 0.39 | 0.20 |
| 3 | 45.7 | 34.1 | 0.88 | 0.44 |
| 4 | 25.8 | 22.0 | 0.50 | 0.25 |
| 5 | 53.5 | 43.1 | 1.03 | 0.51 |
| 6 | 40.0 | 40.0 | 0.77 | 0.38 |
| 7 | 60.3 | 52.1 | 1.16 | 0.57 |
| 8 | 81.1 | 64.1 | 1.56 | 0.77 |
| 9 | 94.2 | 74.0 | 1.81 | 0.90 |
| 10 | 60.1 | 26.6 | 1.15 | 0.57 |
| 11 | 11.3 | 0 | 0.22 | 0.11 |
| 12 | 61.9 | 60.3 | 1.19 | 0.59 |
| 13 | 86.2 | 0 (o_comment raw 80) | 1.66 | 0.82 |
| 14 | 57.8 | 51.3 | 1.11 | 0.55 |
| 15 | 49.0 | 48.3 | 0.94 | 0.47 |
| 16 | 16.9 | 0 | 0.32 | 0.16 |
| 17 | 55.3 | 49.0 | 1.06 | 0.53 |
| 18 | 31.6 | 13.7 | 0.61 | 0.30 |
| 19 | 90.0 | 82.9 | 1.73 | 0.86 |
| 20 | 77.1 | 58.0 | 1.48 | 0.73 |
| 21 | 41.7 | 40.0 | 0.80 | 0.40 |
| 22 | 10.5 | 0 | 0.20 | 0.10 |
| **sum** | **~1116 (~1240 with repeat scans)** | | **21.5-24 s** | **10.6-11.8 s** |

Against the measured totals, the link floor is about 60-66% of the 1-GPU 35.8 s and about 50-54% of the 4-GPU 22.0 s. The link alone caps the suite at about 3.0x, and the 4-GPU link-bound part can never scale past 2.0x. The remaining ~10 s at 4 GPUs is outside the scan link.

**Smoke-run check (Quent, 4 GPUs):**
- q6 iter2: the scan pipeline spans 425 ms for about 40 GB, which is about 94 GB/s aggregate and close to the 105 GB/s ceiling.
- q1 iter2: the scan spans 906 ms for about 45 GB, about 50 GB/s. q1 is compute-bound: `HASH_GROUP_BY` takes a median 9.9 ms per task, against 7.5 ms for `Preparing`.
- All 4 threads per GPU were busy: 3.5 s of busy thread-time per GPU over a 0.9 s span.
- Tasks were split 157/157/156/157 across the GPUs, and the per-GPU end times are within 30 ms of each other.

## 3. Ranked hypotheses

### H1 (hardware, high): the PCIe uplink caps host-to-GPU at 2x, and about half the 4-GPU suite time is on that link
- **Mechanism.** All pinned data lives in the single host NUMA node. Every scan task pulls its projected bytes over its GPU's switch uplink. Two uplinks give 105 GB/s total, so each GPU gets 26 GB/s at 4 GPUs, versus 52 GB/s for 1 GPU. The bytes do not shrink with GPU count.
- **Evidence.**
  - Microbench: aggregate H2D is 52 GB/s for 1 GPU and 105 GB/s for 4.
  - Byte table in section 2: the floor is 10.6-11.8 s of the 22.0 s at 4 GPUs.
  - Smoke q6: scan bandwidth reaches about 94 GB/s.
  - Code path: `compressed_representation.cpp:62-86`, `representation_converter_builtins.cpp:1186-1240`.
- **Hits.** Scan-dominated queries that move lineitem bytes: q1, q6, q12, q14, q15, q19, q20, q9, q8, q17. Also q13 (`o_comment`) and q10 (customer).
- **nsys signature.**
  - Each GPU's H2D memcpy throughput sits flat at about 26 GB/s inside `sirius::compression::host_to_gpu` / `simpatico::io::fetch_payload` and `convert:host_to_gpu:flush_submit` ranges.
  - Summed H2D bytes per query roughly match the byte table.
  - Kernel occupancy is low while copies are continuous.
  - At 1 GPU, the same bytes go through at about 52 GB/s.
- **Quent signature.**
  - Scan tasks' `Preparing` (`origin_tier=HOST`) grows about 2x per task from 1 to 4 GPUs. (Note: `input_bytes` is the estimated size, not wire bytes.)
  - The scan pipeline's span is about bytes / 105 GB/s.
  - 3.9 of 4 executor threads are busy on every GPU.
- **Experiment.**
  1. Run `nsys stats --report cuda_gpu_mem_size_sum,cuda_gpu_mem_time_sum` per query at 1 and 4 GPUs and compute H2D GB/s per GPU over the scan window.
  2. Run the suite with `gpu_ids: [0,2]` (two GPUs, two uplinks). Link-bound queries should land near the 4-GPU time. Compute-bound queries such as q1 should land near the halfway point.

### H2 (config, high): the 2-GPU data point uses two GPUs that share one switch, so link-bound work gets no speedup
- **Mechanism.** `topology.num_gpus: 2` takes a prefix of the visible GPUs (`sirius_config.cpp:525-541`, configuration.md "num_gpus"), which is {0,1}. Both sit behind one uplink, so the aggregate is 52 GB/s, the same as 1 GPU. That explains the 1.06-1.16x at 2 GPUs. The cross-switch pair {0,2} gets 102 GB/s.
- **Evidence.** Microbench results; the log line "topology summary"; `specific_config.yaml` uses `num_gpus`.
- **nsys signature.** With {0,1}, each GPU's H2D runs at about 26 GB/s. With {0,2}, it runs at about 51 GB/s.
- **Quent signature.** Scan `Preparing` medians with {0,1} equal the 4-GPU values. With {0,2} they equal the 1-GPU values.
- **Experiment.** Run 2 GPUs as `gpu_ids: [0,2]` vs `[0,1]` on q6/q12/q19 and the full suite. Expect {0,2} to be about 1.7-1.9x on link-bound queries.

### H3 (design, medium-high): cross-switch exchange is host-staged and rides the same saturated uplinks, adding H2D bytes that do not exist at 1 GPU
- **Mechanism.** There is no P2P between switch pairs: the smoke log shows `Pool access unavailable` for 0<->2, 0<->3, 1<->2 and 1<->3. `peer_copy_into` (`cucascade/src/cudf/representation_converter_builtins.cpp:596-695`) stages each column buffer through the single HOST pool:
  - a D2H leg in 1 MiB `cudaMemcpyAsync` pieces, then `target_stream.sync()`;
  - then an H2D leg in 1 MiB pieces, then another `sync()`.

  The two legs are store-and-forward with no overlap between them. With round-robin partition placement over 4 GPUs (`partition_placement::round_robin`), about 3/4 of each partitioned input moves to another GPU, and half of that crosses switches. Those moves carry *decoded* rows (3-5x the compressed scan bytes). The H2D leg competes directly with scan H2D. Microbench bidir shows 175 GB/s rather than 105+114, so the D2H leg also takes some H2D headroom.
- **Hits.** Large partitioned joins and aggregations: lineitem⋈orders (q3, q4, q5, q7, q8, q9, q10, q12, q18, q21), partsupp joins (q2, q9, q11, q16, q20), and q13 orders⋈customer.
- **nsys signature.**
  - NVTX `convert:gpu_to_gpu:host_staging` ranges, each containing a D2H burst on device A, then `cudaStreamSynchronize`, then an H2D burst on device B with the same byte count.
  - Total H2D bytes per query exceed the scan-byte estimate at 4 GPUs.
  - No `convert:gpu_to_gpu:peer_copy` between switch pairs.
- **Quent signature.**
  - Consumer tasks (`PARTITION` → join/merge) show `Preparing` with `origin_tier=GPU`, and long durations on GPUs that differ from the producer's.
  - `ExecutorAwaitingTask` events while staged clones are in flight.
- **Experiment.**
  1. Using nsys, sum bytes inside `host_staging` ranges per query (q9, q18, q21) at 4 GPUs.
  2. Compare 2-GPU `[0,1]` (P2P, one uplink) against `[0,2]` (host-staged, two uplinks) on q9/q18/q21. If `[0,2]` loses its scan advantage on join-heavy queries, staging is eating it.
  3. Confirm `probe_peer_dma_works(0,2)==false` in the debug log.

### H4 (design/mechanical, medium): per-task synchronous H2D→decode→compute with a fixed 4 threads per GPU, and no H2D/compute overlap engine in multi-GPU mode
- **Mechanism.** Each scan task does the following on one executor thread, waiting at each step:
  1. H2D fetch;
  2. decode, then wait (`compressed_scan.cpp:643-731`; the stream-lease release syncs);
  3. `stream.sync()` (`gpu_pipeline_task.cpp:735-737`);
  4. compute operators.

  The only H2D/compute overlap comes from the other 3 threads on that GPU. At 4 GPUs the per-GPU link rate halves, so the H2D part of `Preparing` doubles. With a fixed number of threads in flight, per-GPU throughput falls (Little's law). `memory_prefetcher` refuses multi-GPU queries (`sirius_scan_manager.cpp:~2155`). Splits are pre-created and static, so a free thread cannot pull ahead on another GPU's data.
- **Evidence.**
  - Quent smoke at 4 GPUs: all 4 threads per GPU were busy.
  - q1 `Preparing` median is 7.5 ms against 15 ms of compute per task; q6 is 3.2 ms against about 6.9 ms.
  - On average, 1.3-1.4 tasks per GPU are in `Preparing` at once.
  - The GB300 tuned profile (configuration.md) found `pipeline.num_threads` 4→8 gave -16% on q1 and -14% on q12.
- **nsys signature.**
  - Per-GPU copy-engine idle gaps that line up with all 4 threads sitting in compute or `cudaStreamSynchronize` (`convert:host_to_gpu:final_sync`, decode lease release).
  - SM idle while copies run, and the reverse.
  - Low kernel concurrency.
- **Quent signature.**
  - Per-task `Preparing` scales about 2x from 1 to 4 GPUs while `op*` compute per task stays flat.
  - Busy thread-time per GPU is about 4 x span.
- **Experiment.**
  1. Set `executor.pipeline.num_threads: 8` at both 1 and 4 GPUs. If the 4-GPU gain exceeds the 1-GPU gain, the latency-hiding deficit is a scaling factor.
  2. Optionally test `memory_prefetcher.enable: true` at 1 GPU to bound the overlap benefit. It cannot run at 4 GPUs.

### H5 (config/plan, medium): compression plans and pin layout are tuned for GB300 GPU-tier pins, not a 26 GB/s-per-GPU host link
- **Mechanism.**
  - The plans are "Pareto-picked: max ratio with decompress >= 250 GB/s" (header of `plans/tpch_sf1000/lineitem.txt`). That criterion suits a 370 GB/s C2C link. Here the link is 26 GB/s per GPU, so higher-ratio, slower-decode plans pay off.
  - `o_comment -> identity` carries a "GPU-TIER OVERRIDE" tag, yet it is used for the HOST pin. q13 ships about 80 GB raw. Snappy at about 2.5x would save about 48 GB, roughly 0.46 s at 4 GPUs. `docs/super-sirius/compressed-pinning.md` says snappy is right for host pins.
  - `l_shipmode` uses `str_split` at 1.7x, about 29 GB per query (q12, q19). It has only 7 distinct values, so dictionary plus bitpack would be about 2-3 GB.
  - customer, part, partsupp and supplier are pinned raw (`*_disabled.txt`). q10 ships about 25 GB of customer.
  - Zone maps prune nothing for date filters (q6, q14, q15, q20) because chunks interleave row groups and dates are not clustered. Every chunk is shipped.
- **Why it hurts scaling.** Bytes on a link capped at 2x put a 2x ceiling on whatever fraction of time they occupy. Removing bytes raises the scaling ceiling.
- **nsys signature.** H2D bytes per query match the table in section 2. q13's H2D is dominated by `o_comment`.
- **Quent signature.** q13 `GPU_SCAN` on orders has the longest `Preparing` per task among the orders scans.
- **Experiment.** Re-pin orders with an `o_comment` snappy plan and lineitem with a dictionary plan for `l_shipmode`. Measure q13, q12 and q19 at 1 and 4 GPUs. Expect the 4-GPU gain to be about 2x the 1-GPU gain in relative terms.

### H6 (design, low-medium): binding, statically pre-assigned round-robin placement of host-chunk scan tasks
- **Mechanism.**
  - Host data is the same distance from every GPU, yet each chunk gets a binding device from a global cursor (`task_creator.cpp:804-811`) when its task is created. All tasks are created up front, because nothing applies backpressure in `drain_cached_provider`.
  - The scheduler never steals (`task_scheduler.cpp:302-333`), and the lookahead `device_id_hint` is ignored.
  - The cursor is shared across concurrent scan pipelines and queries (`numa_affinity` is keyed only by NUMA node). Interleaved creation can therefore skew a pipeline's chunks onto a subset of GPUs.
  - Small tables have fewer chunks than the 16 thread slots: supplier about 2, partsupp about 11, part about 21, customer about 25. This leaves ceiling(n/4) tails.
  - A GPU that also carries heavier pinned-partition work (join build/probe) cannot shed scan tasks to idle GPUs.
- **Evidence.** In the smoke run, single-scan q1/q6 are balanced: 157/157/157/156, with end times within 30 ms. So this is *not* a factor for single-scan queries. The risk is confined to multi-scan and join queries.
- **nsys signature.** Per-GPU end of `GPU_SCAN` activity is skewed within a pipeline. One GPU stays busy while others idle at a pipeline tail.
- **Quent signature.**
  - Histogram of `Routing.preferred_device_id` per scan pipeline (`quent_tasks.py` prints it).
  - The per-device last `Finalizing` is skewed.
  - `ExecutorAwaitingTask` fires for idle devices while the queue is non-empty.
- **Experiment.** Run `quent_tasks.py` on 4-GPU telemetry for q2, q9, q13, q18, q21. A code change to test it: leave host-tier cached chunks preference-less (no_preferred_device), so any ready GPU takes them.

### H7 (mechanical, low): 1 MiB host blocks split every H2D (and the host-staged legs) into ≤1 MiB DMAs
- **Mechanism.**
  - `memory.host.block_size: 1048576` causes `copy_pinned_blocks_to_device` to issue one `cudaMemcpyAsync` per MiB per leaf buffer (`compressed_representation.cpp:62-86`).
  - The raw-host path batches 1 MiB segments into `cudaMemcpyBatchAsync`.
  - The staging path loops over 1 MiB pieces.
  - The microbench's 26/52 GB/s figures used 1 GiB copies, so the effective rate with 1 MiB pieces under 4-way switch contention is unmeasured.
  - The GB300 tuned profile measured -13% for the suite with 64 MiB blocks.
- **nsys signature.** Thousands of ≤1 MiB H2D memcpys per task, with gaps between them and an achieved rate below the bulk rate.
- **Quent signature.** None beyond a larger `Preparing`.
- **Experiment.**
  1. Set `memory.host.block_size: 64MiB`, `pool_size: 8` at 1 and 4 GPUs. Expect at most a few percent, mostly not scaling-specific.
  2. Microbench 1 MiB-chunk H2D on 4 GPUs concurrently.

### H8 (config, low): `specific_config.yaml` host capacity (100 GB) is far below the about 300 GB pinned footprint
- lineitem alone is 144 GB compressed. If the user's original 1/2/4-GPU numbers used 100 GB, some pins may have failed or degraded, so scans would have mixed in NVMe/parquet. The orchestrator's `make_config.py` already sets 600 GB.
- **Experiment.** Grep the original run logs for pin failures or `host reservation failed`, and confirm all 8 tables' pins finished `ok`.

## 4. Observability notes
- Compressed host batches are created without a Quent DataBatch probe (`sirius_scan_manager.cpp:353`), so their H2D does not appear in Quent DataBatch events. Use nsys for wire bytes.
- `Preparing` = H2D+decode wall time. `stream.sync()` is placed at `gpu_pipeline_task.cpp:736` specifically for accurate timing.
- NVTX to filter on:
  - `sirius::compression::host_to_gpu`
  - `simpatico::io::read_table[memory,subset]`
  - `simpatico::io::fetch_payload`
  - `convert:host_to_gpu*`
  - `convert:gpu_to_gpu:host_staging`
  - `convert:gpu_to_gpu:peer_copy`
