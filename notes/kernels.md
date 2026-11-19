# DF CUDA kernel audit (stack #2569 -> #2570)

Scope: `src/cuda/{accumulation_partial.cuh, bounds_accumulator.cu, dynamic_filter_prepared_keys.cu,
dynamic_filter_probe.cuh, exact_key_stager.cu, sirius_dynamic_bloom_filter.cu,
sirius_dynamic_in_list_filter.cu, sirius_dynamic_small_in_list_filter.cu}` + `src/op/dynamic_filter/detail/`.
Read-only audit at `pr2570` tip (`/opt/dlami/nvme/dfperf/repo`), base `c4fa3638`. Nothing built or run:
every impact number below is an ESTIMATE to be confirmed by microbenchmark (nsys/ncu).
cuco internals were read from `/home/ubuntu/sirius/build/release/_deps/cuco-src` (main checkout's
fetch; the dfperf clone had not fetched cuco yet) - re-check the pin once the dfperf build finishes.

## 0. PR attribution of the CUDA code

- **All kernel/accumulator mechanics are #2569 (S5B1).** New files: `accumulation_partial.cuh`,
  `bounds_accumulator.cu`, `exact_key_stager.cu`, `dynamic_filter_prepared_keys.cu`. Edited:
  bloom `.cu` (per-key shapes/domains, string fingerprint iterator, prepared ctor, partial moved out),
  in-list / small-in-list (prepared-keys ctors), `dynamic_filter_probe.cuh` (+79: `require_prepared`,
  `current_device`, `string_fingerprint_at`, `with_string_fingerprint_iterator`).
- **#2570 (S5B2) changes no kernel.** In `src/cuda` it only deletes the shared-geometry
  `accumulated_bloom_builder::try_create` (-31 lines); in `detail/` only doc + removal of
  `accumulated_bloom_builder::supports` and `publication_path`. What 2570 changes is the *caller*:
  every build (single-partition too) now runs these accumulators from the build PARTITION /
  `sirius_physical_build_filter::execute` (`build_filter_construction.cpp:270-301` contribute,
  `:303-398` finish). So 2570 decides how often and on what batch sizes the 2569 kernels run.
- Pre-existing on main (not introduced by the stack, but on the hot path): cuco bloom
  `add_if_async`, `or_bloom_chunk`, `chunk_pipeline`, HOST-staged finish loop, `or_words`,
  `replicate_to_devices`, bloom sizing (16 bits/key).
- Rule of thumb for fixes below: anything inside an accumulator/kernel -> **2569**; anything about
  when/how often/with which batch the accumulator is invoked -> **2570**.

## 1. CUDA Best Practices Guide digest (items used below)

- G1 (10.1, high): minimize host<->device transfers; batch many small transfers into one.
- G2 (10.1.1/10.1.2): pinned memory for async copies; do not over-allocate pinned; overlap copies with kernels on non-default streams.
- G3 (10.2.1, high): coalesced global access; block size multiple of 32.
- G4 (10.3): allocation is expensive - avoid per-call alloc/free (pool / reuse).
- G5 (11.1-11.3, medium): occupancy; 128-256 threads/block; many blocks; `__launch_bounds__`.
- G6 (11.5/11.6): non-default streams for concurrency; one context per GPU.
- G7 (12.1.4, low): shifts instead of div/mod. G8 (12.1.5, low-med): signed loop counters.
- G9 (12.2, high): minimize global memory use/traffic.
- G10 (13.1 high / 13.2 low): avoid intra-warp divergence; make branches predicable.

## 2. Per-call audit

### 2.1 Bloom add - `add_accumulated_keys` (`sirius_dynamic_bloom_filter.cu:264-303`), called per key per batch from `enqueue_add` (`:1080-1120`)
- What: `cuco::bloom_filter_ref::add_if_async` on a raw leased array. Filter type
  (`dynamic_filter_fused_probe.cuh:76`) = default `cuco::bloom_filter_policy<Key>`: sectorized
  blocked Bloom, 32-byte block = 8 x uint32, 8 pattern bits (1 per word), `xxhash_64<Key>` once per
  key, block index by multiply-shift (no modulo - G7 ok), `AddHorizontalLayout=8`,
  `ConditionalAdd=false`, `PersistingL2Access=false`.
- Launch: `add_if_n<…, CGSize=8, 256>`, grid = ceil(n/256), `__launch_bounds__(256)` (G5 ok).
  One thread per key computes the hash; each 8-thread tile then walks its 8 keys via `shfl`, each
  thread doing one `atomicOr` -> 8 atomics per key, all in one 32-byte sector.
- Memory: key read coalesced; validity stencil = bitmask word read (coalesced); filter write =
  **random scatter with atomics, 1 sector per key**. Sizing 16 bits/key (`bloom_sizing.hpp:33`):
  SF1000 orders 1.5B keys -> 3.0 GB, partsupp 800M -> 1.6 GB, part 200M -> 400 MB, customer 150M ->
  300 MB. All far above L2, so each key is one DRAM sector RFO + write. Inherent to Bloom; the
  blocked layout is already the guide-conformant form (G9: one sector, not k random words).
- Passes over keys: 1. Temp allocs: 0. Syncs: 0. Rehash: yes, per filter kind (bloom xxhash64,
  set xxhash32 double hashing, PARTITION's own partition hash) - cheap vs the scatter.
- Host API per call: 2x `cudaGetLastError`, per batch additionally `cudaStreamGetDevice`,
  `cudaEventCreate/Record/Destroy`, 2x `cudaStreamWaitEvent` (see F8).
- STRING keys (2569): `with_string_fingerprint_iterator` (`dynamic_filter_probe.cuh:435-456`)
  hashes bytes in-kernel (xxhash64 over chars, then xxhash64 of the u64). No alloc, no sync -
  replaces `materialize_string_fingerprints` (alloc + extra pass). Good change; variable-length
  char reads are uncoalesced but unavoidable.
- Deviations: none in the kernel itself. Tunables worth a microbenchmark: F5 (ConditionalAdd),
  F6 (bits/key).

### 2.2 Partial lease + zero fill - `lease_partials` (`accumulation_partial.cuh:167-209`) (2569, generalised from main)
- Per GPU: new `rmm::cuda_stream` (cudaStreamCreate), one `rmm::device_buffer` per array,
  `cudaMemsetAsync` per array when `zero_fill`, one `init` event. No sync.
- Multi-GPU: **every GPU gets a full-size copy of every array** (bloom: full filter x N GPUs).
- Deviation (G9/G1): stager passes `zero_fill=true` (`exact_key_stager.cu:120`) although key
  buffers are fully overwritten by slices; only the 4-byte overflow words need zero -> F4.

### 2.3 Bloom reduce + replicate - `finish_impl` (`sirius_dynamic_bloom_filter.cu:1130-1357`), `chunk_pipeline` (`:772-964`), `or_bloom_chunk` (`:307-356`) (main; 2569 made shapes per-key)
- Star topology through the root GPU. Chunk = clamp(raw/8, 2 MiB, 8 MiB)
  (`complete_build_inventory.cpp:303`, `.hpp:259-263`). 3 GB filter -> 384 chunks.
- PEER path: per chunk S `cudaMemcpyPeerAsync` (ingress, publication stream) -> `or_bloom_chunk`
  (root partial stream) -> T `cudaMemcpyPeerAsync` (egress, target streams); 2 scratch buffers,
  event-chained, **one** host sync (`:874`). Fully async - conforms to G2/G6.
- HOST_STAGED path (`:1255-1325`, what this machine takes): per chunk, per source
  `stage(D2H)+sync`, `stage(H2D)+sync`, OR; then root `D2H+sync`, T uploads, T syncs. ~2S+1+T
  `cudaStreamSynchronize` per chunk, strictly serial (3 GB, 4 GPUs: ~3800 syncs, ~10x the filter
  bytes through one host window). **Out of scope per user (host bounce handled elsewhere)** -
  recorded only because it will dominate any nsys trace of multi-GPU bloom finish; do not
  attribute that time to the kernels.
- `or_bloom_chunk`: 256 threads, grid min(ceil(words/256), 4096), grid-stride loop, `uint4`
  (16 B) loads/stores - coalesced (G3 ok), bandwidth-bound: reads (S+1) x chunk, writes 1 x.
  `size_t` (unsigned) loop counter (G8, negligible). No issue.
- Bytes moved for a B-byte filter on N GPUs: memset N*B, ingress S*B, OR (S+1)*B read + B write,
  egress T*B. Single GPU / single partition: no sources/targets, no scratch, just one stream sync.
- `or_words` (`:489-493`, main, distributed `merge_from` only): 1 thread per uint32, no grid
  stride. Not on the TPC-H single-node path; ignore.

### 2.4 Bounds - `enqueue_bounds` (`bounds_accumulator.cu:112-145`), `enqueue_add` (`:334-397`), `finish` (`:399-548`) (all 2569)
- What: per key per batch `cub::DeviceReduce::Min` + `cub::DeviceReduce::Max` over a
  `masked_bound` transform iterator, results written to a 32-byte device slot (no readback).
- Passes over keys: **2** (min, max separately). CUB calls: 4 (2 size queries + 2 runs), each
  run = up to 2 kernels (reduce + single-tile), plus 1 `rmm::device_buffer` alloc/free per key per
  batch (G4). Syncs: 0 for fixed width.
- `masked_bound`: per-row bitmask test, predicable (G10 ok); reads coalesced.
- STRING keys: `cudf::reduce` min + max (`:386-392`) with `to_string(stream)` -> **2 stream
  syncs + 2 D2H per batch per string key**, and these syncs also wait for the bloom/stager work
  just queued on the same stream. TPC-H has no string join keys -> no TPC-H impact; note only.
- `finish`: one pinned vector (`make_pinned_vector_async`), one `cudaMemcpyAsync` per
  (GPU, key) array, events, **1 `stream.sync()`** (`:439`); second sync (`:518`) only when a
  narrowed carrier needs `restore_scalar`. Then `to_scalars` (`:569-579`) = 2 cudf scalars per
  key = 4 tiny device allocs + 4 tiny H2D copies (G1: small transfers; negligible count).
- On main the zone map came from the concatenated build (cudf min/max path); now it is paid per
  PARTITION batch.

### 2.5 Exact key staging - `prepare_build_keys` / `compact_bounded` (`dynamic_filter_prepared_keys.cu:98-164`), `exact_key_stager` (`exact_key_stager.cu`) (all 2569)
- No-null column: 1 `thrust::for_each_n` (`bounded_dense_write`), coalesced read + coalesced
  write at a host-known offset; `exec_policy_nosync` -> no sync, no alloc. 1 pass.
- Nullable column: `rmm::device_uvector<size_type> ranks(rows)` (alloc, 4 B/row) +
  `thrust::exclusive_scan` (CUB scan, own temp alloc) + `for_each_n` -> 3 passes, 2 allocs, still
  no sync (the copy_if-style count readback is avoided by using host-known valid counts - good).
- Overflow: `atomicOr(overflow, 1u)` only on the error path (no contention normally).
- `try_create` (`:94-126`): 2 arrays per key per GPU (buffer + 4-byte word), both memset.
- `gather` (`:245-329`): `cudaStreamSynchronize` per contributing non-root GPU (`:279`), then
  `enqueue_replica_copy` per written slice, then **one 4-byte `cudaMemcpyAsync` per (GPU, key)**
  (`:303`, G1 batching deviation), then 1 sync (`:316`). Exact keys are L2-bounded by plan
  (`construction_plan.cpp:80-104`), so volumes are small (KBs-MBs).
- `gathered::build` (`:225-243`) -> prepared-keys ctors.

### 2.6 Hash IN-list - `sirius_dynamic_in_list_filter::construct` (`sirius_dynamic_in_list_filter.cu:218-246`) (2569 refactor; set layout from main)
- `cuco::static_set`, `double_hashing<1, xxhash_32>`, bucket 4, capacity 2-4x keys chosen from
  `cudaDeviceGetAttribute(L2CacheSize)` per construction (`:75-88`, host call, cheap).
- cuco ctor -> `clear_async` (sentinel fill kernel), `insert_async` (cg 1, default block size),
  no sync. Input is now the prepared rep column: 1 pass, random writes inside an L2-sized table.
- Column ctor (`:200-207`) -> `prepare_build_column` (`dynamic_filter_prepared_keys.cu:166-192`):
  1 rep-typed alloc + 1-3 passes; DECIMAL128 only: `membership_build_fits_rep` -> `column_values_fit`
  (minmax + sync). Not used by the PARTITION path (stager feeds the prepared ctor).
- No deviation.

### 2.7 Small IN-list - `construct` (`sirius_dynamic_small_in_list_filter.cu:170-190`) (2569)
- <= 12 keys (`sirius_dynamic_filter.hpp:414`). Now a single D2D `rmm::device_buffer` copy of the
  prepared needles (was a thrust::copy kernel with conversion). No sync. Fine.

### 2.8 Prepared bloom ctor - `build_prepared_bloom_replica` (`sirius_dynamic_bloom_filter.cu:170-183`, `:452-469`) (2569)
- cuco owning filter: ctor `clear_async` + `add_async` (full-tile fast path, no stencil). No
  production caller on the PARTITION path found (accumulated arrays are used instead).

### 2.9 Probe header (2569 delta only)
- Only build-side helpers were added; probe kernels (`run_membership_probe`, `cub::DeviceFor::Bulk`)
  are untouched by the stack.

## 3. Per-batch cost model at the build PARTITION (per key column, no nulls, fixed width)

| accumulator | kernels | passes over keys | device allocs | host syncs |
|---|---|---|---|---|
| bloom add | 1 | 1 (random 32 B scatter/key) | 0 | 0 |
| bounds (zone map) | 2-4 (Min, Max, each reduce + single tile) | 2 | 1 | 0 |
| exact stage (instead of bloom) | 1 | 1 | 0 | 0 |
| + per accumulator plumbing | - | - | 1 event create/destroy | 0 |

Sequential reads of an 8 B x 100M-row key column are ~1 ms-class; the Bloom scatter for the same
batch is the dominant kernel time (expect 10s of ms when the filter is GBs). So kernel time is
ruled by bloom add + (multi-GPU) the reduce/replicate traffic; bounds/stager matter through
launch + host-API overhead when batches are many and small.

## 4. Findings (deviation -> fix -> expected impact -> PR)

- **F1 (G9, G4) Min and Max are two reductions** - `bounds_accumulator.cu:131-144`. Fix: one
  `cub::DeviceReduce::Reduce`/`TransformReduce` over a `{lo,hi}` pair with a min-max op writing the
  slot packed as `{T lo; T hi}` (adjust `decode`), or a small custom kernel using block reduce +
  `atomicMin/atomicMax` (no temp storage, 1 launch; keep CUB for `__int128`). Halves bounds passes
  and launches, removes 2 of 4 CUB dispatches, can drop the per-call temp alloc. Impact: small per
  batch (~1 pass + ~2 launches + 1 alloc per key); visible only with many small batches. **PR 2569.**
- **F2 (G1) string bounds sync twice per batch** - `bounds_accumulator.cu:386-392`. Fix: keep
  string min/max on device per batch (or reduce once at finish) and read back in `finish`'s single
  sync. Impact: none on TPC-H (no string join keys); removes a hidden sync point that otherwise
  serializes the PARTITION task behind its own bloom add. **PR 2569.**
- **F3 (G1) overflow words: one 4-byte allocation, memset and D2H per (GPU, key)** -
  `exact_key_stager.cu:117`, `:303`. Fix: one contiguous `uint32[keys]` array per partial -> 1
  alloc, 1 memset, 1 D2H per GPU. Impact: micro (saves ~3 CUDA calls per key per GPU). **PR 2569.**
- **F4 (G9) stager zero-fills key buffers that are fully overwritten** -
  `exact_key_stager.cu:120` + `accumulation_partial.cuh:196`. Fix: zero only the overflow words
  (per-array zero_fill flag). Impact: saves N_gpu x valid_rows x rep bytes of memset; small because
  exact lists are L2-bounded. **PR 2569.**
- **F5 (tunable) `ConditionalAdd=false`** - `dynamic_filter_fused_probe.cuh:76` (type from main,
  used by 2569's add path). For duplicate-heavy build keys a read-before-atomic skips most atomics.
  TPC-H builds are mostly unique PKs -> expect ~0 or slightly negative; microbenchmark with unique
  vs duplicated keys before touching. Changing the policy type changes probe ref types too.
  **Neither PR as-is; if adopted, 2569.**
- **F6 (G9, tunable) 16 bits/key** - `bloom_sizing.hpp:33` (main). Filter bytes scale every
  multi-GPU cost linearly (memset x N, ingress x S, OR, egress x T) and set probe cache behaviour.
  12 or 8 bits/key cuts that traffic 25-50% at higher FPR (8 pattern bits stay). Needs the full
  TPC-H run to judge (FPR vs probe/transfer savings). **Pre-existing knob; evaluate on the stack
  tip, land separately or in 2570 (it owns "every filter at PARTITION" cost).**
- **F7 (G2) 2-8 MiB transfer chunks** - `complete_build_inventory.hpp:259-263`. A 3 GB filter =
  384 chunks x (S+T copies + 1 kernel + ~10 event calls) on the PEER path, and 384 x ~10 syncs on
  the HOST path. Larger chunks (32-64 MiB; scratch = 2 x S x chunk) cut per-op overhead ~8x.
  Impact: PEER path low (~10s of ms on a GB-scale filter); HOST path large but out of scope.
  **Constants introduced with per-key shapes in 2569.**
- **F8 (G6, G4) per-accumulator streams/events** - each accumulator leases its own
  `accumulation_partial` set (`build_filter_construction.cpp:226-234`): 3 streams per GPU per join,
  and per batch 3x (`cudaStreamGetDevice`, event create/record/destroy, 2 wait-events). Fix: one
  partial stream + one `done` event per contribution shared by bloom/stager/bounds. Impact: ~20
  fewer CUDA API calls per batch; micro unless batch counts are high. **PR 2569.**
- **F9 (G4) temp alloc per reduction** - `bounds_accumulator.cu:135`. Subsumed by F1; otherwise
  size once per task and reuse. **PR 2569.**
- **F10 (not a kernel issue, largest lever) full-size per-GPU partials + star reduce** - with N
  GPUs a B-byte filter costs N x B memset + (S+T) x B inter-GPU bytes even on the PEER path. Only
  GPUs that actually contributed are ingested (`contributed` flag, good). Kernel-side there is
  nothing to shave; the lever is fewer bytes (F6) or fewer contributing GPUs per build, i.e.
  operator/plan level. **2570 territory (placement of construction).**
- Conformant / no action: bloom add launch config and blocked layout; multiply-shift block index;
  `or_bloom_chunk` (coalesced uint4, grid-stride); stager nosync thrust policies and host-known
  counts (no count readback); in-list set build; small in-list D2D copy; single finish sync on
  the PEER path; pinned readback buffers in bounds/stager finish.

## 5. Suggested microbenchmarks (nsys/ncu) to confirm

1. Bloom add throughput vs filter size (20 MB / 400 MB / 3 GB), int32 vs int64, `add_if` (stencil)
   vs `add`, ConditionalAdd on/off with unique and 10x-duplicated keys. ncu: L2 hit rate, atomics.
2. Bounds: current Min+Max vs single minmax reduce vs custom atomic kernel, at 1M / 10M / 100M rows
   (shows the launch-overhead vs bandwidth crossover).
3. Contribution fixed overhead: empty-ish batches (1K rows) through bloom+bounds and
   stager+bounds; count CUDA API calls and wall per batch (F1/F3/F8).
4. Multi-GPU finish on the PEER path only if a peer-capable pair exists; otherwise exclude the
   HOST-staged loop from kernel attribution (user-excluded).
5. Full TPC-H with 16 vs 12 vs 8 bits/key (F6) using quent/nsys ranges
   `dynfilter::accum::contribute|finish|late_wait`, `dynfilter::bounds::*`, `dynfilter::stage::*`.
