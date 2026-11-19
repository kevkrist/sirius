# DF concurrency / latency audit (stack tip pr2570) — READ-ONLY, nothing measured

Paths relative to `/opt/dlami/nvme/dfperf/repo/src`. PR tags: **[69]** = #2569, **[70]** = #2570,
**[base]** = already on main (touched only lightly by the stack).
Config: 4 GPUs, 4 pipeline threads, 4 task-creator threads, hash_partition_bytes 1GB,
scan_task_batch_size 4GB, max_build_hash_table_bytes 8GB -> Bloom cap/GPU = max(4*4GB, 8GB) = 16GB
(`sirius_config.hpp:272`; the `max(...)` is [70] but value unchanged under this config).

All statements below come from reading code. Costs are estimates to be confirmed by
microbench / nsys; none were run.

## 0. Flow at tip

1. Arrival: `partition::on_input_batch_pushed` -> `build_input_certifier::on_arrival`
   (`op/dynamic_filter/build_input_certifier.cpp:36`) -> `build_arrival_ledger::record`
   (`complete_build_inventory.cpp:239`): per batch rows + per-column (type, null_count), host only.
2. First claim: `partition::get_next_task_input_data` (`op/sirius_physical_partition.cpp:461`) ->
   `certifier.decide()` (`build_input_certifier.cpp:43`) -> returns false until source pipeline
   finished -> `session.begin(inventory)` (`dynamic_filter_publisher.cpp:943`):
   `plan_construction` + `build_filter_construction::try_create` = leases on every replica GPU.
3. Per task (one certified batch): `before_transform` -> `session.contribute`
   (`publisher.cpp:1095`) -> `construction->contribute` (`build_filter_construction.cpp:270`):
   bloom `enqueue_add`, stager `enqueue_add`, bounds `enqueue_add`, all on the **task stream**,
   before the scatter.
4. Task whose contribution completes the inventory = Publishing Task: `after_transform` ->
   `publish_if_final` -> `state::publish` (`publisher.cpp:627`) -> `construction->finish`
   (`build_filter_construction.cpp:305`) -> replicate exact/zone -> `fan_out`. Runs after
   scatter, before `sink`.
5. `on_finalize_operator` -> `finalize_input` -> `release_retired_storage` (`publisher.cpp:840`).

Single-partition (forward / broadcast / local, `partition.cpp:158-198`) [70]: same 3/4, no
scatter between. Multi-partition hash (`partition.cpp:229-246`): hook placement is [base], the
exact/bounds accumulators and unified construction are [69].
BUILD_FILTER operator (right-family joins) uses the same certifier, publishes right after
contribute (`sirius_physical_build_filter.cpp:74-90`).

## 1. Locks

| Mutex | Where | Held across | Verdict |
|---|---|---|---|
| `state::mutex` (session) | `publisher.cpp:787` | claim in `contribute` (1107-1147): binary search, `end_if_channels_closed` (atomic loads), `get_table_view`, `batch_key_facts::of` (vector alloc), `construction->reserve` (O(keys), vector alloc). No CUDA. | OK. ~4-5 acquisitions per batch (claim, completion counter 1153, `active_operation` dtor 614, `settle` 567). Negligible at 1GB batches. |
| same | `publish` 631/691/704/725, `complete` 400 | counters; `complete()` calls `producer.finish` -> channel mutex (nested session->channel, `sirius_dynamic_filter.cpp:569`) | OK, no CUDA under lock. |
| `_decision_mutex` | `build_input_certifier.cpp:45` | **entire `begin()`**: plan, `cudaDeviceGetAttribute` per GPU, `lease_partials` x3 accumulators x4 GPUs (reservation, RMM alloc of whole Bloom arrays, `cudaMemsetAsync`, 12 `rmm::cuda_stream` creates, events) | Runs on a task-creator thread; other creators polling the same PARTITION block behind it. One-off per join. Cost = RMM pool alloc time for up to 4 x multi-GB. Measure (`dynfilter::construction::begin` NVTX range exists). [69] begin, [70] certifier |
| ledger `_mutex` | `complete_build_inventory.cpp:241` | `try_to_read_only` + vector alloc per arriving batch | Serializes the 4 pushing threads for microseconds. OK. |
| `written_mutex` (stager), `leaf_mutex` (bounds) | `cuda/exact_key_stager.cu:204`, `cuda/bounds_accumulator.cu:393` | vector push only | Leaf, OK. [69] |
| channel mutex | `sirius_dynamic_filter.cpp:550` | per `push_filter` | OK. [base] |

`post_unlock_actions` (`post_unlock_actions.cpp`) [69]: only carries `[dynamic_filter_key]` log
lines out of the lock; `dispatch()` logs at DEBUG. No perf concern. No lock-ordering hazard found
(session -> channel only; decision -> session only; ledger never nested with session).

No lock is held across CUDA work or a host wait. Lock contention is not a perf problem here.

## 2. Streams and synchronization points

### Per contribution (every build batch, every pipeline thread)
- All DF kernels go on the **task stream**, ahead of the scatter: task wall time = DF + partition,
  no overlap. (`build_filter_construction.cpp:294-299`)
- Bloom `enqueue_add` (`cuda/sirius_dynamic_bloom_filter.cu:1080`): 1 `add_if_async` per Bloom key;
  no alloc, no host wait. 1 `cuda_event` create/record/destroy + 2 `cudaStreamWaitEvent`.
- Stager `enqueue_add` (`cuda/exact_key_stager.cu:144`): `prepare_build_keys`
  (`cuda/dynamic_filter_prepared_keys.cu:99`): null-free = 1 `for_each`; with nulls = `ranks`
  uvector (4 B/row temp) + `exclusive_scan` + `for_each`. No host wait (nosync policy).
- Bounds `enqueue_add` (`cuda/bounds_accumulator.cu:334`): per numeric key `cub::DeviceReduce::Min`
  then `::Max` = **two full passes** + 1 tiny `device_buffer` temp + 2 size queries
  (`bounds_accumulator.cu:131-144`). No host wait.
- **STRING zone-map key: 2 x `cudf::reduce` + 2 x `to_string(stream)` = 2 blocking syncs per
  batch per key** (`bounds_accumulator.cu:386-392`), plus pinned allocs. Only blocking read in
  the contribution path. TPC-H join keys are integers, so likely not hit; confirm from
  `[dynamic_filter_key]` lines.
- 3 separate "done" events + folds per batch (one per accumulator, each with its own partial
  stream): `accumulation_partial::enqueue_contribution` (`cuda/accumulation_partial.cuh:134`).
  Up to 12 extra streams per join (3 accumulators x 4 GPUs). Microseconds; could be one shared
  partial + one event.

### Publishing Task (serial; blocks that task's `sink` and the filter's visibility)
`build_filter_construction::finish` (`build_filter_construction.cpp:305-400`), in order:
1. `bounds->finish` (`bounds_accumulator.cu:399`): pinned alloc, D2H of slot arrays per GPU,
   **`stream.sync()` (439)** — this also drains every GPU's outstanding contributions. If a
   narrowed carrier exists: scalar restore + D2H + **second `stream.sync()` (518)**.
2. `bloom->finish` (`sirius_dynamic_bloom_filter.cu:1130`): PEER path = chunk pipeline, one wait
   (`late_wait`, 874). HOST_STAGED path (this machine) = sync per chunk per source/target
   (1257-1324): O(chunks x (2*sources + 1 + targets)) host waits, chunk <= 8MB
   (`complete_build_inventory.hpp:259-263`). **User said ignore HOST bounce**, but note the PEER
   path moves the same bytes.
3. `stager->gather` (`exact_key_stager.cu:245`): `cudaStreamSynchronize` per non-root source GPU
   that wrote (279) + one replica copy **per written slice** (283-293, O(batches) copies) +
   overflow-word D2H per GPU + **`cudaStreamSynchronize` (316)**.
4. Exact filter build (`staged->build`, cuco `insert_async`) + zone scalars, then
   **`root.stream.sync()` (398)**.
5. `replicate_to_devices` for in-list / zone map, sync per target
   (`publisher.cpp:653-665`, `cuda/sirius_dynamic_in_list_filter.cu:361-368`).
6. `fan_out` (host only).

=> 4-6 host waits in series on the Publishing Task even on the PEER path; 1 GPU path: 3-4.
Steps 1 and 3 could share a single D2H + single wait (bounds slots + overflow words read in one
pinned buffer), and step 4's sync could be the only one.

### Bloom merge volume (multi-GPU, the big one) [base mechanics, reached for all builds by 69/70]
- Every replica GPU holds a full-size partial (sized for *all* build rows).
- Finish: 3 source arrays -> root (ingress), OR kernel, root -> 3 targets (egress):
  **6 x array bytes through the root GPU's link**, serial inside the Publishing Task.
  Bloom = 2 B/key (`bloom_sizing.hpp:35`, 16 bits/key). orders-sized build (1.5B rows): 3GB
  array -> 18GB moved; 6B-row build: 12GB array -> 72GB moved. At ~10-20GB/s that is seconds of
  publication latency. This is the dominant finalize cost for multi-GPU; measure
  `dynfilter::accum::finish` and `final_commit` -> `visible` marks.
- Sources = only GPUs with `contributed` set (1172): if all build batches sit on one GPU, ingress
  is skipped, egress (3 x bytes) remains.

### Publication ordering
- Hash path: `publish_if_final` runs **after** the final batch's `hash_partition`
  (`partition.cpp:233-246`). Filters could be visible one scatter earlier if published right
  after the final contribution; publishing only reads accumulator storage, not the scatter
  output. [base ordering; 70 keeps it]
- Publishing Task's `sink` (its partitions reaching the join) waits for the whole publish. Other
  PARTITION tasks are not blocked (no lock held, GPU work on own streams) but compete for PCIe
  and the root GPU.

## 3. RMM allocation / reservation

Up-front at `begin` (`accumulation_partial.cuh:167` `lease_partials`), on **every replica GPU**:
- Bloom: `blocks*32` B per key, zero-filled by `cudaMemsetAsync` on the partial stream; every
  contribution waits on `init`. Sized on valid rows of the whole build (`construction_plan.cpp:70`),
  **not capped by the key's known domain cardinality** — a build with duplicate keys
  (rows > domain, counted as `keys_build_exceeded_domain`, `detail/domain_gate.hpp:38`) gets a
  Bloom larger than its distinct count warrants. Candidate: `sized_rows = min(valid_rows,
  domain.cardinality)` when known. [69, `construction_plan.cpp`]
- Exact stager: `valid_rows * rep_bytes` + 4 B word per key per GPU; bounded by IN-list policy
  (L2 fraction 0.125) so small. Zero-filled though it is fully overwritten (`exact_key_stager.cu:120`,
  unneeded memset; tiny).
- Bounds: 32 B x batches per key per GPU.
- Reservation: `scoped_replica_reservation::try_acquire` -> `make_reservation_or_null`
  (`dynamic_filter_replica_reservation.hpp:68`): never waits, never triggers downgrade itself;
  refusal = attempt skipped (`accumulations_skipped_admission`). All-or-nothing across accumulators.

Per batch: no Bloom temp; stager 4 B/row `ranks` only with nulls; bounds one tiny CUB temp per key
per batch (could be allocated once per task).

Publishing Task scratch: PEER = `min(2,chunks) * sources * chunk` <= 2*3*8MB = 48MB
(`sirius_dynamic_bloom_filter.cu:1205`); in-list set built from task allocator with
`build_within_room` check (`build_filter_construction.cpp:342`).

Task reservation: `partition::no_history_peak_memory_estimate` adds `task_scratch_bytes`
(`partition.cpp:506-518`) [70]. Single-partition forward path used to return 0 (-> generic
2 x input fallback, `pipeline/gpu_pipeline_task.cpp:986`); now 2 x input + scratch, same order.
Only used with **no history**: once `pipeline_memory_history` has samples
(`gpu_pipeline_task.cpp:923,965`) the scratch is not added, so the Publishing Task (always the
last, so always has history) is not guaranteed its scratch; it then relies on
`build_within_room` / OOM fallbacks (filter dropped, or Bloom falls to HOST staging at 1209-1214).
Check counters `filters_skipped_construction_admission` in TPC-H runs.

## 4. Memory retained and spill pressure

- Bloom partials live from `begin` (all build input present in the FULL port, i.e. peak build-side
  memory) through the whole PARTITION phase, on all 4 GPUs, then become the published filter
  (`make_filters`, 1359) and live until the probe is done. Worst case under this config 16GB/GPU.
  The memory is invisible to the downgrade executor (cannot be spilled), so it shrinks headroom
  for the 2 x input scatter reservations of concurrent PARTITION tasks -> more reservation waits
  / downgrades of data batches. Not directly triggered by DF code. Watch Quent downgrade events
  during build PARTITION ranges on big-build queries.
- Non-contributing GPUs hold a zeroed full-size array during the whole phase only to receive
  egress at the end; could be leased at publication instead (halves-to-quarters early footprint
  when the build sits on few GPUs).
- **Failed/skipped attempt keeps everything until the site finalizes**: after `end_attempt`
  (channels drained, admission, inventory mismatch, cancellation) the construction, including
  multi-GB Bloom partials on all GPUs, is held until `finalize_input` -> `release_retired_storage`
  (`publisher.cpp:840-852`, comment at 1083), i.e. until every remaining PARTITION task ran.
  Pure dead weight during the scatter phase. [69 lifetime; 70 call site]
- Exact staging buffers + bounds slots: also held to finalize after publication; small.
- PARTITION start is already gated on a complete FULL input (layout sizing,
  `partition/partition_exchange.cpp:80-98`), so `certifier.decide()` returning false until the
  source finished adds no new wait and no new retention of input batches. [checked vs base]

## 5. O(batches^2)

None found. Checked: `inventory.find` (binary search), `claimed` vector, `try_create`
(O(keys x batches)), `inventory.facts()` (O(cols x batches)), bounds finish (O(keys x batches)),
ledger sort (n log n). Linear-but-notable: stager `gather` issues one replica copy per (batch,
key) written on a non-root GPU (`exact_key_stager.cu:283`); slices are contiguous per inventory
order so adjacent ones could be coalesced. Exact keys only (small builds).

## 6. Duplicate work vs PARTITION; sharing options

What PARTITION does (`partition/gpu_partition_impl.cpp:117`): optional key cast column, then
`cudf::hash_partition` (MurmurHash3_32 row hash % P, scatter of all columns, internal host sync),
then per-partition copy (`std::make_unique<cudf::table>(slice)`, 218-226, a second full copy of
the batch — not DF, but the biggest cost in the task).

Key-column passes per batch at tip: partition hash 1 + Bloom 1 + Min 1 + Max 1 (+ stager 1) =
4-5 sequential reads of each key column. Nothing is shared:
- Hash values: partition uses 32-bit Murmur3 inside cuDF (not exposed); Bloom (cuco) needs a
  64-bit hash (block index for up to ~500M blocks + pattern bits). 32 bits is not enough and
  reusing the partition hash would correlate filter bits with partition id. Sharing needs a
  custom partitioner; both join sides and exchange must agree on it. Low value: sequential key
  reads are cheap next to Bloom random writes and the full-table scatter.
- Key cast: partition materializes a cast column for mixed-type keys (`gpu_partition_impl.cpp:154`);
  DF converts per element via iterator (`cuda/dynamic_filter_probe.cuh:468`), no extra column. OK.
- Cheap fusion inside DF: Min+Max in one reduce (halves bounds passes); Min/Max folded into the
  Bloom insert kernel is possible but needs a custom kernel in place of cuco `add_if_async`.
- Locality idea (speculative): insert into the Bloom after the scatter, per partition, with the
  Bloom block index's top bits taken from the partition id -> each partition's inserts touch 1/P
  of the array (cache/TLB-friendly). Costs an extra hash at probe; only worth it if microbench
  shows Bloom insert is memory-latency bound on multi-GB arrays.
- Overlap instead of fusion: enqueue DF kernels on a second stream (the partial's) so they run
  concurrently with the scatter instead of serially ahead of it; correctness already relies on
  event folds, not on the task stream.

## 7. Path matrix

| | 1 GPU | 4 GPU |
|---|---|---|
| Single-partition (forward/broadcast/local) [70] | contribute per batch; finish = 3-4 syncs; no merge; publish immediately after last contribute | batches may sit on several GPUs -> same merge/egress as multi-partition; broadcast builds are small (<3GB) so mostly exact/small Bloom |
| Multi-partition hash [69 on base hook] | contribute + scatter serial on task stream; finish after last scatter | + full-array merge 6 x bytes via root; + 4 x up-front Bloom leases; Publishing Task sink delayed |

## 8. Ranked candidates (to measure before prototyping)

1. Bloom contribution cost per batch vs scatter cost (multi-GB array random writes). Microbench
   `add_if_async` at 300MB / 3GB / 12GB arrays, ncu for memory stalls. Decides everything in §6. [69]
2. Multi-GPU Bloom merge/egress latency (`dynfilter::accum::finish`): reduce bytes — cap size at
   domain cardinality [69 `construction_plan.cpp`], retune bits/key (`bloom_sizing.hpp`), lease
   non-contributing targets lazily. [69]
3. Publish before the final batch's scatter in the hash path (`partition.cpp:233-246`). [70 touches
   this code; ordering is base]
4. Collapse finish syncs (bounds D2H + overflow words + construction sync -> one wait). [69]
5. Release a dead attempt's storage at once instead of at site finalize. [69]
6. Fuse Min+Max into one pass; one fold event per contribution instead of three. [69]
7. Zero-fill of exact staging buffers is unnecessary. [69]
8. Publishing Task scratch not reserved when history exists. [70]
