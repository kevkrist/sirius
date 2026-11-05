# Cross-GPU data movement & exchanges — scaling code map (xgpu_movement)

Scope: partition exchange / repartition, broadcast joins, `lock_and_prepare_batch` / `clone_to`,
cuCascade `convert_gpu_to_gpu` (peer DMA vs host staging), dynamic-filter replication, late
materialization, all-to-one gathers, result D2H. Read-only analysis at commit `bb557b1a` plus
re-analysis of the existing 4-GPU smoke Quent trace
(`00-baseline/smoke_g4/telemetry/01a1ac60-...`, Q1/Q6 warm iterations). Helper scripts:
`05-codemap/qtask.py`, `05-codemap/merge_inputs.py` (run with `python3 -I <script> <telemetry_dir>`).

## Topology facts that drive everything below

`nvidia-smi topo -p2p r` / `-p2p w` on this host (read-only query):

```
       GPU0 GPU1 GPU2 GPU3
GPU0    X    OK   NS   NS
GPU1    OK   X    NS   NS
GPU2    NS   NS   X    OK
GPU3    NS   NS   OK   X
```

- P2P is supported only inside each PCIe switch ({0,1}, {2,3}); every cross-switch pair is NS.
- cuCascade's probe (`cucascade/src/memory/common.cpp:520-545`, `probe_direction`) returns
  `UNSUPPORTED` when `cudaDeviceCanAccessPeer==0`, so `probe_peer_dma_works(src,dst)` is false for
  all 8 cross-switch directions (no stderr warning — UNSUPPORTED is not counted as "broken").
- Single NUMA node, single HOST memory space (`[host_pool] HOST:0` in the smoke log); every host
  staging buffer comes from that one `fixed_size_host_memory_resource` (one mutex).
- Uplink measurements (10-analysis/microbench/h2d_concurrency/results.txt): per switch uplink
  ~52 GB/s H2D, ~57 GB/s D2H, ~95-101 GB/s bidirectional. Full duplex: a D2H leg mostly does not
  steal from scan H2D, an H2D leg does.

Consequence for a uniform hash exchange at 4 GPUs (partition p on `gpu_ids[p % 4]`, producers on
all 4 GPUs): 1/4 of pieces are local, 1/4 go P2P under the switch (no uplink use), **1/2 are
host-staged** (D2H on the source switch uplink, then H2D on the destination switch uplink — the
same downstream direction the host-pinned scan saturates). At 2 GPUs ({0,1}) the remote half is
all P2P; at 1 GPU nothing moves.

## How a cross-GPU move happens (the one path)

1. Consumer task is pinned/located on GPU d (`task_creator.cpp:723-848`).
2. `gpu_pipeline_task::execute` → `prepare_for_processing` (`gpu_pipeline_task.cpp:735`), in the
   Quent **Preparing** state, on the executor thread of GPU d.
3. `pipelineable_operator_data::prepare_for_processing` loops over input batches **serially**
   (`sirius_physical_operator.cpp:140-190`), calling `lock_and_prepare_batch` per batch.
4. GPU→GPU mismatch → `read_accessor.clone_to<gpu_table_representation>`
   (`batch_lock_utils.hpp:137`) → cuCascade `convert_gpu_to_gpu`
   (`representation_converter_builtins.cpp:892-960`): `stream.sync()` up front (902), NVTX
   `convert:gpu_to_gpu:peer_copy|host_staging` (916-919), per-column `reconstruct_column_p2p`
   (756-880), `target_stream.sync()` at the end (947).
5. Per leaf buffer `peer_copy_into` (589-697):
   - P2P pair: one `cudaMemcpyPeerAsync`.
   - Cross-switch: allocate blocks from the HOST pool, issue 1 MiB `cudaMemcpyAsync` D2H chunks,
     **`target_stream.sync()`**, then 1 MiB H2D chunks, **`target_stream.sync()`**. D2H and H2D
     never overlap; no `cudaMemcpyBatchAsync` (unlike `src/cuda/device_transfer.cpp`).
   - Null masks: `alloc_and_peer_copy_sync` adds one more sync (725-740). String columns add an
     offsets sync + 4-byte D2H readback + sync (786-815).
   So a host-staged clone costs about 2 host syncs per fixed-width buffer and 5 per nullable one,
   plus extras for strings, all blocking the executor thread.
6. Clones get new batch ids **without a Quent DataBatch probe** (`batch_lock_utils.hpp:135`
   TODO), so in Quent they show up as unresolvable input ids (`?`) and as Preparing time plus
   `Reserving.bytes_to_materialize`.

## Ranked hypotheses

### H1 (high, measured): serial fan-in of hundreds of tiny partials, cloned one at a time with per-buffer syncs

**Mechanism.** Every scan or probe task emits its own partial batch (grouped or ungrouped
aggregate partials, top-N partials, sort runs). The final merge drains **all** of them into
**one** task:
- `sirius_physical_grouped_aggregate_merge::get_next_task_input_data`
  (`sirius_physical_grouped_aggregate_merge.cpp:214-240`)
- `sirius_physical_ungrouped_aggregate_merge::get_next_task_input_data`
  (`sirius_physical_ungrouped_aggregate.cpp:854-873`)
- `sirius_physical_top_n_merge::get_next_task_input_data` (`sirius_physical_top_n.cpp:423-438`)
- `sirius_physical_merge_sort::get_next_task_input_data` (`sirius_physical_merge_sort.cpp:55-75`)

That one task's Preparing phase clones every remote partial serially (step 3 above), with
roughly 2-5 host syncs per column buffer. Nothing pre-combines partials per GPU before the fan-in.
The cost depends on the **number** of pieces and columns, not on bytes. It is zero at 1 GPU
(every input is `lock_to_existing_batch`), small at 2 GPUs (P2P only), and largest at 4 GPUs
(3/4 remote, half of those staged). It sits on a FULL-barrier critical path (scan → PARTITION →
MERGE), so it adds directly to wall time. Partitioned merges have the same pattern once per
partition task: about `#upstream tasks × 3/4` remote pieces per partition.

**Evidence (existing 4-GPU Quent trace, warm iter2):**

| query | wall | merge task Preparing | merge compute | inputs (remote = untracked clone ids) | payload |
|---|---|---|---|---|---|
| Q1 | 1078 ms | **160.6 ms** (iter1 196 ms) | 2.3 ms | 627 (470 remote, 157 local) | 67.8 KB |
| Q6 | 449 ms | 19.4 ms | 2.1 ms | 627 (470 remote) | 2.5 KB |

Q1 loses about 15% of its wall time to moving 68 KB. Remote clones cost about 340 µs each for
Q1's roughly 12-14-buffer partial and about 41 µs for Q6's 1-2-buffer partial, which matches a
per-buffer sync cost. The scan phase before it spans about 906 ms.

**Queries hit:** every query ending in an aggregate or top-N fed by many tasks: Q1, Q6, Q14 and
Q19 (fan-in of 627 lineitem partials), plus Q3, Q4, Q5, Q7, Q8, Q9, Q10, Q12, Q18 and Q21 (fan-in
over probe-task partials and per-partition merges). A plausible total is 1-2 s of the 22 s 4-GPU
total; this needs confirmation across all 22 queries.

**nsys signature:** inside the single `Pipeline N Task … MERGE_GROUP_BY|MERGE_AGGREGATE|TOP_N_MERGE`
range on one host thread, a train of about 470 `convert:gpu_to_gpu:host_staging|peer_copy` NVTX
ranges, each containing many `cudaStreamSynchronize` calls and byte-to-KB `cudaMemcpyAsync`
DtoH/HtoD. The GPUs are idle during that window (no kernels between the last scan kernel and the
merge kernel).

**Quent signature:** the merge task's Preparing→Computing gap is much larger than its Computing
time. Its Reserving `bytes_to_materialize` is tiny (0.2 MB). Its Computing `input_batch_ids` holds
about (n-1)/n unresolvable ids.

**Experiments:**
1. Already-done check: run `05-codemap/qtask.py` on any 4-GPU telemetry and list
   `MERGE_*` Preparing ms per query.
2. Run Q1/Q6 warm with `topology.gpu_ids: [0,1]` (all P2P) and `[0,2]` (all staged). Expect merge
   Preparing of about 50 ms vs about 300+ ms (Q1).
3. Fix sketch to measure: before the fan-in, coalesce partials per GPU (one local pre-merge or
   concat per device, so 4 inputs instead of 627). Or clone all remote batches with one packed
   transfer per source GPU and a single sync.

---

### H2 (high): half of all exchange traffic is host-staged across the shared uplinks, synchronously

**Mechanism.** Cross-switch P2P is NS, so `peer_copy_into` takes the host-staging branch
(`representation_converter_builtins.cpp:598-697`) for 8 of the 12 ordered GPU pairs. Each staged
buffer is D2H → full sync → H2D → full sync on one stream, so effective per-copy bandwidth is about
`1/(1/BW_d2h + 1/BW_h2d)`, around 13-14 GB/s under the measured 4-GPU contention versus about
50 GB/s for in-switch P2P. The H2D leg uses the downstream switch uplink, which the host-pinned scan
already saturates (about 105 GB/s aggregate, the 2x ceiling). The copy runs in the consumer task's
Preparing phase and blocks one of the GPU's 4 executor threads (`pipeline.num_threads=4`;
`gpu_pipeline_executor.cpp:57`) for the whole transfer. Staging buffers come from the single
`HOST:0` pool (`fixed_size_host_memory_resource.cpp:129-196`, one mutex; `expand_pool` under that
mutex if the pool runs dry).

Per keyed exchange of D bytes at 4 GPUs, the extra downstream-uplink load is **D/2**, which
directly competes with scan H2D. The upstream leg (D/2) mostly rides the idle D2H direction (the
bidir microbench shows about 10% interference).

**Queries hit:** anything with a hash-partitioned exchange of large data: lineitem⋈orders on
orderkey (Q3, Q4, Q5, Q7, Q8, Q9, Q10, Q12, Q18, Q21), partkey/suppkey shuffles (Q2, Q9, Q11, Q16,
Q17, Q20), and high-cardinality grouped merges (Q18 on l_orderkey, Q13 on c_custkey, Q10, Q15,
Q20).

**nsys signature:** NVTX `convert:gpu_to_gpu:host_staging` ranges (cross-switch) next to
`convert:gpu_to_gpu:peer_copy` (in-switch). Inside staging, alternating Pinned DtoH and HtoD
memcpys in 1 MiB slices with `cudaStreamSynchronize` between the legs. With
`--gpu-metrics-devices=all`, PCIe RX on GPUs 2 and 3 rises during GPU 0/1→2/3 exchanges while scan
H2D throughput drops.

**Quent signature:** non-scan tasks (`origin_tier=GPU`) with `bytes_to_materialize > 0` have long
Preparing phases. The Preparing ms/GB ratio is bimodal: staged pairs run about 3-4x slower than
P2P pairs.

**Experiments:**
1. In one 4-GPU nsys trace of Q9/Q18/Q3, sum the bytes and durations of the `host_staging` vs
   `peer_copy` NVTX ranges. Exchange GB divided by the query's scan GB says how much this tax adds
   to the 2x ceiling.
2. 2-GPU A/B: `gpu_ids [0,1]` (P2P exchanges, 52 GB/s scan) vs `[0,2]` (staged exchanges,
   102 GB/s scan). Per-query differences separate scan-bound queries from exchange-bound ones.
3. Smallest code test: issue each staged column's D2H for all buffers, sync once, then issue
   all H2D legs and sync once (or double-buffered chunks with events). A merge-task Preparing drop
   confirms the sync-bound part.

---

### H3 (high, design): no key-aware placement, so (n-1)/n of every keyed operator's bytes are reshuffled, again at each key change

**Mechanism.** Host-pinned chunks go to GPUs round-robin with no key awareness (pin:
`round_robin_strategy`; scan tasks: `_numa_affinity.next_gpu` at `task_creator.cpp:773/811`, all
4 GPUs on NUMA 0). Every keyed consumer installs `partition_placement::round_robin(P, gpu_ids)`
(`partition_placement.hpp:59-71`; hash join `sirius_physical_hash_join.cpp:902-911`; grouped
merge `sirius_physical_grouped_aggregate_merge.cpp:211-215`). `sirius_physical_partition::execute`
hash-scatters each input on its current GPU (`sirius_physical_partition.cpp:257-269`), and
`gpu_partition_impl::hash_partition` deep-copies each slice into its own table
(`gpu_partition_impl.cpp:213-221`). A uniform shuffle therefore moves 0 → 1/2 → 3/4 of bytes as
GPUs go 1 → 2 → 4.

lineitem and orders are both clustered by orderkey, but nothing co-locates matching ranges, so
lineitem⋈orders and GROUP BY l_orderkey (Q18) always shuffle the full probe or partial stream.
Join chains (Q5, Q7, Q8, Q9) reshuffle on every new key. The shuffled bytes are **decompressed**,
post-filter columns, while the scan moves Simpatico-compressed bytes, so the exchange-to-scan byte
ratio is higher than row counts suggest.

**nsys signature:** `sirius_physical_partition::execute` NVTX followed by consumer-task
`convert:gpu_to_gpu:*` ranges whose total bytes are about 0.75 × bytes partitioned. The same
query at 1 GPU shows the partition ranges but no convert ranges.

**Quent signature:** per query, Σ`bytes_to_materialize` over non-scan tasks ≈ 0.75 ×
Σ`input_bytes` of PARTITION tasks at 4 GPUs, about 0.5× at 2 GPUs and 0 at 1 GPU. The Preparing
share of task time in join and merge pipelines grows with N.

**Experiments:**
1. From Quent at 1, 2 and 4 GPUs, tabulate per-query exchanged GB (`bytes_to_materialize` on
   GPU-origin tasks) against scan GB.
2. Upper-bound test of co-location: force the probe and build of lineitem⋈orders onto one GPU
   pair (`gpu_ids [0,1]`) and compare.
3. Design follow-up: range-partitioned pinning of lineitem and orders by orderkey across GPUs,
   with a "local" partition layout when the placement matches the pin.

---

### H4 (medium-high, config): broadcast rule too narrow for SF1000, so large probes get shuffled instead of small builds replicated

**Mechanism.** `compute_hash_join_partition_strategy` (`sirius_physical_hash_join.cpp:824-912`)
broadcasts only if `total_bytes < 64 MB` (`partition_small_table_bytes(4)`, 16 MB/GPU), **or**
`total_bytes < max_broadcast_join_size` (default **256 MiB**, `sirius_config.hpp:67`; not set in
our yaml) **and** card ratio ≥ 1.25·n = 5 (869). It also requires BUILD_PROBE (891), where the
broadcast per-GPU hash table must fit `max_build_hash_table_bytes`, which our yaml sets to 2 GB
(≈134M build rows) instead of the derived default of 8 × hash_partition_bytes.

Byte cost of broadcast is (n-1)·B, versus (n-1)/n·(B+P) for a shuffle. Broadcast wins whenever
P > (n-1)·B, i.e. a probe more than 3× the build at 4 GPUs. At SF1000 many builds of 0.3-4 GB
(filtered orders, customer, partsupp subsets, supplier) face 10-60 GB probes, so the 256 MiB cap
forces shuffles. Broadcast probes use `partition_mode::local`
(`sirius_physical_partition.cpp:174-188`): **zero** probe movement.

**Queries hit:** Q2, Q3, Q5, Q7, Q8, Q9, Q10, Q11, Q16, Q20 and Q21 (dimension or filtered-orders
builds against lineitem/partsupp probes).

**nsys signature:** probe-side `sirius_physical_partition::execute` hash-partition kernels and
convert ranges disappear for joins that flip to broadcast. Replaced by 3 build clones per join.

**Quent signature:** the probe-pipeline PARTITION tasks become pass-through (tiny compute), and
probe-task `bytes_to_materialize` goes to about 0.

**Experiments:** `SET max_broadcast_join_size='8GB';` (option exists,
`sirius_extension.cpp:3540`) plus `operator_params.max_build_hash_table_bytes: 8GB`. Enable debug
logging and grep `partition strategy: … [broadcast]` (`sirius_physical_hash_join.cpp:990-1009`).
Compare per-query 4-GPU time. Check memory: the broadcast build is resident per GPU.

---

### H5 (medium, config/design): partition count not a multiple of the GPU count gives a skewed consumer phase

**Mechanism.** STANDARD joins and grouped merges use `natural_num_partitions = max(ceil(bytes /
hash_partition_bytes), n)` (`partition_sizing.hpp:101-120`), placed round-robin. With P = 5, 6 or 7
at 4 GPUs, GPU 0 gets 2 partitions while the others get 1-2: the slowest GPU does
ceil(P/4)/(P/4) = up to 1.6× the mean. A grouped merge is **one task per partition**
(`grouped_aggregate_merge.cpp:214-240`), so at most P of the 16 executor threads are busy and that
tail is serialized. At 1 GPU the same P tasks share one device with no imbalance. BUILD_PROBE
uses `min(natural, n)` and is balanced. STANDARD and merges are not.

**Signatures:** in nsys, one GPU's kernel row stays busy after the others go idle at the end of
join or merge pipelines. In Quent, per-GPU (executor_thread label `gpuX`) Computing end times for
that pipeline differ by about one task length. Debug log `partition_exchange resolved … P
partitions … placement`.

**Experiment:** Collect P per exchange from debug logs and count P mod 4 ≠ 0. Re-run with
`hash_partition_bytes: 256MB` (P ≫ 4, so imbalance under 10%) and compare join/merge pipeline
spans.

---

### H6 (medium-low, mechanical): unpinned single-partition STANDARD joins re-clone the build for every probe task

**Mechanism.** If a build is under 64 MB but not BUILD_PROBE-eligible (MIXED join, FULL OUTER, no
foldable concat), the strategy is `unpinned(1)` (`sirius_physical_hash_join.cpp:902-910`). The
cross-schedule **borrows** the build batch for every (probe, build) pair
(`sirius_physical_hash_join.cpp:1371-1386`), and the task creator places each pair on the GPU with
the most input bytes, i.e. the probe's GPU (`task_creator.cpp:733-760`). The clone made in
`lock_and_prepare_batch` exists only for that task (`sirius_physical_operator.cpp:155-170`), so the
build is re-cloned per probe batch: #probe tasks × 3/4 × B bytes, half of it staged.

**Queries hit (likely):** Q17 and Q20 (equi-key plus inequality against per-key aggregates, so
MIXED, with a small build), and any MARK/MIXED join with a small build.

**Signatures:** in nsys, repeated `convert:gpu_to_gpu:*` ranges of identical size inside the join
pipeline's tasks. In Quent, join-pipeline tasks each show `bytes_to_materialize` ≈ build size.

**Experiment:** Debug-log the join's `Join Mode: STANDARD`, `1 partitions`. Run Q17/Q20 at 4 vs 1
GPU, and count convert ranges against probe tasks.

---

### H7 (medium for the 2-GPU column, config/hardware): a 2-GPU run on {0,1} gets no extra host bandwidth

**Mechanism.** GPUs 0 and 1 share one switch uplink (52 GB/s total for both). A prefix 2-GPU
topology therefore gets the same scan H2D bandwidth as 1 GPU, which matches the 1.06-1.16x 2-GPU
speedups. `[0,2]` gets 102 GB/s but host-stages every exchange (H2), which is the trade-off this
area controls.

**Experiment:** Re-run the 2-GPU column with `topology.gpu_ids: [0,2]`. Not the 4-GPU problem,
but it shows how much of the remaining 4-GPU gap is H2D ceiling vs exchange tax.

---

### H8 (low): dynamic-filter replication and accumulation go through host staging; consumers do not wait

**Mechanism.** Bloom arrays are 2 B/key, capped at 256 MiB/GPU
(`op/dynamic_filter/config.hpp:27`, `bloom_sizing.hpp`). Accumulated publication reduces partials
to a root and fans out to every replica (`sirius_dynamic_bloom_filter.cu` about 680-860). Ordinary
replication uses `enqueue_device_copy` (`src/cuda/device_transfer.cpp:84-250`), which takes the
staged route (D2H → sync → H2D → sync) for cross-switch pairs. Consumers "never wait for
publication" (`docs/super-sirius/dynamic-filters.md`), so a slow publish lets probe scans run
unfiltered and enlarges the H2/H3 shuffle volume.

**Signatures:** nsys NVTX `dynfilter::replicate_devices` (`dynamic_filter_publisher.cpp:1207`)
duration and its staged memcpys. In Quent or the log, the dynamic-filter stats show early probe
scan tasks with selectivity of about 1.0.

**Experiment:** Toggle `enable_dynamic_filter_multi_partition: false` at 4 GPUs. Compare publish
time vs the probe scan start time.

## Checked and judged negligible / not applicable

- **Late materialization:** experimental, off unless `SIRIUS_EXP_LATE_MAT=1`, and it works only
  for GPU-tier pins (`docs/super-sirius/late-materialization.md`). Inert for host-tier pins.
- **Result D2H:** RESULT_COLLECTOR tasks take 0.2-0.3 ms in the Q1/Q6 Quent data. TPC-H results
  are tiny.
- **Device-wide sync in `wait_for_writer`** (`gpu_data_representation.cpp:245-253`): happens only
  for an unknown writer. Partition outputs record their writer via `make_data_batch(…, stream)`
  (`data_batch_utils.hpp:193-205`). Confirm in nsys that the `convert:gpu_to_gpu` ranges contain
  no `cudaDeviceSynchronize`.
- **Broadcast build replication:** (n-1)·B with B < 256 MB, done once per GPU through the CONCAT
  slot. Small.
- **Sort / merge-sort gathers:** TPC-H ORDER BY inputs are post-aggregation and small. Same
  per-batch fan-in mechanism as H1 but with few inputs.

## Context observed in the trace (outside this area, for the synthesizer)

Q1 warm at 4 GPUs: the scan pipeline (627 tasks) had Σ Preparing 8.0 s and Σ Computing 6.1 s over
16 threads, which is about the 906 ms span. The executor threads are saturated, alternating a
synchronous host→GPU prepare with compute. Anything that blocks those threads (H1, H2 clones)
directly lengthens the critical path.
