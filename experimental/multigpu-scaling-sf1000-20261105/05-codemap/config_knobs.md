# Config knobs vs multi-GPU scaling (area: config_knobs)

Read-only code/doc analysis at commit bb557b1a. Nothing was run on the GPUs. Inputs:

- `specific_config.yaml`, the run config `configs/g4.yaml` (host capacity raised to 600GB)
- `bench/sf1000-repro/sirius-sf1000.yaml` (the single-GPU GB300 tuning)
- `docs/super-sirius/{configuration,multi-gpu-architecture,task-creator,scan}.md`
- `src/sirius_config.{hpp,cpp}` and the code that consumes each knob
- the existing 4-GPU smoke run `00-baseline/smoke_g4` (Q1 and Q6, two iterations each, log + Quent)

## Hard facts that frame every knob

| Fact | Source |
|---|---|
| P2P works only inside a PCIe switch: 0<->1 and 2<->3 are OK, every cross-switch pair is NS | `nvidia-smi topo -p2p r`; smoke log lines 8-15 `Pool access unavailable: owner GPU 0, accessor GPU 2` (x8) |
| Cross-switch GPU->GPU copies are host-staged in a synchronous loop. Per column buffer: D2H in `block_size` chunks, `target_stream.sync()`, H2D in chunks, `sync()` | `cucascade/src/cudf/representation_converter_builtins.cpp:589-697` (`peer_copy_into`), syncs at :665, :694; whole-table `stream.sync()` at :900 and :947 |
| One NUMA node, so there is one HOST memory space and one `fixed_size_host_memory_resource` (one mutex) for all 4 GPUs | log `numa=0` x4; `cucascade/src/memory/fixed_size_host_memory_resource.cpp:140,165,286` |
| Host->GPU ceiling: 52 GB/s for 1 GPU or for {0,1}; 102 for {0,2}; 105 for all 4 (26 per GPU). Bidirectional, 4 GPUs: 175 GB/s | `10-analysis/microbench/h2d_concurrency/results.txt` (1 GiB contiguous copies) |
| HOST-pinned SF1000 footprint is about 297 GB: lineitem 144 (compressed, 627 chunks), orders 93 (compressed, 163 chunks), part 21.6, customer 25.3, partsupp 11.2, supplier 1.4 (all four uncompressed) | smoke log `[host_pool] ... allocated=` deltas; `627/627 chunk(s) compressed`, `163/163` |
| Every `pipeline.num_threads` thread is busy for the whole scan pipeline at 4 GPUs: average concurrency 3.95-3.99 out of 4 (Q1, Q6) | Quent Task FSM, script `scratchpad/conc.py` |
| Q1 at 4 GPUs: the final `MERGE_GROUP_BY` is a single task whose `Preparing` (cross-GPU gather of 627 partials, 270 KB total) takes 140 ms and 146 ms. That is 13-15% of Q1 wall time (1.10-1.39 s). Q6 `MERGE_AGGREGATE` gathers 627 partials in 19-21 ms | Quent Task `Preparing{origin_tier:GPU}` -> `Computing{input_batch_ids:627}` |

Because host->GPU bandwidth is at most 2x at 4 GPUs, linear scaling is physically impossible for any phase that is bound by H2D transfers. The realistic target is to reach the 2x link ceiling on those phases and close to Nx on compute-bound ones. Most of the knobs below matter because they add traffic on that link or add serial and quantized work to the 4-GPU path.

## Knob inventory (scaling-relevant), current value, verdict

| Knob | Here | Default | Code path | Verdict for 4x96GB host-pinned SF1000 |
|---|---|---|---|---|
| `topology.num_gpus` / `gpu_ids` | 4 (2 = prefix {0,1}) | all | `reservation_manager_configurator.cpp:303-306` (iota prefix) | **Wrong pair for 2 GPUs** (H1) |
| `topology.gpus_per_query`, `operator_params.admission_bytes_per_gpu` | 0 / 0 | 0 / 0 | `sirius_engine.cpp:67-101` (`apply_gpu_cap` and then `gpu_ids.resize(k)`, a prefix) | Off. If turned on, the subset is chosen without regard to topology (H1) |
| `executor.pipeline.num_threads` | 4 per GPU | 4 | one `gpu_pipeline_executor` per GPU with the same config (`task_scheduler.cpp:75-107`); stream pool sized to it (`gpu_pipeline_executor.cpp:57`) | Saturated (H4) |
| `executor.task_creator.num_threads` | 4 (shared by all GPUs) | 1 | `task_creator.cpp:378`; a worker drains all ready input in one go (`:698`) | Probably inert. Creation is eager, not per-device |
| `executor.downgrade.*` | 1 thread | 1 | per GPU space | Inert (no downgrades) |
| `scan_manager.num_threads`, `uring_n_reactors`, `cache`, `readahead_*`, `preparation.*` | default | | cached-provider path skips file I/O (`scan.md:402`) | Inert for warm HOST pins |
| `scan_manager.memory_prefetcher` | off | off | gated to exactly 1 eligible GPU, `sirius_scan_manager.cpp:2152-2157` | Cannot help at 4 GPUs. Enabling it only for 1 GPU widens the gap (H9) |
| `operator_params.scan_task_batch_size` | 1GB (pin-time chunking) | ~2.15 GB (86 GB/40) | one pinned chunk = one scan task = one partial/fragment (`scan.md:402`) | Controls fragment count (H3) and quantization of small tables (H5) |
| `hash_partition_bytes` | 1GB | ~2.15 GB | `natural_num_partitions` (`partition_sizing.hpp:101-120`), `round_robin` (`partition_placement.hpp:59-70`) | Sets the partition count P. P mod 4 imbalance (H5); fragments ∝ P (H3) |
| `concat_batch_bytes` | 1GB | ~2.15 GB | CONCAT groups remote fragments into one batch | Minor. Larger means fewer join tasks but no fewer clones |
| `max_build_hash_table_bytes` | 2GB (explicit) | 8x hash_partition (8GB here) | per-GPU budget, `sirius_physical_hash_join.cpp:874-885` | **Asymmetric across GPU counts** (H8); blocks broadcast (H2) |
| `max_build_probe_resident_bytes` | unset, so 8x2.15 ≈ 17 GB | 8x batch | same | OK |
| `max_broadcast_join_size` | unset, so 256 MiB | 256 MiB | `sirius_physical_hash_join.cpp:866-869` | **Too small for SF1000 on 96 GB GPUs without cross-switch P2P** (H2) |
| `PARTITION_SMALL_TABLE_BYTES_PER_GPU` (compile-time) | 16 MB/GPU | | `sirius_config.hpp`, `partition_sizing.hpp:92-120` | Inputs from 64 MB up are forced to at least 4 partitions, so they are shuffled |
| `max_sort_partition_bytes` | 0 (33%, about 28 GB) | 0 | sort sample | Inert for TPC-H (outputs are top-N or small) |
| `enable_dynamic_filter(_multi_partition)`, `max_dynamic_filter_bloom_bytes_per_gpu` | on, on, 256 MiB | same | accumulated Bloom now has HOST fallback (`dynamic_filter_publisher.cpp:416-429`; the docs line about requiring P2P is stale) | Keep. Check publication latency (log `accumulated Bloom published through HOST staging ... ms`) |
| `memory.gpu.usage_limit_fraction` | 0.9 | 0.95 | | Fine. No spills |
| `memory.host.capacity_bytes` | 100GB (run: 600GB) | 0.9 x node RAM | | **100GB is far below the 297 GB of pins**. 600GB is fine |
| `memory.host.block_size` | 1 MiB | 1 MiB | `copy_pinned_blocks_to_device` (`compressed_representation.cpp:62-80`), `peer_copy_into` loops | One cudaMemcpyAsync per MiB (H7) |
| `memory.host.pool_size` / `initial_number_pools` | 512 / 10 (5 GB) | 128 / 4 | `expand_pool` does a cudaHostAlloc of `block_size x pool_size` under the pool mutex (`fixed_size_host_memory_resource.cpp:165,286-302`), never freed | Pin time only: about 580 expansions (lineitem pin 61 s real, 42 s sys). Warm runs: check for growth (H7) |
| `compression.enable_pin_table_compression` + `input_plan_dir` | on; only `lineitem.txt` and `orders.txt` active | off | `*_disabled.txt` for customer, part, partsupp, supplier (#1618, tuned on GB300) | **Re-enable for PCIe** (H6) |
| `telemetry.enable_quent` | true | true | ~11k Task events per Q1 run | A/B once for overhead (low) |
| log level | info | info | 1254 per-task `PARTITION/HASH_GROUP_BY ... column count mismatch` warnings in 2 Q1 runs | Hygiene. Set `sirius_log_level='error'` for timing runs |

## Ranked hypotheses

### H1. The GPU subset ignores topology. "2 GPUs" means {0,1}: one x16 uplink, so 1.0x host bandwidth (config/hardware, prior HIGH)

- **Mechanism.** `num_gpus: 2` resolves to `iota` = {0,1} (`cucascade/src/memory/reservation_manager_configurator.cpp:303-306`). {0,1} sit behind one switch, so the pair gets the same 52 GB/s as one GPU. The reported 2-GPU speedup (1.06-1.16x) is therefore about the expected ceiling for H2D-bound work, not a software failure. Per-query narrowing (`gpus_per_query`, `admission_bytes_per_gpu`) uses the same prefix rule (`sirius_engine.cpp:75,100`), so any 2-GPU admission also lands on one switch. At 4 GPUs the ceiling is 2x (105/52).
- **Hits.** Every query whose time is dominated by HOST->GPU chunk conversion (Q1, Q6, Q12, Q14, Q15, Q19 scans; lineitem/orders scans in all queries).
- **nsys.** HtoD throughput per device. Each of GPU 0 and 1 tops out near 26 GB/s in a {0,1} run but near 51 GB/s in a {0,2} run. NVTX `sirius::compression::host_to_gpu` per-byte duration about 2x longer on {0,1}.
- **Quent.** `Task.Preparing{origin_tier:HOST}` duration per input byte on scan pipelines: {0,1} about 2x {0,2}. Same task counts per device.
- **Experiment.** Change only `topology.gpu_ids: [0,2]` vs `[0,1]`, same pins, warm suite. Expect [0,2] to land near 1.6-1.9x of 1 GPU on scan-heavy queries, against 1.16x for [0,1]. Then report scaling against the bandwidth ceiling (1x / 2x / 2x), not against N.

### H2. Hash exchanges move about 3/4 of partitioned bytes across GPUs, half of them through synchronous host staging on the same uplinks the scans need. The broadcast and build-probe thresholds are too low to avoid it (design + config, prior HIGH)

- **Mechanism.** At 1 GPU no join or aggregate exchange moves bytes between devices. At 4 GPUs every exchange with P > 1 uses `round_robin` placement (`partition_placement.hpp:59-70`). Producers write P fragments locally and consumers clone the remote ones (`batch_lock_utils.hpp:129-137`, which calls `convert_gpu_to_gpu`). Of each producer's output, 1/4 stays local, 1/4 goes P2P inside the switch, and **1/2 crosses the switch through `peer_copy_into` host staging** (`representation_converter_builtins.cpp:589-697`). That path does chunked D2H, a blocking sync, chunked H2D, and another blocking sync, per column buffer, on the consumer's pipeline thread. The two legs never overlap.
  - The H2D leg uses the downstream direction of the destination switch's uplink, which is the bottleneck the scans already saturate.
  - The probe side of a non-broadcast join is shuffled the same way. That covers BUILD_PROBE with 4 partitions, since `build_probe_partitions = min(natural, num_gpus)` at `sirius_physical_hash_join.cpp:874`.
  - Broadcast, which would keep the probe local, needs `total_bytes < max_broadcast_join_size` (256 MiB default) **and** BUILD_PROBE admission with the whole build charged per GPU (`:866-885`). That means fewer than 125M build rows under `max_build_hash_table_bytes=2GB`. At SF1000 almost every build that matters is between 0.3 and 10 GB, so the large probe gets shuffled. Examples: orders⋈customer for Q3/Q5/Q10, partsupp⋈part for Q9, orders for Q18/Q21, customer/orders for Q13.
- **Hits.** Q3, Q5, Q7, Q8, Q9, Q10, Q12, Q13, Q18, Q21 (and grouped merges such as Q18's 1.5B-group aggregate). The heaviest are lineitem-probe joins and the Q18/Q21 orderkey exchanges.
- **nsys.**
  - NVTX `convert:gpu_to_gpu:host_staging` ranges, alongside `:peer_copy` for 0<->1 and 2<->3.
  - Inside them, trains of 1 MiB DtoH on the source GPU, `cudaStreamSynchronize` on a pipeline thread, then trains of 1 MiB HtoD on the destination GPU, with no overlap between legs.
  - Destination-side HtoD from staging interleaved with scan HtoD on GPU2/3 while the kernel rows show gaps.
- **Quent.** `Task.Preparing{origin_tier:GPU,target_tier:GPU}` on CONCAT, HASH_JOIN and MERGE_GROUP_BY tasks, with large summed duration per device. Sum it per query: zero at 1 GPU, large at 4 GPUs.
- **Experiment.** These settings can be changed per session.
  1. 4 GPUs: `SET max_broadcast_join_size='8GB'; SET max_build_hash_table_bytes='16GB';`. 96 GB GPUs can hold a replicated build of several GB.
  2. Run the join-heavy queries above. Compare against the same with defaults, and compare the summed GPU->GPU `Preparing` time.
  3. For a cleaner attribution, also run `gpu_ids [0,1]` against `[0,2]` on Q9/Q18/Q21: [0,1] has P2P for everything, [0,2] has host staging for everything.
  - Expected: double-digit percent gains on Q9/Q13/Q18/Q21 at 4 GPUs when shuffles become broadcasts. If the gain is small, the exchange is not on the critical path.

### H3. Per-fragment cross-GPU clone latency times fragment count, set by `scan_task_batch_size` x P. The Q1 merge gather is a serial 13-15% tail that does not exist at 1 GPU (design, config lever; prior HIGH, measured)

- **Mechanism.** The cached provider emits one pinned chunk as one split (`scan.md:402`). At `scan_task_batch_size: 1GB` lineitem has 627 chunks, so there are 627 scan tasks, 627 partial aggregates, and for an exchange 627 x P fragments. Every remote fragment is cloned separately. Each clone pays:
  - `stream.sync()` on the source (`representation_converter_builtins.cpp:900`)
  - `wait_for_writer`
  - per-column-buffer D2H, sync, H2D, sync (cross-switch)
  - a final `target_stream.sync()` (`:947`)

  The cost is latency-bound: about 25 µs per column buffer. Measured: the Q1 `MERGE_GROUP_BY` task gathers 627 batches (470 remote, 270 KB in total) in 140-146 ms of `Preparing` on one thread while the other GPUs idle. Q6 (1 column) takes 19-21 ms. At 1 GPU the same inputs are same-device and nothing is cloned.
- **Hits.** Every aggregate-ending query (Q1, Q6, Q12, Q14, Q19 and others). Every partitioned exchange: thousands of small fragments per exchange, landing as `Preparing` time on CONCAT and join tasks.
- **nsys.** In the last ~150 ms of Q1, one GPU's single thread issues thousands of tiny DtoH/HtoD/PtoP memcpys (KB-sized) separated by `cudaStreamSynchronize`, while every other device shows no kernels.
- **Quent.** A single `MERGE_GROUP_BY` / `MERGE_AGGREGATE` task with `Computing.input_batch_ids=627`, and `Preparing` much longer than `Computing`. On joins, `Preparing{GPU->GPU}` time per CONCAT task scales with the number of fragments, not bytes.
- **Experiment.**
  1. Re-pin with `scan_task_batch_size: 4GB` (157 lineitem chunks; this needs a restart and re-pin). Expect the Q1 merge `Preparing` to fall about 4x, to ~35 ms. Also watch that the scan pipelines do not get slower.
  2. Sweep 1/2/4 GB at 4 GPUs.
  - The real fix is design work, outside config: pre-merge partials per GPU before the cross-GPU gather, and pack all columns into one contiguous staged transfer per fragment.

### H4. `pipeline.num_threads=4` per GPU is the concurrency cap, and at 4 GPUs those threads also block in synchronous transfers (config, prior MEDIUM-HIGH)

- **Mechanism.** One executor per GPU with `num_threads` slots (`task_scheduler.cpp:75-107`). All four slots are busy for the whole scan pipeline: Quent average concurrency 3.95-3.99 (Q1, Q6). At 4 GPUs a slot also sits idle-blocked during:
  - HOST->GPU decode, where each GPU gets half the per-GPU bandwidth of the 1-GPU case
  - host-staged clones (H2/H3)

  So GPU compute and the link are not kept busy. The GB300 single-GPU sweep found 8 threads better (q1 -16%, q12 -14%; `configuration.md` "Tuned profile").
- **Hits.** All queries. The 4-GPU gain should be larger on join-heavy queries.
- **nsys.** Per-GPU kernel rows with gaps that line up with all 4 `gpu_pipeline-gpuN-exec-*` threads sitting in `cudaStreamSynchronize` / `cudaMemcpyAsync`. HtoD rows not continuously busy.
- **Quent.** Average concurrency per device equal to `num_threads` (saturated). `Preparing` share of task time rises at 4 GPUs.
- **Experiment.** Sweep `pipeline.num_threads` over 4/6/8/12 at 1 and 4 GPUs. Report suite time and the per-device kernel-busy fraction. Expect 8 to win at both counts, with a larger relative win at 4 GPUs if blocking in staging matters. Watch for the cliff at 12 seen on GB300.

### H5. Static, binding placement plus quantization: P mod 4, round-robin always from GPU 0, and few chunks for mid-size tables (design + config, prior MEDIUM)

- **Mechanism.**
  - Partition p goes to `gpu_ids[p % n]`, always starting at index 0 (`partition_placement.hpp:59-70`).
  - P = ceil(bytes / `hash_partition_bytes`), floored to 4 at 64 MB or more (`partition_sizing.hpp:101-120`). When P mod 4 ≠ 0, GPU0 (and GPU1, ...) gets the extra partitions in **every** exchange. For example P=5 gives a 1.6x makespan for that stage.
  - Scan tasks for HOST chunks are bound round-robin at creation time (`task_creator.cpp:811`, `_numa_affinity.next_gpu`). Creation is eager: all ready splits are created in one loop (`:698`; Quent queue wait for Q1 scan tasks is about 600 ms). Preferences are binding (`task_scheduler.cpp:318-323`), so nothing is stolen, even though HOST data is equidistant from all GPUs.
  - Mid-size tables at 1GB chunks give coarse counts: about 1-2 supplier chunks, ~11-25 for partsupp/part/customer (estimated from host bytes). That is 8-25% quantization at 4 GPUs, plus whatever falls on the GPU already carrying extra partitions.
- **Hits.** Multi-join queries with mid-size builds (Q2, Q5, Q7, Q8, Q9, Q11, Q16, Q20) and scans of part/partsupp/customer/supplier.
- **nsys.** At the end of each pipeline stage one GPU (often GPU0) is still running kernels while the other three are idle.
- **Quent.** Per pipeline, per device: the spread of `max(Finalizing)` and of summed busy time. Task counts per device (for example 2/1/1/1 on a P=5 join). With debug logging, the `partition_exchange resolved ... N partitions, mode X, placement [...]` lines (`partition_exchange.cpp:118`) give P and placement per exchange.
- **Experiment.**
  1. Run with `SET sirius_log_level='debug'` (diagnostic run only) and histogram P mod 4 across the suite.
  2. Sweep `SET hash_partition_bytes` over 256MB/512MB/1GB/2GB at 4 GPUs. Smaller values make P larger and the relative imbalance smaller, but give more fragments (H3), so there is an optimum.
  3. Code-side follow-ups (not config): round P up to a multiple of n, rotate the round-robin start by operator id, leave HOST-chunk scan tasks unpinned.

### H6. Compression is off for customer/part/partsupp/supplier. That plan set was tuned for GB300's ~350 GB/s H2D; at 26 GB/s per GPU the trade-off reverses (config, prior MEDIUM)

- **Mechanism.** `input_plan_dir` contains `customer_disabled.txt`, `part_disabled.txt`, `partsupp_disabled.txt` and `supplier_disabled.txt` (#1618, "disable compression for most tables"). These were disabled after the GB300 campaign, where decode cost exceeded transfer savings. The smoke log shows the warning `no readable nonempty plan file ... pinning uncompressed` for all four. That is about 60 GB of uncompressed pins crossing a link that gives each GPU 26 GB/s at 4 GPUs (52 at 1 GPU). Decode throughput per GPU is constant, so compression helps 4 GPUs more than 1 GPU.
- **Hits.** Q2, Q3, Q5, Q7, Q8, Q9, Q10, Q11, Q13, Q14, Q16, Q17, Q18, Q19, Q20, Q22 (the parts of each that scan those tables).
- **nsys.** HtoD bytes per query on part/partsupp/customer scan tasks. Uncompressed `convert:host_to_gpu` vs `sirius::compression::host_to_gpu` ranges.
- **Quent.** `Preparing{HOST}` time per task on those scan pipelines.
- **Experiment.** Copy the plan dir, rename the four `*_disabled.txt` to `<table>.txt`, point `input_plan_dir` at the copy, re-pin, and compare 1-GPU and 4-GPU suites. Keep only the tables that win at 4 GPUs.

### H7. Host pool geometry: `block_size=1MiB` means one cudaMemcpyAsync per MiB for compressed H2D and both staging legs; only 5 GB is preallocated (config, prior MEDIUM-LOW)

- **Mechanism.**
  - `copy_pinned_blocks_to_device` (`compressed_representation.cpp:62-80`) and `peer_copy_into` (`representation_converter_builtins.cpp:640-693`) issue one copy per host block.
  - GB300 measured about 28% of conversion time as per-segment overhead at 1 MiB, and moved to 64 MiB blocks. At 26 GB/s a 1 MiB copy takes about 40 µs, so the H2D penalty here is smaller (a few %). It still applies to both staging legs, and with 16 threads across 4 GPUs it is all CPU work.
  - The pool grows with a cudaHostAlloc of 512 MiB per expansion, under the pool mutex (`fixed_size_host_memory_resource.cpp:165,286-302`). Expansions are never released. Pins needed about 580 expansions (pin time only).
  - Warm queries start with only about 430 free 1 MiB blocks (log `free_blocks=430`), so a burst of staging in iteration 1 grows the pool. Steady state should then be stable.
- **nsys.** Histogram of HtoD/DtoH sizes peaking at 1 MiB. `cudaHostAlloc` calls inside a warm iteration, which would mean growth: a long call that stalls other CUDA API threads.
- **Quent.** `MemorySpace` HOST allocated bytes rising during warm iterations. Smoke-log equivalent: `[host_pool] ... QueryEnd allocated=` rising between iteration 2 and 3.
- **Experiment.** `block_size: 16Mi, pool_size: 32` (512 MiB pools), with `initial_number_pools` large enough that pins plus staging never expand (≈ 700). Re-pin and compare warm suites. Expect low single-digit % at 1 GPU and slightly more at 4 GPUs on the join-heavy queries.

### H8. Measurement confound: per-GPU budgets make the 1-GPU and 4-GPU runs execute different algorithms (config, prior MEDIUM)

- **Mechanism.** `max_build_hash_table_bytes=2GB` is a **per-GPU** cap on rows/partitions x 16 B (`sirius_physical_hash_join.cpp:874-885`).
  - At 1 GPU, BUILD_PROBE is admitted only for builds under 125M rows. Larger builds fall back to partitioned STANDARD mode, which rebuilds the hash table for every (partition, probe batch) task.
  - At 4 GPUs the cap rises to under 500M rows.
  - Broadcast candidacy also moves with N (`ratio >= 1.25*N`; small-table threshold 16 MB x N).

  The explicit 2GB is 4x below the default (8 x `hash_partition_bytes` = 8GB, `sirius_config.cpp:1016-1020`). It handicaps the 1-GPU baseline on orders-sized builds and flatters the 4-GPU run. The reported speedups therefore mix plan changes with scaling.
- **Quent.** Different HASH_JOIN task counts and shapes for the same query at 1 vs 4 GPUs (STANDARD: many tasks per partition; BUILD_PROBE: one build plus probes).
- **nsys.** At 1 GPU, repeated cuco build kernels per probe batch.
- **Experiment.** Run 1 GPU and 4 GPUs with `max_build_hash_table_bytes` set to 8GB and to 16GB (unset gives 8GB). Compare per-query speedups. Use the debug `partition_exchange` lines to confirm the same mode at both counts.

### H9. Memory prefetcher works for 1 GPU only (design, prior LOW-MEDIUM, as a baseline caveat)

`maybe_start_memory_prefetcher` skips when there is more than one eligible GPU (`sirius_scan_manager.cpp:2152-2157`). In `specific_config` it is off, so the comparison is fair today. The GB300 tuning turns it on, and doing that here would speed up the 1-GPU baseline only. At 4 GPUs, H2D/compute overlap comes solely from `pipeline.num_threads` (see H4). Experiment: enable it at 1 GPU and check the gain. That gain is a measure of how much a multi-GPU-capable prefetcher could recover.

### Low / inert

- `task_creator.num_threads`, `downgrade.*`, `scan_manager.{num_threads,uring_n_reactors,cache,readahead,preparation}`, `use_hw_decompression`, `max_sort_partition_bytes`: inert for warm HOST pins in TPC-H.
- Quent and logging: one A/B with `enable_quent:false` and `sirius_log_level=error`. The 627-per-run `column count mismatch` warnings should be silenced or fixed regardless.
- Fixed per-query overhead (planning, prepare_for_query, pipeline barriers) is an Amdahl term outside this area. Measure it as query wall minus the sum of pipeline spans in Quent.

## Comparison with `bench/sf1000-repro/sirius-sf1000.yaml` (GB300, 1 GPU)

| Knob | GB300 tuned | specific_config | Implication at 4 GPUs (PCIe, no cross-switch P2P) |
|---|---|---|---|
| `num_gpus` | 1 | 4 | Use `gpu_ids [0,2]` for any 2-GPU point (H1) |
| `pipeline.num_threads` | 8 | 4 | Adopt 8 (H4); sweep |
| `task_creator.num_threads` | 4 | 4 | Same |
| `scan_task_batch_size` | 8GB (a step change: q4 -25%, q12 -18%) | 1GB | 8GB at 4 GPUs gives ~80 lineitem chunks (fine) but 1-3 chunks for mid tables (bad quantization, H5) and the fewest fragments (good, H3). Sweep 2/4GB |
| `hash_partition_bytes`, `concat_batch_bytes` | default (5 GiB clamp) | 1GB | Larger P on GB300 was inert. At 4 GPUs this trades off H3 against H5 |
| `max_build_hash_table_bytes` | default (8x5 GiB = 40 GiB) | 2GB | GB300 effectively never refused BUILD_PROBE; 2GB here biases 1 vs 4 GPU (H8) and blocks broadcast (H2) |
| `host.block_size` / `pool_size` / `initial_number_pools` | 64 MiB / 8 / 392 (196 GB preallocated) | 1 MiB / 512 / 10 (5 GB) | Adopt larger blocks and preallocation (H7) |
| `host.capacity_bytes` | 471 GB | 100GB (run uses 600GB) | 100GB cannot hold the 297 GB of pins; keep at least 350-600GB |
| `gpu.downgrade_trigger/stop` | 0.9/0.85 | default 0.8/0.6 | Inert (no downgrades) |
| `memory_prefetcher` | on, 3 threads | off | Single-GPU only (H9) |
| `scan_manager.num_threads` / `uring_n_reactors` | 18 / 4 | default | Inert for HOST pins |
| disk tier | configured | none | Inert |
| `enable_quent` | false | true | A/B for overhead |
| compression plans | the #1618 set (only lineitem and orders) | same set | Re-evaluate for PCIe (H6) |

## Recommended sweep order (cheapest first, 4 GPUs unless noted)

1. `gpu_ids [0,2]` vs `[0,1]` (H1). This decides how much of the "2-GPU" failure is hardware.
2. Per-session SETs, no restart: `max_broadcast_join_size` {256MiB, 2GB, 8GB} x `max_build_hash_table_bytes` {2GB, 8GB, 16GB} on Q3/5/9/13/18/21 (H2, H8). Use `hash_partition_bytes` {256MB, 512MB, 1GB, 2GB} for H5.
3. Restart-only: `pipeline.num_threads` {4, 6, 8, 12} at 1 and 4 GPUs (H4).
4. Re-pin: `scan_task_batch_size` {1, 2, 4GB} (H3/H5), and host `block_size 16Mi/pool_size 32/initial_number_pools ~700` (H7).
5. Re-pin: re-enable compression plans for customer/part/partsupp/supplier (H6).
6. Baseline hygiene for every comparison: same `max_build_hash_table_bytes` at all GPU counts, `sirius_log_level=error`, one Quent on/off A/B.
