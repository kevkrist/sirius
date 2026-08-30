# VALIDATION — PROTO-A (P11 descriptor packing + P2 read/H2D overlap)

Branch `proto-a-decode-overlap`, base `1ad67e7f`. Two benchmarkable stages plus one teardown fix:

- **Commit 1 (stage 1, P11)**: `perf(scan): pack decode descriptor uploads into one pinned blob per split (P11)`
- **Commit 2 (stage 2, P2)**: sub-batched read -> H2D overlap + event-gated pinned-staging release
- **Commit 3 (fix)**: noexcept swallow-and-leak teardown for the two thread-local caches (see
  "R2' exit-abort fix" below); benchmark either at commit 2 or commit 3 — the fix is
  teardown-only and does not touch any measured path.

Nothing here was run on a GPU by the implementer. Everything below is what the orchestrator
should run and what outcome proves each path fired.

## R2' exit-abort fix (commit 3)

Observed by the orchestrator: R2' (gpu-tier pin) processes aborted AT EXIT
(`__call_tls_dtors -> _Unwind_Resume -> __cxa_call_terminate`) — an exception escaping a
thread-local destructor during process teardown. The CUDA runtime calls in those destructors are
C APIs and cannot throw; the throw-capable path was `~deferred_staging_release` destroying its
entries, whose cucascade releases (`multiple_blocks_allocation` -> FSMR chunk return,
`reservation` -> arena notify) run C++ code that can throw (or hit UB that surfaces as a throw)
once the memory managers have begun tearing down. Fix, per the coordinator directive:

- `deferred_staging_release::~deferred_staging_release()` (duckdb_native_decoder.cpp) is
  `noexcept`, deliberately LEAKS the entries' blocks + reservations (`unique_ptr::release()`),
  and `(void)`-swallows the event destroys. Entries only survive to thread exit on the shutdown
  path (every next split drains them first), so the leak is shutdown-only in practice; the OS
  reclaims at process exit. Normal-lifetime releases stay in `drain()` / `reap_completed()`.
- `pinned_slab_pool::~pinned_slab_pool()` (decode_descriptor_arena.cu) is `noexcept` with every
  CUDA call `(void)`-swallowed; on `cudaErrorCudartUnloading` the slab leaks deliberately.

Re-run to confirm: the R2' validation suite plus one R2' block — every process must exit rc=0.
Residual to watch: if any R2' pin-serving thread exits MID-lifetime (not at shutdown) after
running native decode splits, its last split's staging blocks leak from the 196 GB pinned pool
(bounded at one split per thread exit). The clean structural home for that case is a
process-lifetime registry owned by `SiriusContext` (normal teardown frees while the managers are
alive, TLS destructor stays as the swallow-and-leak fallback) — `sirius_context.*` is outside
this contract's owned surface, so it is left to the hunt lead as a follow-up.

## What to run

### Catch2 (build `pixi run make test`, or run the unittest binary directly)

| Tag / filter | Covers | Proves |
| --- | --- | --- |
| `"[scan][decode]"` | whole decode family incl. all pre-existing codec tests, now routed through prepare/flush/launch | stage-1 restructure is behavior-preserving |
| `"[scan][decode][arena]"` (NEW, stage 1) | arena round-trip, slab growth mid-staging, one arena shared by multiple prepared tables with mixed codec runs, stage/flush protocol errors | descriptor-blob correctness across mixed codec runs |
| `"gpu_decode_table - zero validity runs skip the null count*"` (NEW, stage 1, in `[scan][decode]`) | null-count skip semantics: all-zero counts without the batched call; mixed nullable/non-nullable still exact | zero-validity-run `batch_null_count` skip |
| `"[scan][decode][overlap]"` (NEW, stage 2, CPU-only, no GPU pool needed) | `partition_read_sub_batches` (boundaries, forced-small sub-batches, single/empty), `await_read_sub_batches` success ordering, **A1 forced-short-read** (mid-split short read with later sub-batches in flight joins every outstanding future, runs the fail-safe, and only then unwinds — modeled with real `sirius::exec::semi_future` objects consumed exactly as production does), H2D-enqueue-failure variant | A1 join-before-unwind protocol |
| `"[scan]"` strings/rle/bitpacking/alp tags (pre-existing) | per-codec entry points kept as wrappers (`decode_rle_data` etc.) | codec-level compatibility |

The real event-gated release and the sub-batched IO path have no GPU-free harness at 1ad67e7f
(the file-read lane requires a `SiriusContext` + host FSMR spaces); they are exercised by any
SQLLogic scan over a file-backed DB below. The CPU-only `[overlap]` tests pin the partition
arithmetic and the failure protocol those paths delegate to.

### SQL / end-to-end

- `test/sql/tpch-sirius.test` (and the usual SQLLogic scan suites) on a file-backed DB: every
  native scan split now takes the packed-descriptor path (stage 1) and, when it reads from disk,
  the sub-batched overlap path (stage 2). Results must be byte-identical to `1ad67e7f`.
- Path-fired evidence in nsys: stage 1 — the per-decode-window swarm of small pageable
  `cudaMemcpyAsync` H2Ds (~4.5 ms avg each at q19) collapses to ONE pinned H2D per split (plus
  DICT_FSST's internal round trips), and the per-varchar `cudaStreamSynchronize` after descriptor
  upload disappears; stage 2 — the NVTX range `native_reads_h2d_overlap` replaces
  `native_reads` + `native_h2d`, per-split `cudaMemcpyBatchAsync` count rises from 1 to
  ~`ceil(on_disk_bytes / 512 MiB)`, and the post-H2D `cudaStreamSynchronize` in the submit path is
  gone.

### A/B plan (two benchmark points)

1. **Baseline**: `1ad67e7f`.
2. **Stage 1 only**: commit 1 (`cf2c1d54`). Expected R1' q19 ~-0.1 standalone, -0.25..-0.35
   post-P1 (reactors-8) per the amended P11 prediction (A2 discount applied: the exact-chars
   sizing sync is NOT counted as removable — hot 8 GB-split string columns exceed the existing
   512 MiB stat-bound gate and keep the sync).
3. **Stage 1+2**: commit 2 (branch head). Expected R1' increment -0.6..-0.95 over reactors-8 per
   P2. Per the review's INFO note, also record the per-split sub-batch count and the per-call
   `cudaMemcpyBatchAsync` submit cost (P12's ~21 ms blocking-submit finding): if the extra batch
   submits eat the win, lower the sub-batch count by raising `kReadSubBatchTargetBytes`
   (decoder.cpp constant, currently 512 MiB) — it is deliberately a constant, not a knob.

## Byte-identity

Both stages change only WHEN bytes move (scheduling/packing), not what is decoded, in which
order splits emit, or row/batch order. No intentional row/batch-order change anywhere. One
defensive-path nuance: BITPACKING/ALP/ALPRD now validate `type_size` before checking for an
empty run, so an empty run with an invalid width throws where it silently returned before
(unreachable from the walker-validated production path; RLE already threw first).

## Sync-count accounting (per split, steady state)

Baseline at `1ad67e7f`:
1. read `fut.get()` (host wait on IO, whole split)
2. post-H2D `cudaStreamSynchronize` in `submit_and_await`
3. `cudf::batch_null_count` D2H sync per `gpu_decode_table` call (always)
4. per varchar column: unconditional post-descriptor-upload `cudaStreamSynchronize`
   (gpu_decode_strings.cu:240) + exact-chars sizing sync (only when the stat bound is unknown or
   > 512 MiB) + `cudf::null_count` sync (only when the column carries nulls)
5. DICT_FSST prepare: 2 inherent D2H syncs per run (headers, results)

After stage 1 (commit 1): (1), (2), (5) unchanged. (3) skipped when NO column in the call stages
validity runs (host-known; per-`gpu_decode_table`-call granularity — fw table, each array child,
each array-validity decode). (4)'s unconditional sync REMOVED — all descriptor sources are pinned
arena memory whose reuse is event-gated; the exact-chars sync KEEPS its existing 512 MiB gate
(A2); `null_count` stays conditional. Descriptor H2Ds: many pageable -> ONE pinned
`cudaMemcpyAsync` per split (+ DICT_FSST internals, A3 carve-out; + the FSST decoder array,
which turned out to be device-BUILT scratch, so its placeholder host upload was dropped
entirely rather than packed).

After stage 2 (commit 2): (1) becomes K in-order sub-batch waits totalling the same IO (each
overlapped with the previous sub-batches' H2D); (2) REMOVED — replaced by a CUDA event recorded
behind the split's last H2D enqueue, consumed by `deferred_staging_release` (never a host wait on
the split's own critical path; the next split on the same thread drains the event before taking
its host reservation, by which point it has long fired). Error path (A1): on any mid-split
failure, every outstanding read future is joined and a `cudaStreamSynchronize` covers all issued
H2Ds BEFORE unwind releases the FSMR blocks/reservation. `submit_host_only_and_await`
(reads-empty splits, incl. the T9 delta lane) is byte-for-byte unchanged.

Net: the task thread's per-split host waits go from {full read, full H2D, >=1 null-count, 1 per
varchar} to {in-order sub-batch read waits (overlapped), null-count only when nulls exist,
exact-chars only per its gate, DICT_FSST's inherent pair}.

## Lifetime reasoning the review demanded written down (A4)

The removed post-H2D sync also guaranteed the PAGEABLE `host_copies` sources
(CONSTANT/ROARING staging — `pinned_segment_bytes.owned_bytes` is a plain `std::vector` despite
the name — and `scan_info`-backed host bytes). These survive the event scheme because pageable
`cudaMemcpyAsync` H2D stages its source into driver memory before the call returns, and the
owners (the `staging_state` local in `decode_duckdb_native_split`, and `scan_info` itself)
outlive the split regardless. Stated in-code at the `host_copies` enqueue.

The `[dev-staging]` `device_buf` stays ONE allocation; sub-batch H2Ds write into it; its
stream-ordered deallocation orders after every enqueued copy/kernel even on unwind. The arena's
device blob and pinned slab follow the same pattern (stream-ordered free; event-gated slab
reuse via a thread-local pool — A5: right-sized `cudaMallocHost` slabs, NOT 64 MiB FSMR blocks;
A6: the "event join" is a deferred release, no host wait added anywhere on the task path).

## Pinned staging growth budget (stage 2)

Per task thread, at most ONE previous split's staging (blocks + reservation) is event-held while
the current split runs: `deferred_staging_release::local().drain()` runs before each new host
staging reservation. Worst case at the nightly regime (8 GB splits, on-disk bytes <= ~8 GB,
8 pipeline threads): 8 threads x (1 in-flight + 1 event-held) x ~8 GB ~= 128 GB transient upper
bound vs the 196 GB pinned pool — but the realistic figure is the contract's ~12->24 GB (on-disk
bytes per split are compression-reduced, and the held entry is released at the next split's
start, virtually always already fired). Residual liveness note for the hunt lead: an IDLE thread
holds its last split's staging until its next split (or thread exit). A thread never blocks in
the pool's no-timeout `request_reservation` while holding blocks only it could free (it drains
first), so no new self-deadlock; the residual is bounded memory retention, relevant when stacking
with PROTO-B's zero-downgrade evidence (X2 — note the stacking order used, leave-one-out).

## Interactions with drafts #1476 / #1661

Both drafts rewrite the decoder/ingestible wholesale; per the contract this work targets
`1ad67e7f` and does NOT compose with them. Collision surface if either lands first:
- `src/op/scan/duckdb_native_decoder.cpp` — both stages rewrite `submit_and_await`
  (renamed `submit_reads_and_stage_h2d`) and the decode orchestration block.
- `src/cuda/scan/*` + headers — stage 1's prepare/launch split touches every codec entry point;
  #1476/#1661's decoder rewrite would supersede the callers but the
  `decode_descriptor_arena` mechanism and the codec-level `prepare_*` entries are re-hostable.
- `test/cpp/scan/test_gpu_native_decode.cpp`, new `test_duckdb_native_read_overlap.cpp`.

## Notes / adjacent observations (not implemented)

- `decode_uncompressed_data_cub`, a `[[maybe_unused]]` alternative D2D copy path in
  gpu_native_decode.cu, was deleted during the stage-1 restructure (it embodied exactly the
  pageable-upload pattern P11 removes and had no callers). Flagging since the common rules bar
  drive-by refactors; restore trivially if wanted.
- The FSST decoder array upload in the old strings orchestrator was a placeholder (the array is
  fully built on device by `kernel_build_fsst_decoders`); it is now allocated directly. A future
  cleanup could drop `prepared_fsst::decoders` host storage entirely.
- CONSTANT data still takes per-segment broadcast launches and per-segment pageable
  `host_copies` H2Ds in `submit_reads_and_stage_h2d` — that is IMPL-C/P5's lane, deliberately
  untouched here; the batched-CONSTANT fold would now slot naturally into the arena.
- The array-validity decode still burns a throwaway BOOL8 column per ARRAY column (pre-existing
  TODO), now visible in the prepared-decode structure; exposing a mask-only decode would remove
  a `gpu_decode_table` call (and its null-count sync) per ARRAY column.
- `submit_host_only_and_await` still ends in a blanket sync; reads-empty splits (q1/q6
  l_shipdate-only projections, T9 delta) could adopt the same event-deferred pattern for a small
  win, but the contract pinned it unchanged.
