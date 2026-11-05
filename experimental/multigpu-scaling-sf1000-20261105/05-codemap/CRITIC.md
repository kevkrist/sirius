# CRITIC: completeness review of the 5 code maps + discriminating experiment plan

Scope: read-only. No GPU work was run. Existing Quent telemetry for `00-baseline/g1_a` and `00-baseline/g4_a` was
flattened on the CPU with `bin/quent_flatten.py`, giving `<scratchpad>/qf/{g1,g4}` (regenerate in about 1 min). The
analysis scripts are in this folder: `critic_work_decomp.py`, `critic_op_inflation.py` and `critic_op_vs_linkload.py`.
All numbers below are for iter4 (warm), commit bb557b1a, with `gpu_ids [0]` and `[0,1,2,3]`, and Quent on.

## 0. The decomposition the five reports did not do (most important result)

None of the readers measured where the 4-GPU time actually goes. Existing Quent data answers that directly. Thread-time
here means the sum over all executor threads of task time, from preparing to finalize.

| Thread-time (iter4, 22 queries) | g1 | g4 | delta |
|---|---|---|---|
| HOST-origin prepare (H2D + decode) | 79.6 s | 131.9 s | **+52.3 s** (1.66x) |
| GPU-origin prepare (cross-GPU clones) | 0.2 s | 25.3 s | **+25.1 s** |
| Compute (operator execute, including its stream syncs) | 52.3 s | 94.9 s | **+42.6 s** (1.81x) |
| Total | 132.1 s | 252.1 s | 1.91x |
| Occupancy = total / (wall x threads) | 0.943 | 0.805 | 1.17x loss |
| Wall | 35.0 s | 19.56 s | 1.79x |

Check: 4 x (1/1.91) x (0.805/0.943) = 1.79, which matches the measured value. **The main obstruction is work inflation
(the same query costs 1.9x the thread-time at 4 GPUs), not idle GPUs.** Idle time and stragglers account for only 1.17x.

The +42.6 s of extra compute splits as follows (per operator type, g1 to g4):

- PARTITION: 6.1 to 29.6 s.
  - **+15.9 s comes from exchanges that are pass-through at 1 GPU (P=1) but hash-partition at 4 GPUs.** These are new
    work, not slower work. Example: Q4 PARTITION(10) goes from 9 ms to 897 ms.
  - +7.7 s comes from identical hashing work running 2.3x slower.
- GPU_SCAN: +11.8 s (1.72x).
- HASH_GROUP_BY: +4.5 s (1.62x).
- DYNAMIC_FILTER: +3.0 s.
- UNGROUPED_AGGREGATE: +1.2 s (1.88x).
- MERGE_GROUP_BY: +1.1 s.
- FILTER: +1.0 s.
- PROJECTION: 1.04x, so no inflation.
- HASH_JOIN: **−3.9 s**. It is cheaper at 4 GPUs because of a join-mode change (section 2, C4).

Bridge of the +120 s: link +52.3 s, slower identical compute about +30.8 s, cross-GPU clones +25.1 s, new partition work
+15.9 s, join-mode change −4.0 s.

Two counterfactual wall times, assuming occupancy stays at 0.805:
- Removing compute inflation, clones and forced partitioning gives about 14.0 s (2.5x).
- Also restoring occupancy to 0.94 gives about 12 s (2.9x).

The rest is the physical 2x host-link ceiling. Caveat: thread-time savings do not turn into wall-time savings when a
phase is link-saturated.

## 1. Impediments not covered (or not quantified) by the readers

### N1. Identical operator work runs 1.6-3x slower per task at 4 GPUs, independent of link load (HIGH, new and measured)

- **Same task graph, same inputs, same per-GPU concurrency, slower per call:**

  | Operator | g1 median | g4 median | Mean concurrent ops on the same GPU (g1 / g4) |
  |---|---|---|---|
  | Q1 HASH_GROUP_BY(4), 627 tasks, 157.9 GB both runs | 4.77 ms | 10.15 ms | 4.0 / 3.23 |
  | Q18 PARTITION(5) | 2.67 ms | 8.0 ms | 3.94 / 3.98 |
  | Q6 UNGROUPED_AGGREGATE(2) on about 1.4 MB per task | 1.81 ms | 3.57 ms | — |

- **Not explained by PCIe saturation.** Q1 HASH_GROUP_BY takes 9.6 ms even when no HOST upload is in flight on any
  GPU. Q18 PARTITION(5) has zero concurrent uploads and is still 3x slower. The per-GPU medians are equal (no switch
  asymmetry). Source: `critic_op_vs_linkload.py`.
- **The pattern points at syncs.** Ops with mid-op host syncs, device scalars or pinned bounce buffers (copy_if/filter,
  reductions, groupby, partition) inflate. Pure PROJECTION does not (1.04x), and neither does the unfiltered Q18
  GPU_SCAN(3) (1.01x). That points at **host-side cross-GPU coupling**, or GPU clocks.
- Existing hypotheses that fit, in order:
  - **sync_contention H1 (code verified, prior raised to high):**
    - `cucascade/src/memory/small_pinned_host_memory_resource.cpp:166-185`: one pool-wide `mutex_`, LIFO slab pop,
      `cudaStreamWaitEvent` on the freeing stream's event (which can be another GPU), all under the lock.
    - `:203-225`: `cudaEventRecord`, plus `cudaSetDevice`/`cudaEventCreate` on a miss, under the same lock.
    - `.pixi/envs/default/include/cudf/detail/device_scalar.hpp:46` builds a bounce buffer with
      `make_pinned_vector(1, stream)`, which is `make_pinned_vector_async` plus `sync_stream` (`vector_factories.hpp:522-527`).
    - With one NUMA node, `sirius_context.cpp:1056-1083` installs a single pool for all GPUs.
    - So every device_scalar or small host_vector on GPU B can wait on GPU A's queued work, and 16 threads serialize on
      one mutex that wraps driver calls.
  - Other process-global locks on hot paths. `numa_small_pinned_mr::map_mutex_` (`src/memory/numa_small_pinned_mr.hpp:93,106`)
    is taken on every alloc and free. CUDA driver contention is also possible.
  - GPU-side: clock or power capping with four 600 W cards busy. Clocks were idle-normal when sampled
    (clocks_event_reasons=0), but that was not under load.
- **Contradicts:**
  - host_scan H4's signature "per-operator compute stays flat".
  - sync_contention H3 and config_knobs H4, which frame the loss as threads waiting on PCIe.
  - The data shows per-task compute itself inflates.

### N2. N-forced partitioning creates work that does not exist at 1 GPU (HIGH, new framing, measured +15.9 s)

- `partition_sizing.hpp:85-120`: `partition_min_num_partitions(N)=N` once input reaches 16 MB x N (64 MB at N=4).
- `sirius_physical_hash_join.cpp:873,895`: BUILD_PROBE runs at `min(natural, N)` partitions.
- `sirius_physical_partition.cpp:157`: `count < 2` means pass-through.
- So at N=1 every BUILD_PROBE join, and every exchange of 1 GB or less, skips hash partitioning on **both** sides,
  including the huge lineitem probe.
- At N=4 the same exchanges run a hash_partition kernel and a slice deep copy (`gpu_partition_impl.cpp:213-221`), then
  fan out into P x tasks fragments, then CONCAT.
- The readers treated this only as data movement (sched S1). The GPU and CPU work is a separate cost, paid even for the
  local quarter of the data.
- Hits: Q4 (897 ms vs 9 ms), Q3 PARTITION(18), Q5/Q8/Q10/Q12/Q21, which are the worst-scaling queries.

### N3. Accumulated Bloom filters go through host staging with 4-45 ms latency, only at 4 GPUs (MEDIUM, upgrade of xgpu H8)

- In g4_a, iter4 logs `accumulated Bloom published through HOST staging ... X ms after the final contribution` for:
  Q2 (3.9), Q4 (22.1), Q5 (12.6/19.2), Q7 (6.0), Q8 (11.6/37.8), Q9 (34.3), Q10 (21.0/44.6), Q12 (12.0), Q16 (13.6),
  Q20 (0.2) and Q21 (28/29).
- g1_a has none of these except Q20, which is a peer path taking 0.0 ms. Its builds are single-partition, so the filter
  publishes directly.
- These are almost exactly the worst-scaling queries (1.32-1.70x).
- The mechanism matters only if probe tasks run before publication. They are created eagerly
  (`task_creator.cpp:697-866`) and consumers do not wait (xgpu H8). So late publication means unfiltered probe batches,
  which means larger PARTITION/shuffle inputs.
- Also confounded with shuffle joins (N2/M3). The fix to test is a short publication wait.

### N4. Cross-device copy streams and contexts (LOW-MEDIUM, mechanical, verify in nsys)

- `representation_converter_builtins.cpp:916-930`: `convert_gpu_to_gpu` takes `target_stream` from
  `memory_space::acquire_stream()`. That is a shared `rmm::cuda_stream_pool(16)` round-robin (`memory_space.cpp:117-119`),
  not the task stream. Concurrent clones and other users on the same GPU can therefore collide on one stream, and each
  `target_stream.sync()` then waits for unrelated work.
- `peer_copy_into` (`:640-665`) issues the **D2H leg on the destination GPU's stream for source-GPU memory**, under a
  `cuda_set_device_raii(src)` guard, on pairs without P2P.
- The CUDA runtime accepts a memcpy on a stream that is not current, but which engine services a non-P2P cross-context
  D2H, and whether the driver adds its own staging, is unverified.
- nsys check: which device row the staging DtoH appears on, and its GB/s. Expect about 50 GB/s. If it is far lower,
  this is a defect.

### N5. Measurement hygiene that changes the scaling table (HIGH value, cheap)

- **g1 Q13 is not warm.** Iterations take 5.03, 3.92, 2.47 and 2.65 s. The hot mean includes 3.92, so Q13 scaling is
  overstated (2.76x; really about 2.4x). Q13 also triggers about 26 unsatisfiable `memory pressure: requesting downgrade
  of ~36.7 GB` monitor requests per iteration, at both g1 and g4. Use 5 or more iterations, or drop iter2, for Q13.
- **The user's g4 total (21.97 s) is not the orchestrator's g4_a (20.04 s).** g4_a uses `capacity_bytes 600GB`, while
  the user's `specific_config.yaml` uses 100GB. Recheck the user's original pin log (host_scan H8). The g1 totals match.
- **Every baseline ran with Quent on.** One Quent-off g4 total is needed to bound telemetry perturbation. The
  `stream.sync()` at `gpu_pipeline_task.cpp:735-737` runs regardless.

## 2. Evidence corrections (verified against code or data)

| # | Claim | Verdict |
|---|---|---|
| C1 | sched S1 experiment (3): "any host_staging doubles uplink traffic". xgpu H2: the staged H2D leg contends *because* it is staged. | **Wrong framing.** True cross-switch P2P, had the platform allowed it, would also go up switch A's uplink and down switch B's uplink, so the uplink bytes are the same. Staging adds a host DRAM hop, serializes the two legs (`peer_copy_into` has 2 syncs) and adds latency; it does not add downstream uplink bytes. Only **intra-switch** placement (0↔1, 2↔3) keeps shuffle bytes off the uplinks. So the lever is switch-aware placement, not "enable P2P". |
| C2 | host_scan H4: "per-operator compute stays flat". config H4 / sync H3: "the loss is PCIe waits". | **Contradicted** (N1): compute inflates 1.81x overall and 2-3x for specific ops, at equal concurrency and with no link traffic in flight. |
| C3 | sync H1, prior "high" | Mechanism verified line by line (N1). It also exists at N=1, coupling the 4 streams on one GPU, so it is a *differential* cost: at N=4 the waits land on other GPUs, which keep their own SMs busy elsewhere. The magnitude is unproven until nsys or a probe confirms it. Keep "high" as the leading N1 candidate. |
| C4 | config H8 (join-mode confound), prior "medium" | **Confirmed by data.** Q3 HASH_JOIN(15): g1 1616 ms in STANDARD over 54 tasks (91 GB processed) vs g4 165 ms in BUILD_PROBE. Suite HASH_JOIN is 6.4 s → 2.5 s. With `max_build_hash_table_bytes=2GB`, BUILD_PROBE needs rows x 16 B < 2 GB at N=1 but rows/4 x 16 B < 2 GB at N=4 (`hash_join.cpp:873-889`). This **flatters** g4. Under equal algorithms the scaling gap is wider. |
| C5 | sched S1/S4, xgpu H4: "max_build_hash_table_bytes derived default = 8 GB" | Correct. `sirius_config.cpp:1016-1020` derives 8 x hash_partition_bytes = 8 GB only when the value is not explicit. Note there is a second gate, `max_build_probe_resident_bytes` (8 x the GPU-derived batch size), which no reader mentioned. Include it in E4. |
| C6 | xgpu H1 / config H3 (fan-in) | Consistent with the data. MERGE_GROUP_BY GPU-origin prepare is 7.5 s over 101 tasks (74 ms each). CONCAT GPU-origin prepare is 16.7 s over 624 tasks. Together they are the entire +25 s clone bucket. |
| C7 | sched S2 / xgpu H5 / config H5 / host H6 / sync H2 (imbalance, binding) | Bounded by the measured occupancy loss: 0.943 → 0.805 overall. Small queries are worst (Q2/Q11/Q16/Q22 at about 0.5). That caps the combined straggler and tail family at about 15% of g4 wall, below N1/N2/M3. |
| C8 | sched S6 (creation fabric) | Weak evidence: 600 `no schedulable partition (build/probe already drained)` warnings at g4 vs 11 at g1, so hint-walk churn exists, but it is cheap. Keep "low". |
| C9 | sched S3 "lookahead compiled out" | Accurate: `creator/config.hpp:101,106` uses active/source as default *values*, not compiled out. A code change is still needed to flip them. |
| C10 | Everything about 2 GPUs (S5, host H2, xgpu H7, sync H8, config H1) | Plausible, but off the 4-GPU focus. Treat it as one item (M11). |

## 3. Merged hypothesis list (duplicates folded)

| ID | Merged from | Measured or expected share of g4 | Kind |
|---|---|---|---|
| **M1** Host-link ceiling (2 x Gen5 uplinks, 105 GB/s) | host H1, config H1 (4-GPU part) | +52 s thread-time. Wall floor about 11 s | hardware |
| **M2** Per-op compute inflation (host-side cross-GPU coupling or clocks) | **N1**, sync H1, sync H6 | about +31 s | design/mechanical |
| **M3** Shuffle clone cost: (N-1)/N moved, synchronous per-buffer staging, no key or switch awareness | sched S1, xgpu H2/H3, sync H4, config H2, host H3 | +25 s (CONCAT 16.7, MERGE_GROUP_BY 7.5) | design |
| **M4** N-forced partitioning (P≥N floor, BUILD_PROBE min(nat,N)) | **N2** (part of sched S1) | +16 s | design |
| **M5** Fan-in of hundreds of tiny partials, serial and latency-bound | xgpu H1, config H3, sched S7 | subset of M3 (about 7.5 s thread, critical path) | design |
| **M6** Broadcast / BUILD_PROBE thresholds too low | sched S4, xgpu H4, config H2 | lever on M3 and M4 | config |
| **M7** Occupancy loss: P mod N, binding, barriers, tails, 4 threads | sched S2/S3/S7, xgpu H5, config H4/H5, host H4/H6, sync H2/H3/H5 | 1.17x | design/config |
| **M8** Join-mode confound | config H8, C4 | flatters g4 by about 4 s thread | config/measurement |
| **M9** Host bytes: disabled compression for 4 tables, o_comment, l_shipmode | host H5, config H6 | lever on M1 | config |
| **M10** Late accumulated Bloom (host-staged) | **N3**, xgpu H8 | unknown; queries Q4/5/8/10/12/21 | design |
| **M11** 2-GPU prefix on one switch | S5, host H2, xgpu H7, sync H8, config H1 | 2-GPU row only | config |
| **M12** Low priority | sched S6, host H7 (1 MiB blocks), host H8 (capacity), sync H6/H7, xgpu H6, config H7/H9, **N4** | small | mechanical |

## 4. Prioritized experiment plan (minimal runs, ordered by information value)

All runs are warm (iteration 1 discarded, 4 or more iterations, at least 6 for Q13), host-pinned, and from the same
commit. Re-flatten Quent and rerun `critic_work_decomp.py` / `critic_op_inflation.py` after each run. That gives a
consistent before/after on the 5-bucket decomposition.

**E1 — nsys A/B on 3 queries, g1 vs g4. 2 short runs. Decides M2 (GPU-side vs host-side) and calibrates M1/M3/N4.**
- Queries: Q1, Q6 and Q18, iters 1-3, profiling iter 3 (use `nsys --capture-range=nvtx` or a delay).
- Flags: `--trace=cuda,nvtx,osrt --cuda-memory-usage=false --gpu-metrics-devices=all --sample=none` (optionally
  `--cudabacktrace=all` on Q6 only).
- Measure:
  1. For each NVTX op range (`sirius_physical_grouped_aggregate::execute`, `sirius_physical_partition::execute`, the
     ungrouped agg), the sum of GPU kernel time inside the range vs the range wall.
     - If kernel time is about 2x at g4 too, it is GPU-side: check the SM clock and throttle in gpu-metrics.
     - If kernel time is flat and wall is about 2x, it is host-side. Then rank, inside the ranges:
       `cudaStreamWaitEvent`, `cudaStreamSynchronize`, `cudaEventRecord`, `pthread_mutex_lock` (osrt), and
       `cudaHostAlloc`/`cudaFreeHost` counts.
  2. Per-GPU HtoD GB/s inside `sirius::compression::host_to_gpu` (M1).
  3. Bytes and time in `convert:gpu_to_gpu:host_staging` vs `:peer_copy`, and which device row the staging DtoH lands on
     (N4).

**E2 — code probe for M2, only if E1 says host-side in the slab or event path. Scratch worktree, g1 + g4 full suite.**
- Key `small_pinned_host_memory_resource` pools by CUDA device: `sirius_context.cpp:1060-1073` plus
  `numa_small_pinned_mr::node_for_current_device`.
- Alternative: skip slabs whose `event_device != device_of_stream(stream)`.
- Success means g4 compute thread-time drops toward 1.0-1.2x of g1, and g4 wall drops by about 2-3 s.

**E3 — config flip for M6+M8, both counts. g1 and g4 full suite, 2 runs.**
- Remove `max_build_hash_table_bytes` (it then derives 8 GB). Set `SET max_broadcast_join_size='8GB'`. Optionally raise
  `max_build_probe_resident_bytes`.
- Effects:
  - Removes the join-mode confound (equal algorithms at both counts).
  - Moves medium builds to broadcast, so the probe gets `partition_mode::local`, with no probe hash, no probe shuffle and
    no Bloom accumulation.
- Read: GPU-origin prepare, PARTITION compute and wall for Q3/4/5/7/8/9/10/12/18/21.
  - If those drop at g4 only, M3, M4 and M10 are policy-fixable.
  - Also report the new g1 baseline: the honest scaling denominator.

**E4 — topology split (answers M11 and stress-tests N1). 2 runs: `gpu_ids [0,1]` and `[0,2]`, full suite, Quent on.**
- Does the compute inflation (N1) appear at both?
  - Both: a multi-context or shared-pool cause (consistent with M2 host-side).
  - Only [0,1]: shared-uplink latency.
- Scan prepare per GB separates link sharing. Clone prepare per GB separates staging vs P2P.

**E5 — granularity sweep for M5/M3/M7. g4 only.**
- (a) `hash_partition_bytes` 256MB vs 1GB vs 4GB. Read the P mod N tail vs fragment count.
- (b) Re-pin with `scan_task_batch_size` 4GB (costly: about 2 min pin). Read MERGE_* GPU-origin prepare: expect about 4x
  less, plus CONCAT prepare.

**E6 — `executor.pipeline.num_threads` 8 at g1 and g4 (M7 latency hiding).** Run it only after E2/E3, because more
threads can amplify M2 contention. Comparing the two is itself a test of N1.

**E7 — bytes lever (M9). Re-pin with 4 table plans enabled, o_comment snappy and l_shipmode dictionary, g1 + g4.**
Bounds how far M1 can be pushed below the 2x ceiling. Expected gain at g4 is about 2x the gain at g1.

**Quent-only, no GPU (do now):**
- (i) Per pipeline, barrier efficiency and spread of the last finalize per GPU (M7).
- (ii) For each Bloom-accumulating join in g4_a, count the probe-scan tasks that started before the publication
  timestamp, and their bytes into PARTITION (M10).
- (iii) Q13: rerun the decomposition excluding iter2 (N5).

Skip unless the above leave gaps: creator thread counts (S6), host block_size (H7), DuringApiCall flag (sync H7) and the
JIT cudaGetDeviceProperties issue (sync H6). For sync H6, read the count from the E1 API summary instead.
