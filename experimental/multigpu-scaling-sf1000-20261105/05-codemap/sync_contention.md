# Code map: synchronization and host-side contention (area = sync_contention)

Read-only code and doc analysis. Nothing was run on a GPU. Setup: TPC-H SF1000, HOST-pinned tables
(Simpatico compression on lineitem/orders), 4x RTX PRO 6000, one NUMA node, and two PCIe switches
(GPU0+1 behind one, GPU2+3 behind the other). Per-pool config: `pipeline.num_threads=4` per GPU and
`task_creator.num_threads=4` for the whole process.

## Bottom line

The host link sets a hard ceiling of about 2x at 4 GPUs (105 vs 52 GB/s). The measured 1.63x leaves
about 20% unexplained. Five code-level mechanisms in this area fit the shortfall; the first three are
the strongest:

1. **H1: one small-pinned slab pool and mutex for all 4 GPUs** (single NUMA node). cuDF's
   `device_scalar` constructor allocates from that pool **and synchronizes its stream**. A recycled
   slab makes the stream wait, via `cudaStreamWaitEvent`, on another GPU's in-flight work. The
   result is host-blocking stalls that cross GPUs, on every filter, gather, valid-count and offsets
   call.
2. **H2: placement is static and binding, with no work stealing.** Host-pinned scan tasks on a
   single-NUMA host get a round-robin GPU at creation time. The scheduler treats every preference
   as binding. Each pipeline barrier then waits for the slowest GPU, and that tail grows with N.
3. **H3: synchronous upload/decompress in the task thread, with only 4 threads per GPU.** The
   background prefetcher is disabled by design whenever more than one GPU is eligible. At 4 GPUs
   the per-GPU link drops to 26 GB/s, so `preparing` time per task doubles while compute per task
   stays the same. The 4 threads can no longer keep the SMs busy.
4. **H4:** cross-GPU shuffle clones are synchronous, run one batch at a time, and grow as (N-1)/N of
   partitioned bytes. Cross-switch copies share the same uplinks as the scan uploads.
5. **H5:** fixed per-query CPU work and scheduling-handoff latency do not scale with GPU count
   (Amdahl).

Smaller items: H6 calls `cudaGetDeviceProperties` on every JIT decode kernel launch. H7 uses
`cudaMemcpySrcAccessOrderDuringApiCall` on the uncompressed host-to-GPU path. A separate config
finding, outside this area (H8): `num_gpus: 2` selects GPU0+1, which share one switch uplink. That
explains the 1.16x at 2 GPUs.

Ruled out or unlikely (details at the end): the uring reactor (not on the cached-host path), the
cuda_async pool release threshold, per-GPU reservation-adaptor locks, the downgrade executor (no
downgrades), the Quent channel, and `cudaSetDevice` churn.

A discriminator that works for every hypothesis: in Quent, per-task **`preparing`** time absorbs
link bandwidth effects (H3/H4/H7). Per-task **`computing`** time for the *same pipeline and batch
size* should not change from 1 to 4 GPUs. If `computing` inflates at 4 GPUs, the cause is
contention or cross-GPU waiting (H1/H6), not bandwidth.

---

## H1: one small-pinned pool shared by all GPUs links their streams together (host-blocking)

**Mechanism.**
- `SiriusContext` installs one `small_pinned_host_memory_resource` **per host memory space (NUMA
  node)** as cuDF's global pinned memory resource, with threshold 8 KiB
  (`src/sirius_context.cpp:1056-1083`). With one NUMA node there is exactly **one pool for all 4
  GPUs**.
- `numa_small_pinned_mr` adds a second process-global `map_mutex_` on every allocate and free
  (`src/memory/numa_small_pinned_mr.hpp:93,106,173`).
- `allocate` takes the pool-wide `mutex_` and pops the **most recently freed** slab (LIFO). If that
  slab carries a `ready_event` from its previous free, it calls `cudaStreamWaitEvent(stream,
  slab.ready_event)` across devices
  (`cucascade/src/memory/small_pinned_host_memory_resource.cpp:166-185`).
- `deallocate` records an event on the freeing stream under the same global mutex. On first use for
  a device it also calls `cudaSetDevice` and `cudaEventCreate` (`:190-225`, `acquire_event_locked`
  `:96-130`). That event captures **all** work already queued on the freeing stream, not only the
  copy that used the slab.
- cuDF 26.10 `cudf::detail::device_scalar` builds its bounce buffer with
  `make_pinned_vector<T>(1, stream)` (`.pixi/envs/default/include/cudf/detail/device_scalar.hpp:46-92`).
  That call is `make_pinned_vector_async` **followed by `sync_stream(stream)`**
  (`.../detail/utilities/vector_factories.hpp:522-527`).
- So every `device_scalar` constructor on GPU B can (a) inherit a wait on GPU A's latest queued
  kernel and (b) block its CPU thread until that kernel finishes. Every 4-8 B slab falls into
  size class 0, so all threads compete for one free list. At 16 workers (4 GPUs x 4), roughly 3/4
  of reused slabs come from another GPU.
- Users in libcudf headers: `copy_if` (`detail/algorithms/copy_if.cuh:56,114`), `valid_if`
  (`detail/valid_if.cuh:94`), `sizes_to_offsets` (`detail/sizes_to_offsets_iterator.cuh:270`),
  `gather` (`make_host_vector` below threshold, `detail/gather.cuh:578,608`), `get_value`, and
  reductions. These cover filters, gathers, null-count computation and string offsets, which run in
  almost every operator.
- At 1 GPU the same mechanism only links the 4 streams of one busy GPU, which costs little. At 4
  GPUs it makes a thread on GPU B idle while GPU A computes, so this loss is specific to scaling.

**Hits:** every operator that filters, gathers or builds strings or null masks. Expect the largest
effect on filter-heavy, wide-output scans (Q1, Q6, Q12, Q14, Q19) and on joins with many gathers
(Q9, Q18, Q21).

**nsys signature.**
- Trace with `--trace=cuda,nvtx,osrt --cudabacktrace=all` (or `--backtrace=dwarf` with sampling).
- Look for `cudaStreamWaitEvent` immediately followed by `cudaStreamSynchronize` on executor
  threads, with backtraces through `small_pinned_host_memory_resource::allocate` and
  `cudf::detail::device_scalar::device_scalar`.
- At 4 GPUs, the `cudaStreamSynchronize` durations on those call stacks get a long tail (ms, close
  to a kernel's length on another device). The tail is absent or short at 1 GPU.
- Correlation check: the end of that sync lines up with the end of a kernel on a different GPU's
  row.
- In the CUDA API summary, count `cudaEventRecord` and `cudaStreamWaitEvent` per task. Counts scale
  with tasks; durations grow with N.

**Quent signature.** `computing` time per task for the same pipeline and batch size grows from 1
to 4 GPUs, while CUPTI kernel time per task stays flat. Executor threads sit in `computing` while
their GPU row is idle.

**Cheap experiment (one-line rebuild).** Pick one:
- Key the slab pools by **CUDA device** instead of NUMA node: change `node_for_current_device()`
  to return `dev`, and build one `small_pinned_host_memory_resource` per GPU in
  `sirius_context.cpp:1060-1073`.
- Or, in `small_pinned_host_memory_resource::allocate`, skip any free slab whose
  `event_device != device_of_stream(stream)`.

Rerun the 4-GPU warm suite. Expected result: 4-GPU time drops and the inflation in `computing` per
task disappears. A useful control is the 1-GPU run, which should barely change.

**Prior likelihood: high** that the mechanism is active. Its size is the open question for
measurement.

---

## H2: static, binding, round-robin placement with no work stealing; barrier tails grow with N

**Mechanism.**
- For a cached HOST-tier scan input, the creator sets `preferred_device_id =
  _numa_affinity.next_gpu(host, gpus_of(host))`. This is a process-global round-robin cursor across
  all 4 GPUs on the single NUMA node (`src/creator/task_creator.cpp:796-812`;
  `src/memory/numa_affinity.hpp`). Host-sourced pipelineable inputs do the same (`:766-775`).
- The provider does not stamp a device for host chunks
  (`src/scan_manager/sirius_scan_manager.cpp:281-310,333-358`).
- The `active` strategy loops `while (!node->all_ports_empty())` and creates a task for **every
  split as soon as it arrives** (`task_creator.cpp:697-866`). The scan's `all_ports_empty` is "the
  connector is closed" (`sirius_gpu_scan_operator.cpp:678`). So the whole scan is pre-assigned
  round-robin before execution.
- The scheduler dispatches to device X only tasks that prefer X or have no preference: "all
  preferences are treated as binding" (`src/pipeline/task_scheduler.cpp:303-325`).
- On a single-NUMA host, HOST-tier data is equally close to every GPU, so the binding brings no
  locality benefit. It only stops an idle GPU from taking queued work. Downstream, data locality by
  bytes (`:752-760`) and round-robin partition placement (`op/partition_placement.hpp`, hash join
  `:840-910`) also bind.
- Any per-GPU variance becomes tail time at each pipeline barrier (hash-join build, then probe;
  aggregate, then merge). Sources of variance include decode selectivity (dynamic filters and zone
  maps differ per chunk), switch contention, a GPU that also owns a heavier join partition, and P
  mod N partition imbalance. The expected maximum over N GPUs grows with N, and a ~1 s query has
  several barriers.

**Hits:** every query. Worst for multi-stage queries (Q9, Q18, Q21, Q2, Q5, Q7, Q8) and for queries
with selective pushdown on lineitem/orders, where per-chunk cost varies.

**nsys signature.** Per pipeline (NVTX ranges and the kernel rows per GPU), GPUs go idle at
staggered times before the next pipeline's first kernels. Per-GPU idle fraction rises at 4 GPUs.
The scheduler thread (`task-scheduler-thread`) shows wakeups with no dispatch.

**Quent signature.**
- Per pipeline, the spread of `max(task end)` minus `min(task end)` across GPUs.
- Tasks waiting in `queued`/`routing` for a specific device while another GPU's executor threads
  are idle.
- `query_event_publisher::publish_executor_awaiting_task` fires when the queue is non-empty but
  nothing matches the ready device (`task_scheduler.cpp:327-330`). A subscriber or log count of
  this event is direct evidence.

**Cheap experiment.** In `task_creator.cpp:773-774` and `:810-811`, leave `preferred_device_id`
unset for HOST-tier inputs when `gpus_of(host).size() == topology.num_gpus`. Unpreferred tasks are
already handed to whichever GPU becomes ready first (`task_scheduler.cpp:319-324`).

Expect the per-pipeline tail spread to shrink and total 4-GPU time to drop. Downstream binding
still forces a copy when needed, which is correct.

**Prior likelihood: medium-high.**

---

## H3: synchronous prepare and synchronous operators with only 4 threads per GPU cannot hide the halved per-GPU link

**Mechanism.**
- Each GPU executor has a bounded pool of `num_threads` (4) workers and that many streams
  (`src/pipeline/gpu_pipeline_executor.cpp:57`).
- A task runs `prepare_for_processing` synchronously on its worker thread, then
  `stream.sync()` (`src/pipeline/gpu_pipeline_task.cpp:735-738`).
- Uncompressed host chunks use `convert_host_fast_to_gpu`: a pre-sync, then a batched copy, then a
  final sync (`cucascade/src/cudf/representation_converter_builtins.cpp:1185-1245`).
- Compressed chunks use `decompress_host_to_gpu`. It runs per-1 MiB `cudaMemcpyAsync` calls
  (`src/compression/compressed_representation.cpp:62-85`) and then Simpatico decode, which "decompress_scan_filter
  synchronizes on every return path" (`src/compression/compressed_scan.cpp:427`).
- `run_one_operator` then calls `cudaStreamSynchronize` after **every** operator
  (`gpu_pipeline_task.cpp:337`).
- So a worker thread alternates between "waiting on the PCIe link" and "launch, then sync", and
  the GPU sees at most 4 concurrent streams.
- The only async upload/compute overlap, `memory_prefetcher`, is refused whenever more than one
  GPU is eligible (`src/scan_manager/sirius_scan_manager.cpp:2128-2167`;
  `memory_prefetcher.cpp:45-48`; docs `configuration.md:562-566`). It is also not enabled in this
  config.
- At 4 GPUs the per-GPU H2D rate falls from 52 to 26 GB/s (measured), so `preparing` per task
  roughly doubles while `computing` per task stays the same. The share of time when all 4 threads
  of a GPU are in `preparing` (SMs idle) rises. The suite therefore lands below even the 2x link
  ceiling, because compute no longer overlaps transfers.

**Hits:** scan-dominated queries over lineitem/orders (Q1, Q3, Q4, Q5, Q6, Q7, Q8, Q9, Q10, Q12,
Q14, Q15, Q18, Q19, Q21).

**nsys signature.**
- Per GPU: H2D memcpy rows busy at about 26 GB/s while the kernel row has gaps.
- NVTX `convert:host_to_gpu` and `sirius::compression::host_to_gpu` ranges get about 2x longer at
  4 GPUs.
- Executor threads spend most time in `cudaStreamSynchronize`. Concurrent kernel streams per GPU
  stay at 4 or fewer.

**Quent signature.** `preparing` per scan task rises about 2x from 1 to 4 GPUs, while `computing`
stays flat (if H1 is fixed). Count the intervals when all executor threads of a GPU are in
`preparing` at once.

**Cheap experiment (config only).** Sweep `executor.pipeline.num_threads` over 4, 8 and 12 at 1 and
at 4 GPUs. If the 4-GPU suite improves noticeably more than the 1-GPU suite, the loss is
overlap-limited. Also try `memory.host.block_size: 64Mi` with `pool_size: 8`, which the GB300
profile found cut per-segment submission cost (`configuration.md:1066-1091`). The compressed path's
`copy_pinned_blocks_to_device` issues one `cudaMemcpyAsync` per block.

**Prior likelihood: high.**

---

## H4: cross-GPU shuffle clones are synchronous, one batch at a time, and grow as (N-1)/N while sharing switch uplinks with H2D

**Mechanism.**
- Partitioned operators place partition p on `active_gpu_ids[p % N]`. Each producing GPU's
  partition output for p must be cloned to p's GPU.
- `lock_and_prepare_batch` clones each foreign batch inside `prepare_for_processing`
  (`src/pipeline/batch_lock_utils.hpp:129-137`).
- `convert_gpu_to_gpu` syncs the caller's stream, makes the target GPU current, waits for the
  writer event, copies peer-to-peer column by column, and then calls `target_stream.sync()`
  (`representation_converter_builtins.cpp:892-958`).
- The fraction of shuffled bytes that cross a device is 0 at 1 GPU, 1/2 at 2 GPUs and 3/4 at 4
  GPUs. Copies between switches (0<->2, 1<->3, ...) go up one switch uplink and down the other,
  sharing the same 52 GB/s uplinks that the scan H2D already saturates.
- Clones run serially per batch, inside the task thread, which compounds H3.
- If a source batch has no writer event, the code runs a `cudaDeviceSynchronize` on the **source**
  device (`cucascade/src/cudf/gpu_data_representation.cpp:244-253`). Task outputs record writer
  events (`src/data/data_batch_utils.hpp:193-227`), so this path should be rare here.

**Hits:** join- and aggregation-heavy queries with multi-partition exchanges (Q9, Q18, Q21, Q13,
Q3, Q10).

**nsys signature.**
- PtoP memcpy rows, or DtoH+HtoD pairs if P2P is unavailable, during join/merge pipelines.
- NVTX `convert:gpu_to_gpu:peer_copy` or `:host_staging`.
- H2D throughput dips on the GPU pair whose uplink also carries the peer traffic.
- Any `cudaDeviceSynchronize` calls from `wait_for_writer` mean the null-writer path was taken.

**Quent signature.** `preparing` of partition-consumer tasks whose input origin tier is a GPU other
than the executing one (`telemetry_fsm().preparing.origin_tier`). Bytes per task in transit grow
with N.

**Cheap experiment.**
- Compare the 1-to-4 speedup of scan-only queries (Q1, Q6) against shuffle-heavy queries (Q9, Q18,
  Q21) from the same run.
- Raise `hash_partition_bytes` (fewer partitions) or force a broadcast for medium builds, and see
  whether the 4-GPU time on those queries improves.

**Prior likelihood: medium.**

---

## H5: fixed per-query CPU work and multi-hop scheduling latency (Amdahl)

**Mechanism.** The following stay constant as GPU count grows:
- DuckDB planning, Sirius plan generation, single-threaded pipeline build
  (`architecture-overview.md` thread model).
- Scan-manager per-query prepare (MVCC mask jobs, pinned chunk statistics, provider construction).
- Teardown.
- Per pipeline barrier, the next pipeline's first task crosses about 5 thread handoffs:
  1. `~gpu_pipeline_task` notifies, and `task_creator::schedule` pushes.
  2. The single creator **manager** thread walks the hint chain serially for all GPUs
     (`task_creator.cpp:597-640`).
  3. A creator worker takes the per-pipeline `get_task_creation_lock` (`:699`).
  4. The single scheduler thread matches the task (`task_scheduler.cpp:268-375`).
  5. The executor manager thread for that GPU reserves memory, then hands to a worker
     (`gpu_pipeline_executor.cpp:113-150`).

The fixed share of each ~1 s query grows as the GPU-parallel share shrinks.

**Hits:** short queries (Q2, Q11, Q16, Q17, Q20, Q22) and multi-pipeline queries.

**nsys signature.** Every GPU idle at query start and end and between pipelines. The DuckDB main
thread or scan-manager threads are busy (osrt and sampling) while no kernels run.

**Quent signature.**
- Query span minus the union of task `computing` intervals.
- Gap from query start to the first `queued` task.
- Gap from a pipeline's last `finalizing` to the next pipeline's first `computing`.

**Cheap experiment.** Run the 22 queries at SF1 or SF10 with the same config on 1 and on 4 GPUs.
The near-constant time gives the serial floor. Fit
`T(N) = S + P/N` to the measured SF1000 totals: from 35.8 s and 22.0 s, if the link ceiling did not
exist, S would be about 17 s, which is not plausible. So S is only part of the story; measure it
directly.

**Prior likelihood: medium.**

---

## H6: `cudaGetDeviceProperties` plus source render and hash on every Simpatico JIT decode launch

**Mechanism.** Each warm decode launch goes through `launch_decode_kernel`, then
`compile_rendered` (`src/compression/simpatico_codegen/src/bridge/codegen_runtime.cpp:170-210`).
That path:
- runs `getenv`;
- calls `jit::arch_cc_for_current_device()`, which calls **`cudaGetDeviceProperties`** every time
  (`jit/nvrtc_compiler.cpp:32-41`);
- computes an XXH3 hash of the rendered source and takes a global `KernelCache::mu_` lookup
  (`jit/kernel_cache.cpp:78-90`);
- takes the per-kernel `func_mu_` (`jit/nvrtc_compiler.cpp:96-108`);
- calls `raise_shared_memory_limit` and `cuLaunchKernel`.

The source is re-rendered per launch (`codegen_runtime.cpp:639-651`). All of this is CPU time on
the worker thread inside a decode that already serializes on syncs. `cudaGetDeviceProperties` is a
known slow call that queries many attributes, and it may take a driver lock.

**Hits:** every compressed lineitem/orders chunk decode.

**nsys signature.** In the CUDA API summary, a high `cudaGetDeviceProperties_v2` count (about one
per decode launch) and its total time. Per-call time rising at 4 GPUs would point to a driver lock.

**Quent signature.** Higher `preparing` CPU time on compressed scans that does not match H2D bytes.

**Cheap experiment.** Cache the compute capability per device in a static array (a 3-line patch),
or measure first: the nsys API summary gives the total directly.

**Prior likelihood: low-medium.**

---

## H7: the uncompressed host-to-GPU batch copy uses `cudaMemcpySrcAccessOrderDuringApiCall`

**Mechanism.** `convert_host_fast_to_gpu` flushes its `cudaMemcpyBatchAsync` with
`cudaMemcpySrcAccessOrderDuringApiCall` (`representation_converter_builtins.cpp:1234`; flush at
`:320-345`). The code comment's intent is "the source is ready". The flag's contract is stronger:
all source reads must finish before the API returns. The driver can meet that only by blocking for
the whole transfer or by staging through internal buffers, which is pageable-style: CPU memcpy plus
DMA, about 3x the host DRAM traffic. With 4 GPUs uploading at once, this can contend on host memory
bandwidth or driver staging. It affects uncompressed pinned tables: part, partsupp, customer,
supplier.

**nsys signature.** `cudaMemcpyBatchAsync` API duration is close to the transfer time (in other
words, synchronous). H2D rows show many small staged chunks, or the label "Pageable" instead of
"Pinned".

**Quent signature.** `preparing` for partsupp/part/customer scans scales worse than for the
compressed lineitem scans.

**Cheap experiment.** Switch to `cudaMemcpySrcAccessOrderStream`, which is correct because the
source is fully written before the call and is kept alive until `final_sync`. Compare Q2, Q11, Q16
and Q20 at 4 GPUs.

**Prior likelihood: low.**

---

## H8 (config; outside this area but decisive for 2 GPUs): `num_gpus: 2` selects GPU0+GPU1, which share one uplink

`topology.num_gpus` uses a **prefix** of the visible GPUs (`multi-gpu-architecture.md` hardware
caveats). GPUs {0,1} together get 52 GB/s, the same as one GPU (measured). That alone explains the
2-GPU result of 1.06-1.16x.

**Experiment:** `topology.gpu_ids: [0, 2]`. Expect the 2-GPU speedup to approach the 4-GPU
link-limited behaviour.

**Prior likelihood: high.**

---

## Checked and ruled out, or unlikely

- **Shared uring ioctx and reactor:** used only for fresh file reads. Cached HOST-tier scans serve
  from `pinned_entry::host_chunks` (`sirius_scan_manager.cpp:333-358`). nsys should show no reactor
  activity in warm runs.
- **cuda_async pool release churn:** pools are built as
  `cuda_async_memory_resource(memory_capacity)` (`cucascade/src/memory/memory_space.cpp:127`). In
  rmm 26.10, a missing `release_threshold` means UINT64_MAX (`rmm/mr/cuda_async_memory_resource.hpp:79-81`),
  and the pool is primed up front. No map/unmap churn, so peer-access grants add no per-allocation
  cost.
- **Reservation adaptor and stream-pool locks:** per memory space, so per GPU, never shared across
  GPUs (`reservation_aware_resource_adaptor.cpp:50-90,195`; `stream_pool.cpp:82-110`;
  `memory_space.cpp:117-119` uses a 16-stream round-robin pool).
- **Downgrade executor:** no downgrades were observed. The monitor polls every 10 ms per GPU, which
  is negligible. Only the reserve path takes the predicate lock.
- **Quent emission:** lock-free mpsc (`quent crates/channel`). NVTX capture is off by default. To
  confirm, rerun 1 and 4 GPUs with `enable_quent: false`; the scaling ratio should not change.
- **`cudaSetDevice` churn:** executor threads set the device once
  (`gpu_pipeline_executor.cpp:91-97`). The RAII guards in the converters are no-ops when the device
  already matches.
- **Simpatico per-device stream registry mutex:** reached once per (thread, device)
  (`util/stream_pool.cpp:25-40,62-78`).
- **The `cudaDeviceSynchronize` in Simpatico `count_touched_chunks`:** diagnostics only
  (`codegen_runtime.cpp:790-818`).
- **Dynamic-filter replication:** `dynfilter::replicate_devices` copies serially to each of the N
  GPUs (`dynamic_filter_publisher.cpp:1200-1215`), so it grows with N but is bounded by the filter
  size. The NVTX range duration makes it easy to check.
