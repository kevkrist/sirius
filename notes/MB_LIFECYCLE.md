# MB_LIFECYCLE: DF construction lifecycle microbenchmarks (MB0, MB2, MB3, MB4, MB5, MB7)

Date 2026-11-18. Code: worktree `/opt/dlami/nvme/dfperf/wt-mb-life`, branch `mb/lifecycle`
(commit `a5b37728` on top of pr2570 `ae939824`, local only).
- Benchmark: `test/cpp/operator/bench_dynamic_filter_lifecycle.cpp` (Catch2
  `[!benchmark][dynamic_filter][mbN]`, registered in `cmake/sirius-test-sources.cmake`).
- One prototype hook in src: `SIRIUS_PROTO_BLOOM_CHUNK_MIB` overrides the largest Bloom transfer
  chunk (`src/op/dynamic_filter/complete_build_inventory.cpp`, `accumulated_bloom_chunk_bytes`).
- Scripts: `/opt/dlami/nvme/dfperf/mblife/{env.sh,build.sh,run.sh,nsys_batch.sh,tab.py,api.py,api8.py}`.
  Raw output: `/opt/dlami/nvme/dfperf/mblife/out/*.log` (`ROW,...` lines), nsys:
  `/opt/dlami/nvme/dfperf/mblife/nsys/*.sqlite`.

## What was run

All under `flock /opt/dlami/nvme/dfperf/machine.lock`. Release build. Every number is the median
of 7 repetitions after 1 unreported warm-up (G=4 MB4: 3 repetitions). Median and minimum were both
recorded; they differ by < 5% in almost every row (raw logs), so run-to-run noise inside a process
is small. Each sweep ran once (one process per GPU set); no cross-process repeat was done.

| Label | GPUs visible | Cases |
|---|---|---|
| `g1_all` | 0 | MB0, MB2, MB3, MB4, MB4x, MB5, MB7 at G=1 |
| `g2_all` | 0,1 (P2P pair, PEER path) | MB2, MB3, MB4, MB4x, MB5 at G=2 |
| `g4_all` | 0-3 | MB2, MB3, MB4x, MB5 at G=4 |
| `g4_mb4` | 0-3 | MB4 at G=4 (HOST-staged: report only) |
| `ns_*` | 0 / 0,1 | nsys `--trace=cuda,nvtx` of MB2 (1 key, 32 rows; 1 / 8 / 512 batches; 1 / 5 reps) |

API used:
- Session level (MB2, MB4x, MB5, MB7): the production `dynamic_filter_publication_session`
  (`begin` -> `contribute` -> `publish_if_final`), the path PARTITION uses, through `acc::fixture`.
  Kinds: `bloom` (plan `bloom_only`), `auto` (production choice: exact IN-list while it fits the
  L2 fraction 0.125, else Bloom), `bloomzone` (Bloom + zone map). INT64 keys.
  `enq` = the `contribute` call, `wait` = the task's stream sync after it (a task does this sync
  with or without DF), `publish` = `publish_if_final`.
- Builder level (MB3, MB4): `accumulated_bloom_builder::try_create / enqueue_add / finish`, so the
  Bloom bytes are chosen independently of rows.
- One-shot shape (MB5): column constructor of the same filter class the session published, on GPU 0
  over the concatenated build, stream sync, then `replicate_to_devices`. Concatenation not timed.

Caveats (unverified / limits):
- Memory comes from the test fixture's allocator (`initialize_memory_manager`, cuCascade default
  upstream). nsys shows `cudaMallocFromPoolAsync` at 27-67 us per call here. If the production
  allocator differs, lease/alloc costs in `begin` and `publish` differ too. Not checked.
- No task reservation is attached (default allocator), unlike a real Publishing Task.
- nsys captures with 512 batches lost CUDA events (fractional per-batch counts); the API counts
  below use the 1-batch and 8-batch captures, which give integer counts. NVTX `dynfilter::*`
  ranges do appear under nsys (confirms HYPOTHESES C6).
- ncu was not run: no kernel matters in these phases (kernel work is MB1's subject).
- Quent telemetry: the unit-test binary wrote no session dirs (0 moved).
- The 4-GPU numbers go through HOST staging for Bloom (and the exact/zone replica copies cross
  non-P2P pairs): reported, excluded from conclusions.

## MB0: primitive prices (GPU 0, us per operation)

| Operation | us |
|---|---|
| tiny op + host wait (`memset 4 B` + `stream.sync`) | 5.5 |
| 4-byte D2H + host wait | 7.4 |
| idle `stream.sync` | 0.26 |
| stream create + destroy | 3.8 (1.2 ms under nsys) |
| event create + record + destroy | 0.46 |
| event record + `cudaStreamWaitEvent` | 0.38 |
| `cudaStreamGetDevice` | 0.03 |
| 4 KiB alloc + free | 0.77 |

So one host wait in a finish chain costs about 5-8 us when the queued work is tiny.

## MB2: fixed cost per contribution

Per-contribution cost does not depend on rows (1 / 32 / 1000), batches (1..512) or G (1 / 2 / 4):

| Kind | enq us, 1 key | 2 keys | 4 keys | task wait us |
|---|---|---|---|---|
| bloom | 4.7 (G=4: 6.1) | 8.3 (10.0) | 15.3 (17.1) | 6.5-8 |
| auto (exact) | 5.2 (6.6) | 8.7 (10.3) | 15.6 (17.2) | 5.5-6.5 |
| bloom + zone map | 15.0 (18.1) | 27.4 (30.5) | 51.0 (55.1) | 5.5-6.3 |

Model: about 1.4 us per batch (session: claim, mutex, facts) + 3.5 us per key (Bloom or exact),
+ 10 us per key for bounds. 512 batches of a 1-key Bloom join = 2.4 ms of host time in total.

CUDA API calls per contribution, 1 key (nsys, 8-batch minus 1-batch captures, integer counts):
1 kernel launch, 1 event create + 1 record + 1 destroy, 2 `cudaStreamWaitEvent`,
1 `cudaStreamGetDevice`; no alloc, no host wait (the 1 `cuStreamSynchronize` is the task's own).
Priced with MB0 the event/wait/get-device calls are about 1.3 us of the 4.7 us; the rest is the
kernel launch and host bookkeeping.

Per attempt (1 batch, 1 key, nsys): Bloom G=1: 7 event creates, 1 stream create, 1 lease alloc,
1 memset, 2 host waits in publish. Exact G=1: 3 kernels, 3 allocs, 3 host waits. Exact G=2: 4
kernels, 6 allocs, 4 host waits, 1 peer copy.

Whole-attempt fixed cost, 1 batch of 1 row (us):

| Kind, keys | G=1 begin / publish | G=2 (PEER) begin / publish | G=4 begin / publish |
|---|---|---|---|
| bloom, 1 | 8 / 10 | 17 / 25 | 33 / 53 |
| bloom, 4 | 13 / 10 | 288 / 37 | 469 / 131 |
| exact (small IN-list), 1 | 10 / 23 | 79 / 97 | 270 / 329 |
| exact, 4 | 245 / 204 | 1180 / 744 | 1798 / 1053 |
| bloom + zone, 1 | 14 / 60 | 124 / 504 | 363 / 1478 |
| bloom + zone, 4 | 25 / 180 | 989 / 2324 | 1902 / 4587 |

(begin varies 2-3x between neighbouring rows; allocator calls dominate it.)

## MB3: begin (Bloom lease + zero-fill + streams), builder level

| G | keys | host time of `try_create` (us), 1 MB .. 12 GB | zero-fill GPU time, async |
|---|---|---|---|
| 1 | 1 / 2 | 8-11 / 16-19 | 0.69 ms per GB per GPU |
| 2 | 1 / 2 | 18-22 / 35-41 | (12 GB: 8.4 ms; 3 GB: 2.1 ms; |
| 4 | 1 / 2 | 39-44 / 75-82 | 1 GB: 0.7 ms; 128 MB: 0.09 ms) |

- Host cost is about 9-10 us per key per GPU and independent of bytes (1 MB to 12 GB).
- The zero-fill does not block `begin`; it runs on each partial's stream at about 1.45 TB/s and
  only delays that GPU's first insert (or the merge) by 0.7 ms per GB.
- Destroy: 4-7 us (G=1) to 26-33 us (G=4).
- Session-level `begin` (MB2/MB5, includes planning + all accumulators): 7-100 us at G=1,
  30-700 us for Bloom at G=4, up to 2.0-2.4 ms for 4 exact or 4 zone-map keys at G=4.

## MB4: finish

Bloom finish, builder level, 1 key (`finish` call wall time, ms). "src" = contributing GPUs.

| Bytes | G=1 | G=2 PEER, src=1 | G=2 PEER, src=2 | G=4 HOST, src=1 (excluded) | G=4 HOST, src=4 (excluded) |
|---|---|---|---|---|---|
| 1 MB | 0.004 | 0.044 | 0.090 | 0.086 | 0.279 |
| 16 MB | 0.004 | 0.37 | 0.51 | 1.08 | 3.37 |
| 128 MB | 0.003 | 2.57 | 3.03 | 7.63 | 23.2 |
| 512 MB | 0.004 | 10.1 | 11.4 | - | - |
| 1 GB | 0.004 | 20.3 | 22.5 | 60.9 | 185.6 |
| 3 GB | 0.005 | 60.9 | 67.4 | 182.8 | 556.8 |
| 12 GB | 0.007 | 241 | 270 | - | - |

- G=1: 3.5-7 us at any size: nothing to merge or copy.
- PEER: linear, 20 ms per GB per target (about 50 GB/s, the P2P link). A contributing non-root
  GPU adds only about 10% (its ingress overlaps the egress). q3-like 294 MB: about 6 ms.
- HOST staged (excluded): 60 ms/GB with one source and 3 targets, 181 ms/GB with 4 sources.

Chunk size (prototype override; default clamp is 2-8 MiB), finish ms:

| Case | 8 MiB | 32 MiB | 64 MiB |
|---|---|---|---|
| G=2 PEER, 3 GB, src=1 | 60.9 | 59.8 | 60.1 |
| G=2 PEER, 3 GB, src=2 | 67.4 | 64.6 | 66.0 |
| G=2 PEER, 12 GB, src=2 | 270.1 | 259.6 | 260.8 |
| G=2 PEER, 128 MB, src=2 | 3.03 | 3.13 | 3.12 |
| G=4 HOST, 3 GB, src=4 (excluded) | 556.8 | 534.8 | 531.4 |

Larger chunks: 0-4% on PEER (inside or near noise), 4-5% on HOST.

Exact slices (MB4x, session level, 1 key, `publish_if_final` us; total rows fixed, split into
1 / 64 / 512 batches; "spread" = batches round-robin over the GPUs):

| G | placement | 32768 rows: 1 / 64 / 512 slices | 512 rows: 1 / 64 / 512 slices |
|---|---|---|---|
| 1 | GPU 0 | 77 / 79 / 84 | 42 / 77 / 82 |
| 2 PEER | all on GPU 0 | 242 / 242 / 255 | 85 / 148 / 213 |
| 2 PEER | spread | 232 / 367 / 1513 | 103 / 351 / 1400 |
| 4 | all on GPU 0 | 471 / 508 / 489 | 211 / 319 / 406 |
| 4 | spread | 488 / 1200 / 5798 | 267 / 1006 / 5422 |

- Local slices are free. Each slice on a non-root GPU costs about 4.9 us per key on the PEER pair
  ((1513 - 255) / 256) and about 13.8 us per key at G=4 ((5798 - 489) / 384, partly HOST staged).
  Two keys double it (G=2 spread 512 slices: 3.0 ms; G=4: 11.5 ms; MB2 4 keys at G=4: 21-30 ms).
- Coalescing slices per source GPU would cut a 512-batch, 1-key exact publish from 1.5 ms to
  about 0.25 ms on PEER.

## MB5: small build end to end against the one-shot shape

us; "tip" = begin + sum(enq) + sum(task wait) + publish; "shot" = constructor + replicate.
1 key, kind `auto` (what production would pick):

| G | rows | batches | published | tip | of which task waits | shot | tip - shot |
|---|---|---|---|---|---|---|---|
| 1 | 12 | 1 | small IN | 47 | 7 | 16 | +31 |
| 1 | 1e3 | 16 | hash IN | 294 | 100 | 67 | +227 |
| 1 | 1e3 | 128 | hash IN | 1582 | 809 | 62 | +1520 |
| 1 | 1e5 | 1 | hash IN | 105 | 5 | 69 | +36 |
| 1 | 1e7 | 1 | bloom | 242 | 208 | 196 | +46 |
| 1 | 1e7 | 128 | bloom | 1866 | 1222 | 190 | +1676 |
| 2 PEER | 12 | 1 | small IN | 136 | 6 | 37 | +99 |
| 2 PEER | 1e3 | 16 spread | hash IN | 436 | 97 | 113 | +323 |
| 2 PEER | 1e5 | 128 spread | hash IN | 2132 | 799 | 216 | +1916 |
| 2 PEER | 1e7 | 1 | bloom | 692 | 205 | 601 | +91 |
| 2 PEER | 1e7 | 16 spread | bloom | 1123 | 317 | 598 | +524 |
| 4 | 12 | 1 | small IN | 322 | 6 | 92 | +230 |
| 4 | 1e3 | 16 | hash IN | 724 | 98 | 188 | +535 |
| 4 | 1e3 | 128 spread | hash IN | 3396 | 760 | 201 | +3195 |
| 4 | 1e5 | 1 | hash IN | 1000 | 6 | 510 | +490 |
| 4 | 1e7 | 1 | bloom | 1590 | 205 | 2142 | -552 |
| 4 | 1e7 | 16 on GPU 0 | bloom | 1814 | 327 | 2140 | -326 |
| 4 | 1e7 | 16 spread | bloom (HOST ingress, excluded) | 4580 | 308 | 2150 | +2431 |

Forced Bloom, tiny builds: G=4, 12 rows, 1 batch: tip 144 vs shot 166 (tip faster); G=2: 69 vs 58.
Two keys, 16 batches, 1e5 rows, G=4: tip 2462 vs shot 1113 (+1.35 ms).

- The ratio is large (2x to 100x) but the absolute difference is 0.03-0.5 ms per join for builds
  of up to 16 batches, and at most 1.5-3.2 ms for 128 tiny batches (about half of that is the
  per-batch task sync, which a task pays anyway, and enqueue at 5-7 us per batch).
- For a 10M-row Bloom whose batches sit on one GPU, the tip is faster than the one-shot shape at
  G=4 (1.6 vs 2.1 ms), because the one-shot `replicate_to_devices` is slower than the builder's
  egress.
- "Single-GPU-inventory fast path" estimate = session at G=1 + one-shot replicate. For 1e3 rows,
  16 batches at G=4: 294 + 117 = 411 us against 724 us measured: saves about 0.3 ms per join.
  For exact keys, 1 batch at G=4: saves 0.2-0.5 ms per join. Not measured as real code.

## MB7: bounds (zone map) against rows, G=1, 1 batch

| rows | bloom total us | bloom + zone total us | delta |
|---|---|---|---|
| 1e6 | 57 | 165 | +108 (fixed: begin +20, enq +36, publish +69) |
| 1e7 | 234 | 351 | +117 |
| 1e8 | 12532 | 13688 | +1156 (about 1 ms per 100M rows, +9%) |

Zone maps are off in the campaign config, so this is informational only.

## Headroom per prototype variant (estimates from the measurements above)

| Variant | Headroom | Basis |
|---|---|---|
| Single wait for the finish chain | 6-20 us per join (1-3 waits saved at 5.5-7.4 us) | nsys wait counts x MB0 |
| Lazy lease / no zero-fill on pure targets | host: about 10 us per key per GPU; GPU: 0.7 ms per GB per target, async | MB3 |
| Shared stream + event per contribution | <= 1.3 us per key per batch (0.7 ms per 512 batches) | nsys counts x MB0 |
| Coalesced exact slices per source GPU | 4.9 us per remote slice per key on PEER (1.26 ms per key at 512 batches); 13.8 us at G=4 | MB4x |
| Single-GPU-inventory fast path | 0.2-0.5 ms per small join at G=4 | MB5 estimate |
| Larger Bloom chunks (32-64 MiB) | 0-4% of PEER finish (<= 10 ms at 12 GB); 4-5% on HOST (excluded) | MB4 |

None was implemented as real code; the chunk override is the only one measured directly.

## Verdicts

| Hyp. | Verdict | Numbers |
|---|---|---|
| H03 forward path vs one shot | Killed as a time item. Real but tiny: +0.03-0.5 ms per join (<= 16 batches), +1.5-3.2 ms at 128 tiny batches; tip is faster for a 10M-row Bloom at G=4 | MB5 |
| H06 serial finish chain | Killed on G=1 and PEER for small builds: 2-4 host waits of 5-8 us each; publish is 10-250 us at G=1/2. Bloom finish on PEER is link-bound (20 ms/GB/target), not wait-bound | MB0, MB2, MB4 |
| H07 eager leases | Killed for time: begin host cost about 10 us per key per GPU, flat from 1 MB to 12 GB; zero-fill 0.7 ms/GB async. The memory side (unspillable G x B) is not addressed by these benchmarks | MB3 |
| H08 many small batches | API overhead killed (5-17 us per batch, no allocs, no waits). Slice part supported but small: 4.9 us per remote (batch, key) slice at gather on PEER; only matters for exact keys with hundreds of batches spread over GPUs | MB2, MB4x |
| H12 chunk size | Killed on PEER: 0-4% from 8 to 32/64 MiB | MB4 |
| H14 `_decision_mutex` across begin | Killed: begin is 0.01-2.4 ms, typically < 0.7 ms at G=4 | MB2, MB3 |
| H02 (context) | Supported as the only lever that scales: PEER finish = 20 ms per GB per target, so halving Bloom bytes halves it (3 GB: 61 -> 30 ms) | MB4 |

Net: for one join the whole lifecycle overhead outside the insert kernel and the cross-GPU Bloom
copy is below about 3.5 ms in the worst measured small-build case and usually below 1 ms. The costs
that scale are Bloom bytes at finish (PEER: 20 ms/GB/target) and, far behind, remote exact slices.

## Reproduce

```bash
R=/opt/dlami/nvme/dfperf
flock $R/machine.lock $R/mblife/env.sh $R/mblife/build.sh
flock $R/machine.lock $R/mblife/run.sh g2_all 0,1 "[mb2],[mb3],[mb4],[mb4x],[mb5]" \
  SIRIUS_BENCH_DF_GPUS=2 SIRIUS_BENCH_DF_MB=1,16,128,512,1024,3072,12288
python3 $R/mblife/tab.py $R/mblife/out/g2_all.log MB4 gpus,sources,mb,chunk_mib,transport,finish_us
BLIST=8 flock $R/machine.lock $R/mblife/nsys_batch.sh && python3 $R/mblife/api8.py g1_bloom g2_auto
```
Sweep knobs: `SIRIUS_BENCH_DF_{REPS,GPUS,KINDS,KEYS,ROWS,BATCHES,MB,CHUNK_MIB,TOTAL_ROWS}`.
