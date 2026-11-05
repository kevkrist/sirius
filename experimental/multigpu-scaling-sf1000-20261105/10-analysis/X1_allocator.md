# X1: host-side driver, allocator and synchronization contention (TPC-H SF1000, host-pinned, bb557b1a)

Scope: cross-cutting. Covers why identical per-task work costs more host time at 4 GPUs, priced as **wall time on the
4-GPU suite** (gap to linear 20.04 - 8.96 = 11.08 s). Read-only; no GPU work was run.

Scripts and outputs are in `10-analysis/X1/`:

| Script | Output | What it does |
|---|---|---|
| `x1_nsys.py` | `g1.json`, `g4.json`; summarized by `x1_report.py` into `report.txt` | API totals; OSRT locks nested in each CUDA call; allocator latency vs in-flight callers; NVTX op/leaf/task attribution; stream-sync split |
| `x1_sync.py` | `sync_g1.json`, `sync_g4.json` | Classifies each stream sync by GPU work on the synced stream and by the last `cudaStreamWaitEvent` (cross-device, pooled or one-shot event) |
| `x1_collateral.py` | `collateral_g{1,4}.txt` | Non-allocator call latency vs allocator calls in flight |
| `x1_price.py`, `price_tab.py` | `price_g4*.json`, `price_g1*.json`, `price_table.txt`, `price_by_class.txt` | Wall-share pricing in 2 ms bins, link-capped |

Data: `01-nsys/nsys2_g{1,4}`, iter=2 windows only (profiled g4 total 21.78 s vs 20.04 s un-profiled), plus
`20-experiments/*/timings.csv`.

## Summary

1. **The mechanism is confirmed.** The CUDA driver's stream-ordered allocator is guarded by process-wide locks.
   - Inside `cudaMallocFromPoolAsync` and `cudaFreeAsync` at g4, `pthread_rwlock_wrlock` and `pthread_mutex_lock`
     (callchain `cuMemAllocFromPoolAsync`/`cuMemFreeAsync` <- rmm `cuda_async_view_memory_resource` <- cucascade
     reservation adaptor) total **63.3 of 78.1 thread-s** (81%). At g1 they are 2.4 of 10.0 thread-s.
   - Per-call latency depends **only on how many allocator calls are in flight**, and the curve is the same at g1 and g4:
     1 in flight 3.7/3.4 µs, 2 in flight 8.4/7.7 µs, 3-4 in flight 12.5/15.8 µs.
   - g4 just reaches higher concurrency. 50% of its calls see 5 or more callers in flight (36-76 µs); g1 never sees more
     than 4.
   - The allocator is busy (at least one caller inside) **53% of g4 wall** vs 20% at g1, with on average **3.6 of the 16
     executor threads** inside it (0.27 at g1).
2. **Price:**
   - Alloc/free at g1 per-call cost would cut the g4 suite by **~2.4 s (range 1.8-2.9 s)**, to ~17.6 s.
   - A user-space sub-allocator (near-zero driver calls) gives **~3.0 s at g4** (17.0 s), plus ~0.9-2.1 s at g1.
   - Thread-time excess is 65 thread-s, the largest single item in the +120 thread-s g1->g4 inflation (CRITIC §0). It
     is the mechanism behind CRITIC N1/M2 "per-op compute inflation".
   - Per-query pricing predicts the measured `batch4g` gains (r = 0.96) and the `thr8` regressions (r = 0.96).
3. **Where the time sits (profiled wall, total 3.18 s):**
   - Hash PARTITION: 1.62 s. That is 47%, with allocations per task 9.4 -> 18.8, and it includes the slice deep copy at
     `gpu_partition_impl.cpp:212-221`.
   - Frees at task end: 0.63 s.
   - Compressed-scan decompress: 0.56 s. Its thread cost is large (18.9 thread-s) but mostly hidden in link-bound
     phases.
   - HASH_GROUP_BY: 0.30 s. GPU_SCAN: 0.15 s.
   - Biggest queries: Q18, Q1, Q21, Q9, Q12, Q4, Q8.
4. **Other host-side contention is separate and smaller:**
   - **`cudaStreamSynchronize` +28.5 thread-s:**
     - +20 s is waiting on GPU work that is link-bound. These are compressed-scan prepare syncs waiting on the H2D copy
       stream; that is X3's domain.
     - +13.1 s is syncs that waited on a **cross-device pooled event**. This is the small-pinned slab `ready_event` from
       sync_contention H1; zero such waits at g1.
     - H1 is real but short (mean 77 µs). It is worth **~0.2-0.3 s wall**.
   - **`cuStreamSynchronize` +18 thread-s:** almost all in cross-GPU clones (`convert:gpu_to_gpu`, 286k syncs). That
     includes 5.3 s of waits on one-shot writer events from other GPUs, and 0.53 s profiled wall. This is X2's domain;
     do not add it here.
   - **Writer lock collateral:** the allocator's write lock also delays `cudaMemcpyAsync`/`cudaMemcpyBatchAsync`
     issue. Their rdlock waits go 0.46 -> 4.6 thread-s, and latency rises with allocator callers in flight
     (5.8 -> 11 µs). Not priced separately; it disappears with the allocator fix.
   - The user-space mutexes (small-pinned pool, `numa_small_pinned_mr::map_mutex_`) are negligible: 0.31 -> 0.95
     thread-s total.

## Evidence

### E1. Suite API totals (iter2 windows; `report.txt` §1)

| API | calls g1 -> g4 | thread-s g1 -> g4 | mean µs g1 -> g4 |
|---|---|---|---|
| cudaMallocFromPoolAsync | 911k -> 1.197M | 7.33 -> **42.43** | 8.0 -> 35.4 |
| cudaFreeAsync | 911k -> 1.197M | 2.66 -> **35.65** | 2.9 -> 29.8 |
| cudaStreamSynchronize | 444k -> 499k | 87.3 -> 115.9 | 197 -> 232 |
| cuStreamSynchronize | 95k -> 387k | 3.8 -> 21.8 | 40 -> 56 |
| cudaMemcpyBatchAsync | 304k -> 414k | 13.3 -> 22.2 | 44 -> 54 |
| cudaMemcpyAsync | 1.31M -> 1.66M | 6.1 -> 12.3 | 4.6 -> 7.4 |
| cudaLaunchKernel | 440k -> 503k | 3.0 -> 3.9 | 6.8 -> 7.8 (flat) |

### E2. Locks nested inside CUDA calls (OSRT on the same thread, contained in the call; `report.txt` §2)

| Enclosing call | g1 | g4 |
|---|---|---|
| cudaMallocFromPoolAsync | mutex 1.39 s + wrlock 0.15 s | **mutex 13.98 s (370k) + wrlock 19.54 s (622k)** |
| cudaFreeAsync | mutex 0.51 + rdlock 0.13 + wrlock 0.24 | **mutex 12.43 + rdlock 1.84 + wrlock 15.47** |
| cudaMemcpyAsync | rdlock 0.31 + mutex 0.26 | **rdlock 3.07 (563k)** + mutex 0.34 |
| cudaMemcpyBatchAsync | rdlock 0.15 | **rdlock 1.57 (274k)** |
| cudaStreamSynchronize | mutex 0.83 | mutex 2.47 |
| cuStreamSynchronize | mutex 0.16 | mutex 0.70 |
| cudaEventCreate/Destroy | wrlock 0.87 | wrlock 1.12 |
| none (user-space locks) | mutex 0.31 | mutex 0.95 |

The callchains (OSRT backtraces of more than 80 µs) are always `__pthread_rwlock_wrlock`/`__pthread_mutex_lock` <-
`cuMemAllocFromPoolAsync` or `cuMemFreeAsync` <- `cudaMallocFromPoolAsync` <-
`rmm::mr::cuda_async_view_memory_resource::allocate` <-
`cucascade::memory::detail::reservation_aware_resource_adaptor_impl::do_allocate_unmanaged`.

So the allocator holds the driver's write lock. Memcpy issue takes the same lock for read, which is why memcpy waits
grow 10x (E4).

### E3. Latency is a function of concurrent allocator callers only (`report.txt` §3)

| In-flight allocator calls at call start | 1 | 2 | 3-4 | 5-8 | 9-12 | 13+ |
|---|---|---|---|---|---|---|
| g1: share of calls @ mean latency | 70% @ 3.7 µs | 22% @ 8.4 | 8% @ 12.5 | 0 | 0 | 0 |
| g4: share of calls @ mean latency | 26% @ 3.4 µs | 11% @ 7.7 | 12% @ 15.8 | 14% @ 35.9 | 23% @ 59.6 | 13% @ 75.5 |

- Latency grows linearly, about 5-6 µs per extra in-flight caller under profiling. That is the signature of one
  serialized resource whose lock hand-off costs far more than the 0.7 µs uncontended call measured by the microbench.
- Un-profiled microbench (`microbench/alloc_contention/results.txt`): aggregate rate is 280-385k calls/s whatever the
  GPU count.
  - 1 GPU x 4 threads: mean 10.9 µs.
  - 4 GPUs x 4 threads: mean 56 µs.
  - 4 GPUs x 8 threads: mean 104 µs, at the same ~300k/s.
  - Pool peer-access grants make no difference.
- **Conclusion: the per-op inflation is caused by the number of threads contending process-wide, not by anything
  GPU-specific.**

### E4. Collateral, and what is *not* allocator-driven (`collateral_g4.txt`)

Latency against allocator calls in flight (0 / 1-2 / 3-5 / 6-9 / 10+):

| Call | Latency | Coupled to the allocator? |
|---|---|---|
| cudaMemcpyAsync | 5.8 / 8.2 / 11.1 / 10.9 / 10.8 µs | **Yes** |
| cudaLaunchKernel | 8.3 / 7.9 / 7.6 / 7.1 / 6.8 µs | No |
| cudaEventRecord | — | No |
| cudaStreamWaitEvent | — | No |
| nowork stream syncs | 32 / 43 / 34 / 25 / 23 µs | No |

### E5. Allocator occupancy per suite pass

| | g1 | g4 |
|---|---|---|
| allocator calls (malloc + free) | 1.82M | 2.39M |
| serialized minimum at the un-profiled aggregate ceiling | 5.4 s at 340k/s (15% of 35.8 s) | **6.2-8.6 s at 280-385k/s (31-43% of 20.04 s)** |
| nsys: wall with at least 1 caller inside | 7.25 / 36.86 s (20%) | **11.45 / 21.78 s (53%)** |
| nsys: mean callers inside | 0.27 | **3.6** |

Per query (`report.txt` §4):
- The g4 allocator is busy 82% (Q1), 86% (Q6, Q15), 81% (Q18) and 79% (Q4) of the query window, with 5.7-7.9 callers
  inside on average.
- 71-88% of their 2 ms bins run at 120k calls/s or more. That is at the profiled ceiling (about 160-170k/s, see
  fam_groupby and fam_semijoin).
- These are allocator-throughput-bound phases.

### E6. Stream-sync growth decomposed (`x1_sync.py`, suite thread-s, g1 -> g4)

| Sync class | g1 | g4 | Δ | Owner |
|---|---|---|---|---|
| cudaStreamSync, stream had work, last wait same-device (compressed-scan prepare waiting on the H2D copy stream) | 55.5 | 75.5 | +20.0 | X3 (link) |
| cudaStreamSync, cross-device **pooled** event (small-pinned slab `ready_event`), no work | 0 | 7.0 (91k, mean 77 µs, 917 of 1 ms or more) | +7.0 | **H1** |
| cudaStreamSync, cross-device pooled event, with work | 0 | 6.1 (13.6k, mean 447 µs) | +6.1 | **H1** |
| cudaStreamSync, no work, no wait (pure host overhead) | 1.23 (7.8 µs) | 2.90 (16.6 µs) | +1.7 | host |
| cudaStreamSync, other | 30.6 | 24.4 | −6.2 | — |
| cuStreamSync, cross-device one-shot writer event (clone `wait_for_writer`) | 0 | 5.3 (50.5k, mean 106 µs) | +5.3 | X2 |
| cuStreamSync, no work, no wait (mostly clone syncs: 11.5 -> 40.7 µs/call) | 0.9 | 9.4 | +8.4 | X2 |

Notes on the classification:
- "Pooled" means the event was recorded 3 or more times in the report, i.e. it comes from a recycling pool. Writer
  events are created once per representation and destroyed (`cudaEventCreate`/`Destroy`, 180k each).
- The pooled cross-device waits appear almost only under `cudaStreamSynchronize`, which is cudf's
  `detail::sync_stream`. That is the `device_scalar`/`make_pinned_vector` path in the OSRT callchains
  (`cudaStreamSynchronize <- cudf::detail::sync_stream <- make_pinned_vector <- device_scalar`). This confirms
  sync_contention H1's mechanism (`small_pinned_host_memory_resource.cpp:166-185`; single pool for one NUMA node,
  `sirius_context.cpp:1056-1083`).
- Its size is small: excess about 10 thread-s, wall share 0.36 s profiled. The pool mutex itself costs under 1 thread-s.

## Allocation sources

All device allocations take the same path: cucascade `reservation_aware_resource_adaptor`
(`cucascade/src/memory/reservation_aware_resource_adaptor.cpp:462-475` `do_allocate_unmanaged` -> `_upstream.allocate`),
then rmm `cuda_async_memory_resource` (`cucascade/src/memory/common.cpp:551-556`, the default GPU factory;
`memory_space.cpp:122-127`), then the driver.

There is no caching layer, so every cudf temporary and every output column is a driver call.

### By op-level NVTX range (suite; `report.txt` §5)

Excess is (g4 time) − (g4 calls x g1 µs/call).

| Op class | malloc g1 -> g4 | free g1 -> g4 | µs/call g1 -> g4 | excess thread-s | wall s (profiled) |
|---|---|---|---|---|---|
| op:PARTITION | 146k -> **294k** | 55k -> 130k | 5.1 -> **65.7** | **25.7** | **1.62** |
| prep: compressed host->GPU (decompress) | 447k = 447k | 386k = 386k | 5.6 -> 27.2 | 18.0 | 0.56 |
| task:other (frees of inputs/outputs at task end) | — | 234k -> **440k** | 3.6 -> 21.2 | 7.8 | 0.63 |
| op:HASH_GROUP_BY | 79k = 81k | 69k = 71k | 6.6 -> 45.5 | 5.9 | 0.30 |
| op:GPU_SCAN | 116k = 116k | 85k = 85k | 6.9 -> 25.6 | 3.8 | 0.15 |
| prep: gpu_to_gpu clones (target buffers) | 0 -> 131k | — | 11.3 | 0.8 | 0.07 |
| DYNAMIC_FILTER / FILTER / PROJECTION / UNGROUPED_AGG | 109k = 110k | 71k = 72k | 4-5 -> 18-43 | 3.6 | 0.12 |
| CONCAT / MERGE_* / HASH_JOIN / TOP_N | 11k -> 15k | — | ~8 -> ~7 | ≈0 | 0.01 |

### Leaf ranges (g4 calls; `report.txt` §6)

| Leaf range | g4 calls (g1) | What it is |
|---|---|---|
| `make_numeric_column` | 342k | cudf output/temporary columns in every op |
| `sirius::compression::host_to_gpu` | 251k | per-leaf-buffer device allocations of the decoded batch |
| `simpatico::decompress_table` | 217k + 38k | decode temporaries |
| CONCAT task (prepare) | 206k (70k) | clone target buffers plus frees of fragments |
| `table` | **165k** (91k) | the `cudf::table` deep-copy constructor; in PARTITION this is the per-partition slice copy at `src/op/partition/gpu_partition_impl.cpp:212-221` (`std::make_unique<cudf::table>(sliced_partition_views[i].select(...), stream, mr)`): one allocation per column per partition (plus masks and string children) |
| `hash_partition` | **147k** (52k) | `cudf::hash_partition` at `gpu_partition_impl.cpp:187`: reordered output table plus partition map, histogram, offsets and CUB temp |
| `convert:gpu_to_gpu:host_staging` / `:peer_copy` | 87k / 44k | one target allocation per leaf buffer in `reconstruct_column_p2p` (`cucascade/src/cudf/representation_converter_builtins.cpp:580-700`) |
| `allocate_like`, `copy_if`, `aggregate`, `exclusive_scan`, `minmax`, `cast`, `gather`, `lower_bound` | 50-67k each | cudf internals: CUB temp storage, `device_scalar`, output columns |

### Per task (`report.txt` §7; mallocs/task, frees/task, allocator ms/task)

| Task kind | g1 | g4 |
|---|---|---|
| PARTITION | 9.4 / 4.1 / 0.07 ms | **18.8 / 10.1 / 1.90 ms** (more partitions: the P≥N floor, so P x C slice copies) |
| GPU_SCAN..HASH_GROUP_BY | 92 / 87 / 1.24 ms | same counts, **8.89 ms** |
| GPU_SCAN..FILTER | 58 / 56 / 0.54 | same counts, 2.71 |
| GPU_SCAN..DYNAMIC_FILTER | 35 / 31 / 0.36 | same counts, 1.27 |
| CONCAT | 9.4 / 222 / 1.0 | 140 / 333 / 3.9 (clones) |
| MERGE_GROUP_BY..FILTER (Q18) | 40 / 1293 / 5.7 | **981 / 2234 / 100 ms** |

- Scan-side counts are identical at g1 and g4. The cost per call is what changed.
- PARTITION counts double because exchanges that pass through at N=1 partition at N=4 (CRITIC N2), and each
  partition is deep-copied.
- Sizes are not in the traces (`--cuda-memory-usage` was off). From the code:
  - Partition slices are per column of a fragment: Q18 is 36-way and lineitem probe pieces are about 3 MB/4, so mostly
    KB to sub-MB.
  - `device_scalar` and CUB temp storage are bytes to KB.
  - Decode outputs are MB-sized.
  - So **most calls are small**, which is the ideal case for a binning or arena sub-allocator.

## Pricing with arithmetic

**Method** (`x1_price.py`), g4 iter2 windows in 2 ms bins:
- For every alloc/free, excess = duration − reference. The reference is the g1 suite mean: 8.0 µs malloc, 2.9 µs
  free. The excess is signed, so the g1 self-check gives −0.07 s.
- Active threads = executor threads inside a Task NVTX range, averaged over the bin.
- Bin saving = excess_in_bin / active_threads. This is wall share: removing E thread-ns from a phase run by n threads
  shortens it by E/n.
- The saving is capped at bin x (1 − H2D_rate/110 GB/s). A phase at the two-uplink link ceiling cannot shrink. H2D
  counts both `copyKind` HtoD and the pinned->device batch copies reported as PtoP.

| Variant (profiled s, suite) | Allocator @ g1 per-call | Sub-allocator (0.5/0.3 µs) | H1 cross-dev pooled waits | Allocator + H1 | Clone cross-dev waits (X2) |
|---|---|---|---|---|---|
| Thread excess | 65.0 thread-s | — | 11.2 thread-s | — | 4.7 thread-s |
| Wall, no link cap | 4.18 | 4.51 (positive part) | — | — | — |
| Wall, hard gate (skip bins ≥85 GB/s; positive-part excess) | 3.36 | — | — | — | — |
| **Wall, soft link cap (adopted)** | **3.18** | **3.98** | **0.36** | **3.44** | **0.53** |

**Profiled to un-profiled.** CUPTI slows the allocator. The profiled phase ceiling is about 167k calls/s against about
290k/s un-profiled, a ratio of 0.57. The whole-suite ratio is 20.04/21.78 = 0.92. Adopted range is ×0.57-0.92,
central ×0.75.

- **Allocator at g1 per-call cost:** 3.18 x [0.57, 0.75, 0.92] = **1.8 / 2.4 / 2.9 s**. Counterfactual g4 suite: 20.04
  − 2.4 = **17.6 s (17.1-18.2)**, which is 2.03x instead of 1.79x.
- **Sub-allocator (driver off the hot path):** 3.98 x [0.57, 0.75, 0.92] = 2.3 / **3.0** / 3.7 s, so g4 ≈ **17.0 s**.
  - At g1 the same model gives 2.31 s profiled (link cap 55 GB/s). CUPTI adds about 3 µs per call at low contention
    (nsys 3.7 µs vs microbench 0.7 µs for one caller), so the un-profiled g1 gain is about 0.9-2.1 s.
  - Scaling with both fixed: about 34.3/17.0 = 2.0x.
- **H1 (small-pinned cross-GPU slab events):** 0.36 x 0.57-0.92 = **0.2-0.3 s**. Incremental over the allocator, it is
  3.44 − 3.18 = 0.26 profiled.
- **Where the 3.18 s sits** (`price_by_class.txt`, profiled):

  | Class | Wall s | Share |
  |---|---|---|
  | PARTITION | 1.62 | 51% |
  | task-end frees | 0.63 | 20% |
  | compressed decompress | 0.56 | 18%; 18.9 thread-s, so ≥1.2 s potential at 16 threads, but more than half is hidden behind the link |
  | HASH_GROUP_BY | 0.30 | — |
  | GPU_SCAN | 0.15 | — |
  | clones | 0.07 | — |
  | rest | 0.13 | — |

- **Per query (profiled ms):** Q18 657, Q1 500, Q21 345, Q9 273, Q4 239, Q12 228, Q8 216, Q10 191, Q5 175, Q15 172,
  Q3 133, Q14 120, Q6 93. Link-bound queries are about 0: Q17 11, Q20 13, Q19 43, Q13 14, Q2/Q11/Q16/Q22 under 10.

**Cross-checks with the experiments** (un-profiled hot means, `price_table.txt`):

- **`x_g4_batch4g`** (scan_task_batch_size 4 GB, so tasks ÷3.7 and scan and PARTITION allocation counts about ÷4):
  - g4 20.04 -> 16.91 s (−3.13); g1 35.82 -> 33.04 (−2.78).
  - Per query, Δg4 against profiled allocator price: **r = 0.96, slope 0.90.** Examples (Δ vs price): Q18 −562 vs 657,
    Q1 −358 vs 500, Q21 −416 vs 345, Q12 −285 vs 228, Q4 −242 vs 239, Q8 −259 vs 216. Link-bound Q17/Q19/Q20 move
    0-25 ms and are priced 11-43.
  - Removing about 3/4 of the calls (and most of the contention) at the central estimate gives about 1.8-2.4 s of
    batch4g's 3.13 s. The rest is fewer fragments and clones (X2) and per-task overhead, which is also visible at g1.
- **`x_g4_thr8`** (8 threads/GPU):
  - g4 +1.19 s; g1 −0.27 (noise). Regressions per query against allocator price: **r = 0.96**. Examples: Q18 +258,
    Q1 +165, Q21 +100, Q10/Q15 +96, Q4 +90.
  - More threads cannot help an allocator-throughput-bound phase. They lengthen queues (microbench 4x8: mean 104 µs at
    an unchanged ~300k/s) and add lock convoy cost.
- **Topology:** fam_groupby and fam_subq show the Q18 PARTITION(5) and Q15 PARTITION(3) phases flat from 2 to 4 GPUs.
  That is a global ceiling, not per-GPU.

**Consistency with peers (do not double count).**
- The family reports priced the allocator inside their phases: scanagg ~405, groupby 353, joinship ~195 (inside F3/F4),
  multijoin ~267 (inside F3), semijoin 476, subq 95, small ~15, for a total of about 1.8 s.
- This suite-wide 2.4 s (1.8-2.9) agrees at the low end. The difference is task-end frees and allocator time in scan
  phases that are not link-bound (Q1, Q4, Q12, Q15), which some families left as residual.
- Overlaps:
  - About 1.2 s (un-profiled) of the allocator price sits in PARTITION. That is the **same mechanism** as X2-6 ("slower
    hashing", 0.57 s) and is part of X2-3's forced-partition exposure. Do not add X2-6 to X1.
  - The clone-path sync costs (0.53 s profiled of event waits) belong to X2-2.
  - The +20 thread-s of link-bound syncs belong to X3 (X3 L5 already notes the allocator inside the H2D path is hidden
    by the saturated link, which matches: decompress is the largest allocator thread cost but only third in wall).

## Remedies

| # | Remedy | Mechanism removed | Estimated g4 suite gain | Notes |
|---|---|---|---|---|
| R1 | **User-space sub-allocator per GPU in front of `cuda_async`**, below the reservation adaptor so accounting is unchanged. | Driver global lock on the hot path | **~3.0 s (2.3-3.7)**; plus g1 −0.9 to −2.1 s | Seam already exists: `reservation_manager_configurator::set_gpu_memory_resource_factory` (`cucascade/include/cucascade/memory/reservation_manager_configurator.hpp:103`), default factory `cucascade/src/memory/common.cpp:551-556`. Options in rising order of gain: rmm `pool_memory_resource` per GPU (one mutex per GPU: roughly g1-level contention, ~2.4 s); `arena_memory_resource` (per-thread arenas for small sizes, superblocks from a global arena); thread-local `binning_memory_resource` + `fixed_size_memory_resource` bins for allocations ≤1-4 MiB with large sizes passed through. Watch stream-ordered reuse across streams (pool/arena handle it with events and per-stream free lists), fragmentation versus `capacity_bytes`, and downgrade/OOM handling (`handle_oom` retry path, `reservation_aware_resource_adaptor.cpp:440-460`). Validate with the alloc_contention microbench plus a g1/g4 suite. |
| R2 | **Fewer allocations in hash PARTITION:** hand out zero-copy views of the `hash_partition` output (fragments share ownership of the parent buffers) or one contiguous buffer per partition (`cudf::contiguous_split`), instead of P x C deep copies (`gpu_partition_impl.cpp:212-221`; also `evenly_partition` at `:251-256`). | About half of PARTITION's calls, plus a copy kernel per column | **~0.6 s** (half of PARTITION's ~1.2 s un-profiled); less if R1 lands first | Also fewer frees at task end and fewer clone buffers downstream. Avoiding forced P≥N partitioning (X2-3, CRITIC N2) removes the rest of PARTITION's allocations for those exchanges; counted in X2. |
| R3 | **Larger tasks** (`scan_task_batch_size` 4 GB) | About ÷4 allocation count in scan/PARTITION | **measured −3.13 s** (allocator part ~1.8-2.4 s) | Already measured; it is a config change. Overlaps R1: after R1, batch4g's allocator share disappears and its remaining gain is fragments/clones. |
| R4 | **Decode into one slab per batch** (sub-carve the decoded columns and simpatico temporaries) | ~30 driver calls per compressed-scan conversion (Q18: 43.9k over 1417) | ~0.4 s wall (0.56 profiled; mostly hidden behind the link) | Only matters for non-link-bound scans (Q1, Q4, Q6, Q10, Q12). Subsumed by R1. |
| R5 | **Process per GPU** (4 Sirius processes plus a cross-process exchange) | Process-wide driver lock (per process it would behave like 1 GPU x 4 threads) | ~2.4 s (like g1-level per-call cost) | Large design cost (single SQL front end, IPC shuffle). Dominated by R1. Not recommended. |
| R6 | **Keep 4 threads per GPU** until R1 lands | Allocator queueing | avoids +1.19 s (thr8 measured) | Re-evaluate `num_threads` after R1: with the allocator gone, more threads may then hide link latency. |
| R7 | **Per-GPU small-pinned pools** (or skip slabs whose event is on another device), sync_contention H1 | Cross-GPU pooled-event waits in `device_scalar` syncs | ~0.2-0.3 s | One-line keying change at `sirius_context.cpp:1060-1073` / `numa_small_pinned_mr::node_for_current_device`. |

**Combined.** R1 + R7 give about 3.2 s, so g4 ≈ 16.8 s. R1 + R3 overlap heavily: batch4g plus a sub-allocator should
land near 16.0-16.5 s, because batch4g's non-allocator gain (~0.7-1.3 s) adds to R1.

## Not pursued

- **Allocation sizes.** nsys ran without `--cuda-memory-usage`, so sizes come from code only. A size histogram needs an
  rmm `logging_resource_adaptor` or `--cuda-memory-usage=true` run, which means GPU work and was not done.
- **Identity of the driver mutex/rwlock objects.** OSRT has no lock-address arguments. The sharing claim rests on the
  callchains and on the latency coupling (E2, E4).
- **Root cause of higher per-sync host overhead on idle streams** (cudaStreamSync 7.8 -> 16.6 µs, clone
  cuStreamSync 11.5 -> 40.7 µs). It is *not* coupled to allocator concurrency (E4). It is likely driver per-context
  bookkeeping or the cross-context clone path. It falls in X2 (clones) and is small elsewhere (+1.7 thread-s).
- **No GPU experiments** (busy queue). The obvious validation is an R1 prototype, a `mr_factory_fn` returning
  `arena_memory_resource(cuda_async)`, run on the g1 and g4 suite plus `alloc_contention`.
- **CUDA driver version effects** (595.112.07): newer drivers may shard the mempool lock. Not tested.
- Housekeeping: an exploratory `sqlite3.connect` created an **empty file
  `01-nsys/nsys2_g4/nsys/rep.18.sqlite` (0 bytes)**. No such report exists for g4 and `map.csv` does not reference it.
  Delete it when convenient (this agent may not use `rm`).
