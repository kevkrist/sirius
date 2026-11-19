# DF construction: end-to-end control/data flow at stack tip (pr2570)

Read-only code study. Nothing measured; every cost figure below is an estimate to verify
(nsys/ncu microbench + TPC-H). Paths relative to `/opt/dlami/nvme/dfperf/repo`.
Base = `c4fa3638` (merge-base origin/main). Stack: #2569 (pr2569) -> #2570 (pr2570).

## 0. What the stack changes (and which PR owns what)

Base behaviour:
- Hash-partitioned build (>1 partition): Bloom-only accumulation at PARTITION through
  `hash_join_partition_observer` (already existed).
- Single-partition / broadcast build: CONCAT `concat_all` delivers one whole batch; hash join's
  `push_data_batch_partitioned` -> `observe_whole_build` builds every filter one-shot from the whole
  table on one GPU (one pass, one kernel per filter), then replicates.

#2569 (library + tests; production paths unchanged, `observe_whole_build` still live at pr2569):
- `construction_plan.{hpp,cpp}` (per-key planning from inventory facts)
- `build_filter_construction.{hpp,cpp}` (try_create / reserve / contribute / finish)
- accumulators: `src/cuda/exact_key_stager.cu`, `src/cuda/bounds_accumulator.cu`,
  `src/cuda/dynamic_filter_prepared_keys.cu`, `src/cuda/accumulation_partial.cuh`,
  Bloom builder rework in `src/cuda/sirius_dynamic_bloom_filter.cu` (all key types, per-key arrays)
- `build_input_certification.*`, `complete_build_inventory.*` extensions, key-outcome lines.

#2570 (wiring; the production behaviour change):
- `build_input_certifier.*` (shared arrivals/decide/contribute sequence)
- PARTITION forward path calls the observer (`src/op/sirius_physical_partition.cpp:160-195`),
  `on_hash_input` split, `task_scratch_bytes` in the no-history estimate (`:506-518`)
- session API `decline/begin/contribute/publish_if_final` on the new construction
  (`dynamic_filter_publisher.cpp:943-1188`), one-shot path deleted (`sirius_physical_hash_join.cpp`)
- Bloom budget raised to `max(4*scan_task_batch_size, max_build_hash_table_bytes)`
  (`src/sirius_config.hpp:266`) = 16 GB/GPU under `specific_config.yaml` (4GB scan batch, 8GB table)
- `enable_dynamic_filter_multi_partition` retired; right-family BUILD_FILTER for all key types.

Rule of thumb for attribution: kernel / accumulator / plan / `build_filter_construction` changes ->
#2569. Observer, PARTITION hooks, session contribute/publish, estimates, budget, certifier -> #2570.

Config under test (`/home/ubuntu/sirius/specific_config.yaml`): 4 GPUs, 4 pipeline threads,
4 task-creator threads, `hash_partition_bytes` 1GB, `concat_batch_bytes` 1GB, pin-table compression
on. No DF knobs set, so defaults: `enable_dynamic_filter=true`, zone maps OFF,
`inlist_max_l2_fraction=0.125`, `domain_coverage_threshold=0.9`, `keep_threshold=0.9`
(`src/sirius_config.hpp:262-305`). With zone maps off, `bounds_accumulator` exists only for
DECIMAL128 keys (`fit_pending`); TPC-H join keys are integers, so bounds are mostly absent.

## 1. Step-by-step flow

### 1.1 Arrival: `on_arrival` (per batch, producer's sink thread)
`sirius_physical_partition::on_input_batch_pushed` (`sirius_physical_partition.cpp:465`) ->
`hash_join_partition_observer::on_arrival` -> `build_input_certifier::on_arrival`
(`build_input_certifier.cpp`) -> `build_arrival_ledger::record`
(`complete_build_inventory.cpp`, `record` / `read_arrival`).
- Host only. Ledger mutex, `batch.try_to_read_only()`, 3 `dynamic_cast`s, one vector of
  `{type, null_count}` per top-level column. No GPU work. Retained: one entry per batch.
- Declines the whole attempt on a compressed (`simpatico_compressed_representation`) or unreadable
  arrival. Relevant here because `enable_pin_table_compression: true`.
- Cost: microseconds per batch. Negligible.

### 1.2 Decide: `before_claim` (once; task-creator thread)
`get_next_task_input_data` (`sirius_physical_partition.cpp:461`) -> `build_input_certifier::decide`
under `_decision_mutex`. Returns false (no task) until the source pipeline finished.
`read_build_port_facts` + `certify_decision` (`build_input_certification.cpp`), then
`ledger.certify` -> `complete_build_inventory::try_create` (sort by id, per-ordinal carrier check)
-> `session.begin` (`dynamic_filter_publisher.cpp:943`).

`begin` (once, creator thread, outside the session mutex):
1. `plan_construction` (`construction_plan.cpp`): host arithmetic. All plan-time gates are applied
   here, before any GPU work: type/carrier, domain gate, exact-vs-Bloom choice
   (`choose_membership_filter`), Bloom cap (drops every Bloom key if the sum > 16 GB/GPU).
2. `build_filter_construction::try_create` (`build_filter_construction.cpp:104`): leases storage for
   every accumulator on EVERY replica GPU through `lease_partials`
   (`src/cuda/accumulation_partial.cuh:168`):
   - one new `rmm::cuda_stream` per accumulator per GPU (up to 3 x G `cudaStreamCreate`)
   - Bloom: `blocks*32` bytes per key per GPU, `cudaMemsetAsync` zero fill (`:197`)
   - stager: `valid_rows*rep_bytes` + 4-byte overflow word per key per GPU, zero filled
   - bounds: `batches*32` bytes per key per GPU, not zero filled
   - `cub` size queries for scratch bounds (host only)
3. Install under the mutex, phase COLLECTING.

Retained from here until publication/finalize: G x (sum of Bloom arrays) + G x staging buffers.
This storage is raw device memory, not spillable. Bloom is 16 bits per valid build row
(`bloom_sizing.hpp`: 256-bit blocks, 16 keys per block), sized on valid rows, not distinct keys.
Example: 730M valid keys -> 1.46 GB per GPU -> 5.8 GB across 4 GPUs for the whole build phase.

### 1.3 Contribute: `before_transform` (per batch; pipeline task thread, the batch's GPU, task stream)
Hash path: `sirius_physical_partition.cpp:229-231` (`on_hash_input` then `before_transform`),
before `hash_partition`. Forward path (single partition, local, broadcast): `:160-163`, before the
routed output is assembled.

`build_input_certifier::contribute` -> `session.contribute` (`dynamic_filter_publisher.cpp:1095`):
- Under the session mutex (serial across all tasks of the join): `inventory->find` (binary search),
  claim check, `end_if_channels_closed`, `gpu->get_table_view()`, `batch_key_facts::of` (allocates a
  vector over ALL columns of the batch), `build_filter_construction::reserve`
  (`build_filter_construction.cpp:241`: row/carrier/null-count check, slice offsets).
- A batch that is not a GPU representation on a GPU tier, or mismatches the inventory, ends the
  attempt (`skipped_inventory`). A batch on a GPU with no leased partial ends it
  (`skipped_admission`).
- Outside the mutex: `build_filter_construction::contribute` (`:270`) enqueues on the task stream:

  a. `accumulated_bloom_builder::enqueue_add` (`src/cuda/sirius_dynamic_bloom_filter.cu:1080`):
     per Bloom key one cuco `add_if_async` kernel (`add_accumulated_keys`, `:265`): one thread per
     row, grid = rows/256, xxhash_64 of the key, atomic OR into one 32-byte block. Stencil is a
     counting iterator with a validity predicate. Carrier->rep conversion is a transform iterator
     (no copy); STRING keys are fingerprinted in-kernel (no copy). No allocation, no host wait.
     Plus per call: `cudaStreamGetDevice`, `cudaStreamWaitEvent(init)`, one `cudaEventCreate` +
     record + `cudaStreamWaitEvent` onto the partial stream, `cudaGetLastError` x2 per key.
  b. `exact_key_stager::enqueue_add` (`src/cuda/exact_key_stager.cu:144`): per exact key
     `prepare_build_keys` (`src/cuda/dynamic_filter_prepared_keys.cu:131`) writes the valid rows at
     the rep into the batch's slice. No nulls: one `thrust::for_each_n`. With nulls: a scratch
     `device_uvector<size_type>(rows)` from the task allocator + `thrust::exclusive_scan` +
     `for_each_n` (`:99-125`). No host wait. Then `written_mutex` to record the slice.
  c. `bounds_accumulator::enqueue_add` (`src/cuda/bounds_accumulator.cu:334`): per numeric key two
     separate `cub::DeviceReduce` passes (Min, then Max, `:131-143`) and one temp buffer; STRING
     keys do two `cudf::reduce` calls with a host readback each (the one host wait). Only for
     DECIMAL128 keys or opt-in zone maps.
- After: mutex again; the contribution that completes the inventory sets phase PUBLISHING and
  records its original id as the pending publication.

Hash path additionally runs `observe_partitioned_build_batch`
(`sirius_physical_hash_join.cpp:1559`): host only, MARK joins only. Negligible.

### 1.4 Publish: `after_transform` -> `publish_if_final` (once; the task that completed the inventory)
Called after the task's output is built and before it deposits
(`sirius_physical_partition.cpp:193`, `:246`). Every other task returns after one mutex check.
`state::publish` (`dynamic_filter_publisher.cpp:627`) -> `build_filter_construction::finish`
(`build_filter_construction.cpp:305`), all serial on the publishing task's thread and stream:
1. `bounds->finish` (`bounds_accumulator.cu:399`): D2H of all slots from every GPU into one pinned
   vector, `stream.sync()`; a second D2H + sync when a key arrived at a narrower carrier.
2. `bloom->finish` (`sirius_dynamic_bloom_filter.cu:1130`): sources = non-root GPUs that contributed;
   targets = every non-root replica GPU. Chunked (chunk = clamp(bytes/8, 2 MiB, 8 MiB),
   `complete_build_inventory.hpp:259-263`). Direct transport: per chunk, ingress peer copy from each
   source into scratch, `or_bloom_chunk` kernel on the root, egress peer copy to each target, one
   wait at the end (`chunk_pipeline`, `:772-960`). HOST-staged transport (what this machine takes;
   out of scope): every stage is followed by a `cudaStreamSynchronize` (`:1262-1320`).
3. `stager->gather` (`exact_key_stager.cu:245`): `cudaStreamSynchronize` on each non-root partial
   that wrote, one `enqueue_replica_copy` per written slice (per batch per key), D2H of the
   overflow words from every GPU, one `cudaStreamSynchronize`.
4. Per key on the root: `get_available_memory` room check, then small IN-list or cuco `static_set`
   `insert_async` from the staged keys (`sirius_dynamic_in_list_filter.cu:135-142`), zone-map
   scalars. Then `root.stream.sync()` (`build_filter_construction.cpp:398`).
5. Back in `publish`: `replicate_to_devices` per exact filter / zone map, one filter at a time, each
   allocating a set per target GPU, copying, and synchronizing every target stream
   (`sirius_dynamic_in_list_filter.cu:269-388`). Bloom filters skip this (already replicated).
6. `fan_out` pushes to channels (host), stats, log.

The publishing task holds its reservation, input pin, partitioned output, and stream through all of
this, and its deposit waits for it.

### 1.5 Finalize: `finalize_input` (once; operator finalizer)
`on_finalize_operator` (`sirius_physical_partition.cpp:473`) -> `session.finalize_input` ->
`finish_input` (no CUDA work) + `release_retired_storage`. Published filters keep their arrays
through the channels.

## 2. Single-partition vs multi-partition, single-GPU vs multi-GPU

| | hash path (>1 partition) | forward path (single / local / broadcast) |
|---|---|---|
| hook | `:229-231` before scatter | `:160-163` before routing |
| task's other GPU work | hash + scatter of the whole batch | none (batch forwarded unchanged) |
| DF share of task GPU time | a fraction | ~100% |
| base behaviour | Bloom accumulation here already | nothing here; filters built one-shot after CONCAT |
| what is new | exact keys + bounds accumulators, per-key Bloom arrays | everything |
| downstream | CONCAT per partition | CONCAT `concat_all` still runs (`consumer_partition_policy.cpp:82-90`) |

Forward path consequences:
- One whole-table kernel per filter is replaced by one kernel per filter per batch (same rows).
- Publication happens before CONCAT instead of after it, so the filter is visible earlier.
- Batches of a single-partition build sit on whichever GPU scanned them. Each GPU's partial takes
  its own batches, and the root must merge G-1 full Bloom arrays. Base moved the rows to one GPU for
  the concat anyway and built there, so this cross-GPU Bloom merge is a new cost.
- Broadcast: contribution happens once per original batch, before fan-out to the slots.

Multi-GPU (G replica GPUs, restricted by `restrict_replicas_to` to the active GPUs):
- Every accumulator allocates full-size storage on all G GPUs at `begin`, whether or not a GPU ever
  contributes.
- Bloom publication moves (contributing non-root GPUs) x array bytes in, and (G-1) x array bytes
  out. With G=1 there is no transfer and no scratch.
- Concurrent tasks on one GPU insert into the same partial from different streams (atomic OR).

Memory estimate: `no_history_peak_memory_estimate` (`sirius_physical_partition.cpp:506-518`) returns
`2*input + scratch` while constructing, on every layout. The task-level fallback was already
`2*input` when no operator gives a positive estimate (`src/pipeline/gpu_pipeline_task.cpp:977-986`),
so the net change without history is `+scratch` only. Scratch = exact keys x (4 bytes/row + scan
temp) + Bloom publication scratch (<= 2 chunks x (G-1) x 8 MiB). Small.

## 3. Per-batch cost added to the PARTITION hot path (estimates, unmeasured)

Ranked by expected size:
1. Bloom `add_if_async` kernel, one per Bloom key per batch. Reads only the key column, but every
   row does a random read-modify-write into an array far larger than the GPU L2 cache for any big
   build. Expected to be the dominant DF cost. On the hash path it runs serially on the same stream
   ahead of the scatter. On the forward path it is the task's only GPU work.
2. Exact-key staging: one sequential copy per key per batch; with nulls an extra scan and a
   rows x 4-byte scratch allocation. Only for small builds (set <= 0.125 x L2), so small in absolute
   terms.
3. Bounds: two reduction passes per key per batch. Absent for TPC-H under this config.
4. Host overhead per contribution: session mutex twice, one vector over all columns, one event
   create/record/wait per accumulator, a few CUDA API calls. Tens of microseconds; matters only for
   builds with very many small batches.
5. Arrival ledger: negligible.

Once per join: G x (memset of Bloom + staging bytes), up to 3G stream creations (creator thread),
and the publication (section 1.4), which delays the last task's deposit.

## 4. Hypotheses to test

Kernel / accumulator (#2569 files):
- H1. Bloom insert is random-access bound for arrays >> L2. Test: ncu on `add_if_n` across array
  sizes (L2-resident vs 1-4 GB). Candidates: order or bucket keys by block before inserting for
  large arrays; fewer bits per key for very large builds; check cuco's `conditional_add` and
  horizontal-layout settings for the policy in use.
- H2. Validity stencil on non-null columns: `add_if_async` with a counting iterator and predicate
  even when `null_count == 0`. A plain `add_async` branch may be cheaper. Small.
- H3. Bloom sized on valid rows, not distinct keys. Builds with many duplicate keys get oversized
  arrays: more bytes to zero, merge, replicate, and more probe cache misses.
- H4. Eager full-size allocation + zero fill on every replica GPU at `begin`. Non-contributing GPUs
  only ever receive the egress copy, which overwrites the whole array, so their memset is wasted and
  their memory is held for the whole build phase. Contributing non-root GPUs are also fully
  overwritten at egress. Candidate: allocate/zero lazily on first contribution, allocate pure
  targets at publication.
- H5. Staging buffers are zero filled although every slice is written or overwritten by `gather`.
  Wasted memset (small).
- H6. `gather` issues one cross-GPU copy per written slice. Many batches -> many tiny copies.
  Coalesce adjacent slices per source GPU.
- H7. Bloom merge chunk capped at 8 MiB: a 1.5 GB array is ~175 chunks x (ingress per source + OR +
  egress per target) = >1000 async ops and events on the direct path. Larger chunks for large arrays
  cut launch and event overhead.
- H8. Bounds: Min and Max are two passes; one fused min-max reduction halves it. Only matters for
  DECIMAL128 keys or zone maps.
- H9. Three accumulators each own a stream per GPU and fold an event per contribution. One shared
  partial stream per GPU and one event per contribution would cut API calls and stream creation.
- H10. Null-compaction scratch (`exclusive_scan` over rows) per exact key per batch; a
  `copy_if`-style single pass or a shared rank array across keys with the same mask would do less.

Wiring / session (#2570 files):
- H11. Forward-path tasks used to be free and now carry all DF GPU work plus the final publication.
  Measure single-partition and broadcast builds against base (one-shot after CONCAT): total DF GPU
  time, and time-to-visible.
- H12. Single-partition build spread over G GPUs pays a cross-GPU Bloom merge that base did not.
  Candidate: when the layout is single-partition/broadcast and the build will be concatenated on one
  GPU anyway, compare against contributing on the destination GPU or merging lazily.
- H13. `finish` is fully serial with 4+ host waits (bounds sync, Bloom wait, gather sync, root sync)
  and then per-filter serial `replicate_to_devices`, each with its own sync. Candidate: enqueue
  everything, wait once; replicate all exact filters in one fan-out.
- H14. The publishing task holds its output and delays its deposit for the whole publication. With a
  large Bloom this stalls the last partition's delivery and whatever waits on build completion.
- H15. Session mutex held across `get_table_view` + `batch_key_facts::of` over all columns. Only the
  planned key ordinals are needed. Serial section across the 4 pipeline threads; tiny per call.
- H16. Bloom budget raised to 16 GB/GPU: up to 64 GB of non-spillable Bloom storage across 4 GPUs
  for one join, held from `begin`. Check memory pressure / downgrades on the large-build queries.
- H17. History-based estimates: the publishing task's peak (root IN-lists, merge scratch) enters the
  pipeline memory history and may inflate later tasks' reservations.

Work done and then thrown away:
- W1. Any attempt that ends mid-build (a batch on a non-GPU tier, a GPU without a lease, inventory
  mismatch, OOM, channels closed) discards every insert already made. Later batches return early.
  Check the counters `accumulations_skipped_inventory`, `accumulations_skipped_admission`,
  `accumulations_declined_*` in a TPC-H run to see how often this happens.
- W2. Plan-time gates (domain gate, Bloom cap, type) cost nothing on the GPU: they are decided in
  `plan_construction` before any contribution. Good.
- W3. DECIMAL128 key whose bounds do not fit int64: the membership filter is accumulated in full and
  dropped at `finish`. Could stop at the first unfit batch. Not a TPC-H case.
- W4. No build-side usefulness check beyond the domain gate: a filter the consumer later rejects
  (`keep_threshold` 0.9) or that arrives after the probe scan has drained is still built in full.
  `end_if_channels_closed` covers only fully closed channels.
- W5. Compressed arrivals decline the join's filters outright. With pin-table compression on, check
  for `declined_compressed_arrival` key lines; that is lost filtering, not wasted work.

Not a real saving: the Bloom hash (xxhash_64 of the key) and the partition hash (murmur over the
partition keys) are different functions with different seeds, so the hash path cannot share them
without changing one of the two.

## 5. Suggested measurements

- nvtx ranges already present: `dynfilter::construction::begin`, `dynfilter::accum::contribute`,
  `dynfilter::stage::contribute`, `dynfilter::bounds::contribute`, `dynfilter::accum::finish`,
  `dynfilter::stage::gather`, `dynfilter::bounds::finish`, `dynfilter::replicate_devices`, marks
  `dynfilter::accum::final_commit` / `dynfilter::accum::visible`. The contribute ranges cover only
  the enqueue (async), so GPU time needs kernel-level attribution in nsys.
- Log lines: `[dynamic_filter_key]` outcome per key, "construction started: K key(s), B batch(es) on
  G GPU(s)", "constructed filters published ... N ms after the final contribution".
- Microbench: `add_if_n` vs array size and batch size (H1-H3); begin cost vs G and array bytes (H4);
  finish latency vs array bytes, G, batch count (H6, H7, H13), on the direct transport.
