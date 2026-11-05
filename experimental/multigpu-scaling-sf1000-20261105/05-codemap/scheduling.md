# Scheduling and work distribution: what blocks 1 to 4 GPU scaling (code map)

Scope: task_creator, task_scheduler, gpu_pipeline_executor, preferred-device resolution, partition placement and count, barriers, single-GPU tails. This is read-only analysis at commit bb557b1a. Nothing was run.

## 0. Facts established from code (answers to the direct questions)

| Question | Answer | Evidence |
|---|---|---|
| Is `executor.pipeline.num_threads` per GPU or shared? | **Per GPU.** There is one `gpu_pipeline_executor` per GPU memory space, each built from a copy of the config. 4 threads x 4 GPUs = **16 executor threads**, plus one manager-loop thread per GPU. | `src/pipeline/task_scheduler.cpp:74-108`; `src/parallel/task_executor.cpp:104` (pool sized `num_threads`); `src/pipeline/gpu_pipeline_executor.cpp:57` (stream pool sized `num_threads`) |
| Task creator threads | **Global.** 4 creation workers serve all GPUs, fed by one manager thread. The count does not grow with GPU count. | `src/creator/task_creator.cpp:377-381, 592-882` |
| Scheduler | **One** management thread matches tasks to all GPUs. | `src/pipeline/task_scheduler.cpp:177, 253-375` |
| Is device preference binding? | **Yes, always.** A ready GPU takes only tasks that prefer it or tasks with no preference. It cannot take a task bound to another GPU (no work stealing). | `src/pipeline/task_scheduler.cpp:303-335` |
| Host-pinned scan task placement | `scan_operator_input` (cached host chunk) carries no device. The task creator picks `_numa_affinity.next_gpu(host_numa, gpus_of(numa))`, a global per-NUMA round-robin cursor. With a single NUMA node this is plain RR 0,1,2,3 in creation order, bound at creation time. | `src/creator/task_creator.cpp:793-822`; `src/memory/numa_affinity.hpp:31-37` |
| Downstream task placement | Producer stamp (partition placement) wins. Otherwise the task goes to the GPU holding the most input bytes. | `src/creator/task_creator.cpp:723-782` |
| Partition placement | `round_robin(P, gpus)`: partition p goes to `gpus[p % N]`. Every exchange starts at GPU 0, with no rotation and no topology awareness. Broadcast uses `one_per_device`. Single-partition BUILD_PROBE rotates by operator id. | `src/op/partition_placement.hpp:59-76, 162-179`; `src/op/sirius_physical_hash_join.cpp:840-911` |
| Partition count | `natural = max(ceil(bytes / hash_partition_bytes), N if bytes >= N*16MB)`. BUILD_PROBE uses `min(natural, N)`. STANDARD and grouped-merge use `natural`. | `src/op/partition/partition_sizing.hpp:101-120`; `src/op/sirius_physical_hash_join.cpp:873, 900`; `src/op/sirius_physical_grouped_aggregate_merge.cpp:179-215` |
| Admission | Defaults `gpus_per_query=0` and `admission_bytes_per_gpu=0` mean every query gets all GPUs. Admission is not a factor with this config. | `src/sirius_config.cpp:429-430, 531`; `src/planner/gpu_admission.hpp` |
| Lookahead / priority | `strategy=active` (purely demand driven) and `priority=source` (scan pipelines first within a branch). Neither can be set from YAML. | `src/creator/config.hpp:101,106`; `src/creator/task_creator.cpp:196-266` |

### Using the scaling table: fixed overhead is not the problem

Fit `T = a + b*SF` on SF100 and SF1000 (22-query warm totals):

| GPUs | b (s/SF) | a (s) | Marginal speedup b1/bN |
|---|---|---|---|
| 1 | 0.03589 | ~0 | 1.00 |
| 2 (`num_gpus:2` = prefix [0,1], **same PCIe switch**) | 0.03066 | 0.23 | **1.17x** |
| 4 | 0.02161 | 0.36 | **1.66x** |

The size-independent part at 4 GPUs is only about 0.36 s across 22 queries. The scaling loss is **per byte**, not per query. The {0,1} pair has an H2D ceiling of 52 GB/s, the same as one GPU, and gets 1.17x. Four GPUs have a ceiling of 105 GB/s (2.0x) and get 1.66x. So the workload behaves as mostly H2D-bound. The scheduling-side question is what takes up the gap from about 2.0x down to 1.66x, and what keeps the design from beating the H2D ceiling (for example by moving fewer bytes per query over the shared uplinks).

---

## H1 (high, design): placement forces an all-to-all shuffle of (N-1)/N of every exchanged byte, over the same PCIe uplinks the host scan needs

**Mechanism.** Every partitioned consumer (BUILD_PROBE / STANDARD hash join, grouped-aggregate merge, dense count join) pins partition p to `gpus[p % N]` (`partition_placement.hpp:59-71`). The task creator binds every task of that partition to that GPU (`sirius_physical_operator.hpp:421-437` `partitioned_operator_data` stamps the device; `task_creator.cpp:730-732` honors it; `task_scheduler.cpp:307-324` makes it binding).

Producer PARTITION tasks run wherever the scan put the batch (locality, `task_creator.cpp:753-760`). For the RR host scan that is uniform over the GPUs. So the downstream CONCAT or join task for partition p finds about (N-1)/N of its pieces on other GPUs. `lock_or_prepare_batch` clones them in task prepare through `convert_gpu_to_gpu` (`src/pipeline/batch_lock_utils.hpp:123-137`; `cucascade/src/cudf/representation_converter_builtins.cpp:900-950`). That path issues one copy per column and ends in `target_stream.sync()`. It is a peer copy if `probe_peer_dma_works`, otherwise a D2H+H2D host-staged copy (`:574-640`).

- N=1: 0% of exchanged bytes move between devices.
- N=2: 50%.
- N=4: 75%. Of the remote bytes, 2/3 cross switches (for example GPU0 to GPU2/3), so they go through **both** switch uplinks and the root complex. Those are the same x16 links that carry the host-pinned H2D scan, and that scan is already the bottleneck (105 GB/s aggregate). If peer DMA is not verified, **every** remote byte (even within a switch) becomes D2H plus H2D through the uplinks.
- With Simpatico, the scan moves **compressed** bytes over PCIe, but the shuffle moves **decoded cudf columns**. Per logical row, shuffle bytes can match or exceed scan bytes.

Broadcast, which avoids the probe shuffle by routing probe batches `local`, only applies when `build < N*16MB` or `build < max_broadcast_join_size (256MB default)` with probe/build >= 1.25N, **and** when BUILD_PROBE is eligible with the whole build counted per GPU (`sirius_physical_hash_join.cpp:867-899`). At SF1000 almost every join on o_orderkey, ps_partkey, c_custkey, or a large group-by is a shuffle. Pin placement (`pin_table` per-batch RR) is not co-partitioned, so no join is ever local.

A side effect of BUILD_PROBE count = `min(natural, N)`: at N=1 a 57M-row orders build (for example Q10) is a single local hash table and the lineitem probe never moves. At N=4 the same join has 4 partitions and 75% of the lineitem probe crosses GPUs. The engine adds a full probe shuffle exactly when it adds GPUs.

**Queries hit.** Shuffle joins and big group-bys: Q3, Q5, Q7, Q8, Q9, Q10, Q12, Q13, Q18 (group by l_orderkey with about 1.5B groups, then a join to orders), Q21 (lineitem self-joins on orderkey), Q20, and Q2/Q11/Q16 (partsupp). Q1 and Q6 should be unaffected.

**nsys signature.**
- NVTX ranges `convert:gpu_to_gpu:peer_copy` or `convert:gpu_to_gpu:host_staging` inside `Pipeline N Task M [CONCAT ...]` and join-task ranges.
- `[CUDA memcpy PtoP]` rows, or D2H+H2D pairs on the same stream for staging, each followed by `cudaStreamSynchronize`.
- During shuffle phases, the H2D throughput of scan tasks on the other GPUs dips.
- Total PtoP bytes per query scale about 0.75 x exchanged bytes at N=4 and are 0 at N=1.

**Quent signature.**
- Task FSM `reserving.bytes_to_materialize` (input bytes that live in another memory space) is greater than 0 on CONCAT, HASH_JOIN, and GROUPED_AGGREGATE_MERGE tasks at N=4, and about 0 at N=1.
- The `preparing` state of those tasks is long relative to `computing`.
- Summed per query, `bytes_to_materialize` / (total bytes those tasks consume) should be about 0.75.

**Cheap experiments.**
1. Quent only, no rerun: sum `bytes_to_materialize` per query and operator type at g1 and g4. Correlate the per-query 4x speedup with shuffle bytes / scan bytes.
2. Config flip at 4 GPUs: `max_broadcast_join_size: 2GB`, plus `max_build_hash_table_bytes: 8GB` (or remove the key so it derives 8x hash_partition_bytes). This turns moderate builds into broadcast with `local` probes, so the probe never moves. Expect Q5, Q7, Q8, Q10, Q12 and similar to improve at 4 GPUs and stay flat at 1 GPU.
3. nsys: count `host_staging` vs `peer_copy` NVTX ranges. Any `host_staging` means the shuffle doubles uplink traffic.

**Prior: high.** It is the only mechanism whose cost grows with N while the H2D-limited scan shrinks sub-linearly.

---

## H2 (high, design + mechanical): stragglers at global barriers with binding preferences and no work stealing; placement biased toward GPU0 and GPU1 (same switch)

**Mechanism.**
- Every task is bound to a GPU when it is created: scans by RR cursor, partitions by `p % N`. A preference is binding (`task_scheduler.cpp:307-324`). A GPU that runs out of its own tasks parks in `_ready_devices` while other GPUs still have queued work (`task_scheduler.cpp:325-335` publishes `executor_awaiting_task`).
- Phases are separated by **FULL** barriers that wait for the slowest GPU:
  - Build-side PARTITION input is FULL (`consumer_partition_policy.cpp:62,109`).
  - Grouped-merge PARTITION input is FULL unless `enable_runtime_size_estimation` (default false) (`:143-144`; `sirius_config.hpp:245`).
  - BUILD_PROBE build CONCAT is `concat_all` (`:84-86`).
  - Exchange resolution waits on the sizing input (`partition_exchange.cpp:77-104`).
  - The base hint returns WAITING while any FULL source pipeline is unfinished (`sirius_physical_operator.cpp:394-403`). `is_pipeline_finished` means every task on every GPU is done.
- Imbalance sources:
  1. **P mod N.** Multi-partition placement always starts at GPU 0, so with P = 5, 6, 9, 10 ... GPU0 (and GPU1) get ceil(P/N) partitions and the others get floor(P/N). For natural P=5 at N=4 the phase makespan is 2/1.25 = **1.6x** the balanced time. At N=1 there is no inflation. `hash_partition_bytes: 1GB` makes P small (5-12) for 5-12 GB inputs, which is the worst range.
  2. GPU0 and GPU1 sit behind **one** switch. The extra partitions, their incoming shuffle, and their output all land on switch A, which slows that pair further.
  3. Hash skew inside BUILD_PROBE (P=N, one partition per GPU, no rebalancing).
  4. RR scan binding ignores per-task cost (selectivity, pruning, decode cost) and live progress. The `_numa_affinity` cursor is shared across all pipelines and queries, so the phase relative to GPU 0 is arbitrary.

  Each barrier adds `max_gpu(busy) - mean_gpu(busy)` of idle time on the other GPUs. At N=1 this term is 0.

**Queries hit.** Every query with large joins or merges. It shows most where P is in 5-11 (mid-size grouped merges, STANDARD joins with 5-11 GB builds) and in BUILD_PROBE joins with skewed keys.

**nsys signature.**
- Per-GPU CUDA HW rows: GPU2/3 go idle while GPU0/1 still run kernels at the end of each process-wide NVTX `Pipeline N: X -> Y` range (`sirius_pipeline.cpp:462-475`).
- Repeating "staircase" tails at every pipeline boundary.
- GPU0 has more total kernel time than GPU3.

**Quent signature.**
- Per pipeline, the spread of the last `finalizing` timestamp per `routing.preferred_device_id` (device) is large.
- Task-count and busy-time histograms per device per pipeline are skewed toward devices 0/1.
- ExecutorThread rows of some GPUs sit idle while the TaskQueue for the same query is non-empty (`executor_awaiting_task`).

**Cheap experiments.**
1. Quent post-processing, no rerun: for each pipeline, compute `sum_dev(busy) / (N * max_dev(busy))` (barrier efficiency) and sum `(max - mean)` over pipelines per query. That is the straggler tax in seconds.
2. Config: `hash_partition_bytes: 256MB` at 4 GPUs (P x4, so the ceil(P/N) inflation drops to 1.0-1.1x). Compare per-query results, and also at 1 GPU to separate out the per-task overhead change.
3. If (2) helps: a code-side follow-up would rotate the placement start by operator_id, as `select_gpu_subset` already does for single-partition joins, and make P a multiple of N.

**Prior: high.**

---

## H3 (medium-high, design): demand-driven activation, source-first priority, and FULL barriers serialize H2D-bound and compute/shuffle-bound phases instead of overlapping them

**Mechanism.**
- Only `scans.front()` is scheduled at query start (`task_scheduler.cpp:209-221`). Other scans start only when a hint chain reaches them (`task_creator.cpp:314-344`). Lookahead (`schedule_lookahead`, `task_creator.cpp:528-590`) is compiled out by `strategy=active` (`config.hpp:101`).
- Within a branch the scan head gets the lowest priority value (`task_creator.cpp:233-238`, `priority=source`). The per-device pop always takes the lowest value first (`task_scheduler.cpp:318`).
- A creation worker drains a whole scan into the queue in one loop (`task_creator.cpp:698-863`). So each GPU runs its whole backlog of scan tasks before any downstream PARTITION or CONCAT task for that branch, and FULL barriers (H2) force the same order for build sides.
- Result: the timeline alternates between "all uplinks saturated with H2D, SMs underused" and "partition, shuffle, build, probe with H2D idle".

  With N GPUs, the H2D phases shrink by at most 2x (uplink ceiling) and the compute phases by up to 4x. Because the phases add instead of overlapping, the slow part is fully exposed. Overlap would hide the compute and shuffle under the H2D stream on idle SMs and copy engines. Note that cross-switch shuffle competes for the same uplinks, so overlap mainly helps same-switch and on-GPU work.

**Queries hit.** Multi-scan join queries: Q2, Q3, Q5, Q7, Q8, Q9, Q10, Q11, Q13, Q16, Q18, Q20, Q21, Q22.

**nsys signature.**
- Per-GPU HtoD throughput: long windows at about 26 GB/s per GPU alternating with windows near 0 GB/s.
- Kernel density is anti-correlated with HtoD activity.
- Process-wide pipeline NVTX ranges of the build-side scan and the probe-side scan do not overlap.

**Quent signature.**
- Gantt by operator type: GPU_SCAN tasks of the probe pipeline start only after the build-side PARTITION/CONCAT pipelines finish.
- TaskQueue depth is dominated by one pipeline at a time.
- `preparing` (H2D) and `computing` intervals across a GPU's 4 executor threads do not overlap across pipelines.

**Cheap experiments.**
1. Quent-only overlap metric: per GPU, the fraction of wall time where at least one GPU_SCAN task is in `preparing` while a non-scan task is `computing`.
2. Config: `enable_runtime_size_estimation: true` (turns grouped-merge partition input into a PARTIAL barrier).
3. Smallest code probe, in a scratch worktree: set `task_creator_config.strategy = lookahead` or `priority = sink` (`creator/config.hpp:101,106`) and rerun g4 on Q3, Q9, Q18, Q21.

**Prior: medium-high.** That serialization exists is certain. How much of the gap it explains depends on the measured overlap.

---

## H4 (medium, config): join thresholds push 4-GPU joins into shuffle or STANDARD mode

**Mechanism.**
- `max_build_hash_table_bytes: 2GB` is set explicitly. The derived default would be 8 x `hash_partition_bytes` = 8GB (`sirius_config.cpp:1017-1020`). BUILD_PROBE needs about `rows/P * 16B < 2GB`, so builds over about 512M rows at 4 GPUs (or 134M rows when broadcast) fall back to STANDARD. STANDARD then uses natural P with RR placement (H2 imbalance) and a per-partition probe x build cross schedule, which re-hashes the build per probe batch.
- `max_broadcast_join_size` is left at 256MB (`sirius_config.hpp:67`). Broadcast copies (N-1) x build. Shuffle copies (N-1)/N x (build + probe). Broadcast wins whenever build < probe/N, which describes nearly every TPC-H fact-dimension join. The 256MB cap blocks it.
- `hash_partition_bytes: 1GB` sets coarse P (see H2).

**Queries hit.** Q3, Q5, Q7, Q8, Q10, Q12, Q13 (orders/customer builds of 0.3-3 GB), Q9 (partsupp), Q21.

**nsys and Quent signatures.**
- Same as H1 and H2.
- Sirius DEBUG log line `sirius_physical_hash_join id .. partition strategy: P partitions .. Join Mode: STANDARD|BUILD_PROBE [broadcast] .. placement [...]` (`sirius_physical_hash_join.cpp:995-1011`). Run with `SIRIUS_LOG_LEVEL=debug` on one query to list the join modes.

**Cheap experiment.** g4 run with `max_broadcast_join_size: 2GB` and `max_build_hash_table_bytes` removed (8GB derived), compared per query against g4_a. Also run g1 with the same config to confirm the gain is multi-GPU specific.

**Prior: medium.**

---

## H5 (medium, config/topology): the GPU subset for N=2 shares one switch; placement ignores switch topology at N=4

**Mechanism.** `topology.num_gpus: 2` takes the prefix [0,1] (`multi-gpu-architecture.md:254-256`; `sirius_config.cpp:495-535`). Those GPUs share one Gen5 uplink, so the H2D ceiling equals a single GPU. That alone explains the 2-GPU result (1.17x marginal). At N=4, nothing in placement (`partition_placement::round_robin`, `numa_affinity`) knows that {0,1} and {2,3} are pairs. Shuffle partners are chosen uniformly, so 2/3 of shuffle bytes cross switches (H1), and P mod N extras pile onto switch A (H2).

**Signatures.** Same as H1. Additionally, nsys shows per-GPU HtoD of about 26 GB/s at N=4 and about 26 GB/s per GPU for g2s versus 51 GB/s for g2x.

**Cheap experiment.** g2s ([0,1]) vs g2x ([0,2]), already in `configs/`. Expect g2x of about 1.6-1.9x against g2s at about 1.15x. If confirmed, the 2-GPU row in the table measures the hardware topology, not the engine.

**Prior: high that it explains the 2-GPU row. Medium as an engine design issue at N=4.**

---

## H6 (low-medium, mechanical): the scheduling fabric is fixed-size and partly blocking while demand is 4x

**Mechanism.**
- (a) 4 creation workers and 1 manager are global (`task_creator.cpp:377-381`). A worker serving a scan blocks in `split_connector::get_next_split` (`split_connector.cpp:90-93`) **while holding the pipeline's task-creation lock** (`task_creator.cpp:699-700`). Several requests for the same unfinished scan can tie up several of the 4 slots.
- (b) One scheduler thread does the matching (dynamic_casts and telemetry) for 16 executor threads.
- (c) Each GPU's executor manager loop is serial. `make_reservation` can block it (`gpu_pipeline_executor.cpp:229-249`), and while it does that GPU dispatches nothing.
- (d) An OOM or launch-error reschedule sleeps **50 ms on an executor thread** (`gpu_pipeline_executor.cpp:540-543`). OOM is possible without any downgrade.
- (e) Every completed task re-schedules every consumer (`:575-584`). This floods the creation queue with hint walks that take `op_state_mutex` and exchange locks.

**nsys signature.**
- OS-runtime rows: `task_creator*` threads in `pthread_cond_wait` / `futex` for long stretches while GPU kernel rows have gaps.
- `gpu-N-exec-manager` threads blocked in reservation.
- `std::this_thread::sleep_for` (nanosleep) of 50 ms on executor threads.

**Quent signature.**
- Large `queued` to `routing` latency while executors are idle.
- `reserving` state durations above 1 ms.
- Tasks with `finalizing.success=false` (reschedules).
- `failed_to_create_task` storms.

**Cheap experiments.**
1. Set `task_creator.num_threads: 16` and `pipeline.num_threads: 8` (g4) and compare.
2. `grep -c "reschedule (retry\|OOM at operator\|clamping reservation"` in the run log.

**Prior: low-medium.** Tasks are tens to hundreds of ms long, so fabric latency matters only in bursts at barriers.

---

## H7 (low, design): single-GPU serial tails

**Mechanism.** These steps run as one task on one GPU:
- MERGE_AGGREGATE (ungrouped final) drains all partials in one task (`task-creator.md` MERGE_AGGREGATE).
- TOP_N_MERGE, and MERGE_SORT per partition. Sort partitions are sized by memory (0.33 of GPU), so P is 1 for TPC-H ORDER BYs.
- Grouped merge and STANDARD joins with input < N*16MB use `unpinned(1)`, so one partition.
- Result collection.

In TPC-H these inputs are small: Q1 has 4 groups, Q6 is scalar, and the ORDER BY+LIMIT queries produce small outputs. Q18's HAVING output is small too.

**Signatures.**
- nsys: at the end of each query, a single GPU active for a few ms.
- Quent: the last pipeline has 1 task.

**Experiment.** In Quent, measure for each query the time from the last multi-GPU pipeline end to query end. Expected to be well under 5% of query time at SF1000.

**Prior: low.** This is consistent with the fit's small intercept a4 of about 0.36 s total.

---

## Not factors (checked)

- Admission: `gpus_per_query=0` and `admission_bytes_per_gpu=0`, so every query uses all 4 GPUs.
- `scan_manager.pipeline_width = pipeline.num_threads` (4, not x N) (`sirius_config.cpp:1038`). This only budgets file readahead (`scan_manager/config.hpp:305`) and does not apply to host-pinned cached scans.
- Executor CPU affinity is per-GPU NUMA cores (`task_scheduler.cpp:86`). With a single NUMA node all share the same cores, which is fine.

## Suggested measurement order

1. Quent post-processing on existing g1 and g4 telemetry, no reruns: H1 bytes_to_materialize per query, H2 barrier efficiency per pipeline, H3 overlap fraction.
2. g2s vs g2x (H5), already planned.
3. g4 config flips:
   - a. `max_broadcast_join_size: 2GB` + `max_build_hash_table_bytes` removed (H1, H4)
   - b. `hash_partition_bytes: 256MB` (H2)
   - c. `enable_runtime_size_estimation: true` (H3)
   - d. `task_creator.num_threads: 16` (H6)
4. A single-query nsys run of Q10 and Q18 at g4: `host_staging` vs `peer_copy` NVTX, PtoP bytes, HtoD gaps.
