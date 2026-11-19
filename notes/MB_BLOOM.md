# MB_BLOOM: DF Bloom microbenchmarks (MB1 insert, MB6 probe + FPR)

2026-11-18. Stack tip pr2570 (`ae939824`). Worktree `/opt/dlami/nvme/dfperf/wt-mb-bloom`, branch
`mb/bloom`, commit `f5ce0a37` (local only). One GPU (`CUDA_VISIBLE_DEVICES=0`, RTX PRO 6000
Blackwell, 97.9 GB), every build and run under `flock machine.lock`. GPU idle at each suite start
(2310 MHz, 27-28 C, 0 MiB used).

## 0. What was built and run

- Binary: `wt-mb-bloom/build/release/test/bench/df_bloom_bench` (source
  `test/bench/df_bloom_bench.cu`, target added in `cmake/sirius-tests.cmake`, linked like
  `retirer_benchmark` against `sirius_core`). Standalone, not Catch2: prototypes need device code.
- Production paths (results transfer):
  - insert `prod` = `detail::accumulated_bloom_builder::try_create` + `enqueue_add` on a task stream
    (cuco `add_if_async`, validity stencil, same leases / events / reservation manager as the engine);
  - probe = `accumulated_bloom_builder::finish` then `sirius_dynamic_bloom_filter::compute_mask`
    (the single-filter path of `dynamic_filter_merge.cpp`; time includes the BOOL8 output allocation).
- Prototypes (raw array, production block layout unless noted). All of `cuco_addif, cuco_add,
  cuco_cond, cuco_v8, cuco_v8c, w64, w64_1024, w64c, w64v, pairs, sortpairs, sorted` produce
  **bit-identical arrays to `prod`** (FNV of the 16 MiB array, with and without nulls; `r_check.txt`).
  `blk64*`, `diag_*` are not interchangeable (different layout / diagnostic only).
- Method: keys generated on the GPU before each batch (untimed); timed = call + `stream.sync()`.
  Rate = median over full batches, first batch dropped. Arrays are filled completely
  (rows = array bits / bits-per-key), small arrays refilled to reach 6 batches. Keys random unique
  int64 (`mix64(index)`) unless stated; nulls = 12.5 % random; `hash_partition` = plain
  `cudf::hash_partition` MURMUR3, 32 partitions, key + 3 int64 payload columns (32 B/row),
  cuCascade GPU-space allocator.
- Repetitions / variance: one process per case. Per-batch spread inside a case is < 0.5 %
  (min/max in the raw rows). The baseline case recurs in 5 suites + a 3x A/A block:
  300 MiB 2.370-2.373 ms, 3 GiB 2.759-2.767 ms per 16M batch -> **A/A noise ~0.1-0.3 %**.
- Raw data: `/opt/dlami/nvme/dfperf/runs/mb_bloom/` (`c_*.txt` case lists, `r_*.txt` results,
  `nsys/*.sqlite`, scripts `run_cases.sh`, `prof_nsys.sh`, `prof_ncu.sh`, `show.py`, `q.py`).
- Telemetry: the binary loads no Sirius config, so no Quent session was created (script checks
  after every suite: nothing moved).

### NOT done: ncu
`ncu` fails with `ERR_NVGPUCTRPERM` (`/proc/driver/nvidia/params`: `RmProfilingAdminOnly: 1`); running
it elevated was refused by the permission system. **No hardware-counter data exists** (no L1/L2 hit
rates, memory-throughput counters, warp stall reasons). The diagnosis in section 4 rests on
differential kernels (same hash, different memory behaviour) plus nsys. It identifies the bound but
cannot split stall reasons. To get ncu numbers a human must run `prof_ncu.sh` as root or set
`NVreg_RestrictProfilingToAdminUsers=0`; the script is ready
(`prof_ncu.sh add_3072 add_if_n 1 1 suite=insert array_mib=3072 max_batches=3`,
probe: regex `membership_probe_functor`).

## 1. MB1 insert, production path

### 1.1 Array size x batch size x key width (no nulls, unique random keys, 16 bits/key)
ms per batch (median) | M rows/s. keyGB/s = rows/s x key width; sector GB/s = rows/s x 32 B.

| array | rows | batch 1M | batch 16M | batch 128M | int32 @16M |
|---|---|---|---|---|---|
| 16 MiB | 8.4M | 0.033 ms, 30,200 | 0.184 (8.4M rows), 45,600 | same | 47,500 |
| 300 MiB | 157M | 0.156, 6,420 | 2.370, 6,750 | 18.71, 6,840 | 6,860 |
| 1.5 GiB | 805M | 0.179, 5,580 | 2.732, 5,860 | 21.73, 5,890 | 5,950 |
| 3 GiB | 1.61B | 0.182, 5,500 | 2.764, 5,790 | 22.00, 5,820 | 5,880 |
| 12 GiB | 6.44B | 0.183, 5,460 | 2.778, 5,760 | 22.10, 5,790 | 5,830 |

- Out of cache the insert is flat at **~5.8 G rows/s = 46 GB/s of int64 keys = 185 GB/s of 32-byte
  sectors**, from 1.5 GiB to 12 GiB. 300 MiB is 17 % faster, in-cache (16 MiB) is 8x faster.
- Batch size: 1M-row batches lose 5 % (fixed ~14 us per contribution, section 3). 16M = 128M.
- int32 vs int64: +1-2 % for int32. (12 GiB int32 has unavoidable duplicates; irrelevant to `prod`.)
- Lease + zero fill + sync (`try_create`): 0.24 ms @300 MiB, 1.1 ms @1.5 GiB, 2.1 ms @3 GiB,
  8.4 ms @12 GiB (~1.45 TB/s memset). Per GPU.

### 1.2 Other factors (batch 16M, int64 unless noted; ms per batch | M rows/s)

| factor | 300 MiB | 3 GiB | reading |
|---|---|---|---|
| baseline `prod` | 2.371, 6,750 | 2.765, 5,790 | |
| nulls 12.5 % (`add_if` skips them) | 2.087, 7,670 | 2.437, 6,570 | time scales with valid rows (-12 %) |
| plain `add_async`, no stencil (`cuco_add`) | 2.367 | 2.756 | **0 %**: the stencil is free |
| sequential keys | 2.368 | 2.765 | **0 %**: xxhash scatters them |
| duplicates x10, `prod` | 2.371 | 2.763 | **0 %**: no read-before-write |
| int32, nulls / plain add / sequential | 2.049 / 2.329 / 2.332 | 2.395 / 2.716 / 2.722 | as int64 |

8 vs 16 bits/key at equal rows (16M-row batch):

| rows | 16 bits/key | 8 bits/key | gain |
|---|---|---|---|
| 157M | 300 MiB: 2.372 ms | 150 MiB: 1.745 ms | **-26 %** |
| 1.61B | 3 GiB: 2.763 | 1.5 GiB: 2.730 | -1 % |
| 6.44B | 12 GiB: 2.778 | 6 GiB: 2.771 | 0 % |

### 1.3 Insert with `cudf::hash_partition` (H04 overlap), ms per batch

| array | batch | insert alone | hash_partition alone | same stream | insert on 2nd stream | sum of the two |
|---|---|---|---|---|---|---|
| 300 MiB | 16M | 2.372 | 1.345 | 3.695 | 3.668 | 3.717 |
| 300 MiB | 128M | 18.72 | 10.15 | 28.85 | 28.80 | 28.87 |
| 3 GiB | 16M | 2.767 | 1.346 | 4.097 | 4.072 | 4.113 |
| 3 GiB | 128M | 22.01 | 10.15 | 32.08 | 32.09 | 32.16 |
| 12 GiB | 16M | 2.785 | 1.346 | 4.117 | 4.089 | 4.131 |
| 12 GiB | 128M | 22.17 | 10.16 | 32.30 | 32.22 | 32.33 |

- Task time = insert + scatter exactly (same stream within 0.5 % of the sum).
- **A second stream buys 0-0.7 %.** Both kernels are bound by the same memory system; running them
  side by side only interleaves them.
- The Bloom insert costs **2.0-2.2x the scatter** of a 4-column int64 table (0.17 ns/row vs
  0.08 ns/row). Wider build rows shrink that ratio; a wide or string payload was not measured.

## 2. MB6 probe + measured FPR (`compute_mask`, 64M-row probe batches, int64, 6 reps after warm-up)

| members | bits/key | array | ms / 64M rows | M rows/s | FPR (measured) |
|---|---|---|---|---|---|
| 8.4M | 16 | 16 MiB | 0.596 | 107,300 | 0.131 % |
| 8.4M | 8 | 8 MiB | 0.585 | 109,300 | 3.32 % |
| 157M | 16 | 300 MiB | 3.867 | 16,550 | 0.132 % |
| 157M | 8 | 150 MiB | 2.598 | 24,630 | 3.32 % |
| 315M | 8 | 300 MiB | 3.868 | 16,550 | 3.32 % |
| 805M | 16 | 1.5 GiB | 4.483 | 14,280 | 0.132 % |
| 805M | 8 | 768 MiB | 4.354 | 14,700 | 3.32 % |
| 1.61B | 16 | 3 GiB | 4.543 | 14,090 | 0.132 % |
| 1.61B | 8 | 1.5 GiB | 4.484 | 14,275 | 3.32 % |
| 3.22B | 8 | 3 GiB | 4.544 | 14,090 | 3.33 % |

- **Hit rate (1 % / 10 % / 50 %) changes nothing**: time within 0.2 %, FPR identical (0.1316-0.1319 %
  at 16, 3.319-3.326 % at 8). No early exit, so no data-dependent cost.
- FPR depends only on bits/key, not on array size: **16 -> 8 bits/key multiplies false positives by
  25** (0.132 % -> 3.32 % of non-matching probe rows survive).
- Probe speed gain from halving the array: +49 % at 300 -> 150 MiB, +3 % at 1.5 GiB -> 768 MiB,
  +1 % at 3 -> 1.5 GiB. Sorted probe keys: no change (4.530 vs 4.543 ms).
- Probe out of cache = 14.1 G rows/s (113 GB/s of keys), 2.4x the insert rate.
- No false negative in any run (checked: passes >= members).

## 3. nsys (3 GiB array; `nsys/ins3072`, `ins3072_1m`, `probe3072`)

| | kernel | grid x block | regs/thread | kernel ms (avg, min-max) | wall ms |
|---|---|---|---|---|---|
| insert 16M rows | `add_if_n` | 62,500 x 256 | 22 | 2.753 (2.749-2.758) | 2.770 |
| insert 1M rows | `add_if_n` | 3,907 x 256 | 22 | 0.171 (0.170-0.190) | 0.185 |
| probe 64M rows | cub `static_kernel` (DeviceFor) | 250,000 x 256 | 39 | 4.53 | 4.55 |

- One kernel per key per batch, no memcpy, no alloc on the insert path.
- CUDA API per `enqueue_add`: 1 launch (4-5 us), 1 event create/record/destroy, 2
  `cudaStreamWaitEvent`, 1 `cudaStreamGetDevice` (0.3-0.9 us each).
  `dynfilter::accum::contribute` range: **median 10-11.5 us** host time per contribution.
  Fixed cost per batch ~14 us wall including the sync used for timing.
- Launch config: 256 threads, no shared memory, 22 registers: register-limited occupancy is not a
  concern; `w64_1024` (1024-thread blocks) = `w64` to 0.1 %.

## 4. What bounds the kernels (differential kernels, no hardware counters)

Per 16M-row batch, ms (same xxhash64, same block index; only the memory behaviour differs):

| variant | what it does per key | 16 MiB (8.4M rows) | 300 MiB | 3 GiB | 12 GiB |
|---|---|---|---|---|---|
| `diag_read` | read the 32 B sector, write nothing | 0.082 | 0.991 | 1.152 | 1.162 |
| `prod` (8 threads x 1 atomicOr 32-bit) | | 0.182 | 2.373 | 2.765 | 2.779 |
| `cuco_v8` (1 thread x 8 atomicOr) | | 0.798 | 3.163 | 4.453 | 4.638 |
| `w64` (1 thread x 4 atomicOr 64-bit) | | 0.408 | 2.635 | 3.010 | 3.035 |
| `blk64` (1 atomicOr 64-bit, other layout) | | 0.120 | 2.364 | 2.763 | 2.780 |
| `diag_rmw` (non-atomic load/OR/store, racy) | | 0.231 | 2.226 | 2.711 | 2.744 |
| `w64v` (read 32 B, atomics only if bits missing) unique keys | | 0.082 | 2.353 | 2.881 | 2.920 |
| `cuco_cond` (cuco ConditionalAdd) unique keys | | 0.168 | 2.369 | 2.874 | 2.905 |
| `cuco_cond` / `w64v` / `blk64c`, duplicates x10 | | | 0.999 / 0.990 / 0.992 | 1.159 / 1.152 / 1.153 | |

Diagnosis:
1. **Insert is bound by one dirtied sector per key, not by atomics, layout or launch shape.**
   Out of cache every variant that writes the sector lands at 2.7-3.0 ms whether it issues 8
   atomics, 4, 1, or a plain non-atomic store; the read-only variant takes 1.15 ms. So
   ~1.15 ms is the random sector fetch and ~1.6 ms is the cost of making it dirty (write-back).
   The production cuco kernel (8-thread tile) is already the best of the correct variants.
2. **Probe is bound by one random sector read per key**: 64M rows in 4.54 ms = 1.135 ms per 16M,
   the same as `diag_read`. Hash, compare and the BOOL8 store are free next to it.
3. Rates relative to sequential bandwidth (memset 1.45 TB/s): insert moves 185 GB/s of sectors
   each way, probe 450 GB/s one way. Random-access bound, far from the streaming limit.
   Whether the limiter is L2 miss handling, TLB or DRAM transaction rate needs ncu.
   Indirect evidence on TLB: with keys sorted by block inside a 16M batch on a 12 GiB array (one
   key per ~24 blocks, so no sector sharing, only page locality) time drops 3.03 -> 2.57 ms:
   TLB / page locality is at most ~15 %.
4. Cache boundary: flat from 1.5 GiB to 12 GiB; 300 MiB is 15-17 % faster; 150 MiB 37 % faster;
   16 MiB 8x (insert) / 7.6x (probe) faster. Halving a GB-scale array does not change kernel speed.
5. Guide check (kernels.md G3/G5/G9/G10): coalesced key reads, 256-thread blocks, one sector per
   key, no divergence that matters. Nothing to fix in the kernel as written.

## 5. Prototype headroom (insert, 16M-row batches unless noted)

| variant | result | headroom | verdict |
|---|---|---|---|
| plain `add` when the batch has no nulls | 0 % | none | kill |
| second stream beside `hash_partition` | 0-0.7 % | none (1 GPU) | kill |
| fewer / wider atomics (`w64`, `cuco_v8`) | 9 % / 61 % slower | negative | kill |
| different block (`blk64`, 1 atomic, new layout) | 0 % out of cache | none | kill |
| wider launch (1024 threads) | 0 % | none | kill |
| ConditionalAdd on unique keys | 0 % @300 MiB, -4 % @>=1.5 GiB | negative | only with duplicates |
| ConditionalAdd / read-first, duplicates x10 | **2.4x faster** (2.77 -> 1.15 ms) | up to 2.4x on duplicate-heavy builds | keep as option; cuco policy change touches the probe ref type, `w64v`-style kernel does not |
| sort by block, insert only (`sorted`) | 4.1x @300 MiB, 2.0x @1.5 GiB, 1.4x @3 GiB, 1.07x @12 GiB | upper bound of locality | |
| sort by block incl. the sort (`sortpairs`, radix on 64-bit pairs) | 16M: 2.38 vs 2.37 @300 MiB; 2.91 vs 2.73 @1.5 GiB; 3.37 vs 2.77 @3 GiB; 128M batch: 20.6 vs 22.0 (`prod`) @3 GiB (-6 %), 24.8 vs 22.1 @12 GiB (+12 %) | <= 6 % in one corner, loss elsewhere | kill (also changes nothing for the probe) |
| 8 bits/key | insert -26 % only at 300 -> 150 MiB, 0-1 % above | none for GB arrays | see H02 |

## 6. Hypotheses

**H04 (insert is a random scatter, serial ahead of the scatter)**
- Supported: task GPU time = insert + scatter exactly; out of cache the insert is 5.8 G rows/s and
  costs 2.0-2.2x a 4-column int64 `hash_partition`.
- Size of the effect: 0.17 ns per build row. 147M rows (q3 orders) = 25 ms, 1.5B rows = 260 ms of
  GPU time summed over all GPUs. Material only where PARTITION kernel time itself is material.
- Killed: candidate 1 (second stream overlap, 0-0.7 %), candidate 2 (`add` without stencil, 0 %),
  candidate 4 (locality / bucket by block: sort costs more than it saves), any atomics / layout /
  launch change. Candidate 3 (fewer bytes) helps the kernel only below ~300 MiB.
- Left alive: ConditionalAdd-style read-first for duplicate-heavy build keys (2.4x at x10
  duplicates, free to -4 % on unique keys). Needs the duplicate ratio per build from the log pass.
  Not measured: overlap when `hash_partition` has real host gaps (strings, wide tables,
  allocator stalls), and the forward path where the insert is the task's only GPU work.

**H02 (Bloom bytes)**
- Kernel-speed argument for fewer bytes: **killed for arrays >= ~768 MiB** (insert and probe flat
  from 768 MiB to 12 GiB). Supported only for small arrays: 300 -> 150 MiB gives insert -26 %,
  probe +49 %; in-cache is 8x.
- H02a (distinct-key cap): duplicates do not slow the production insert, so the gain is bytes only
  (lease, memset, merge, replication), plus kernel speed if the cap pulls the array under ~300 MiB.
  No FPR cost. Still worth it on those grounds; byte-proportional finish costs are MB4, not here.
- H02b (16 -> 8 bits/key): FPR 0.132 % -> 3.32 % (25x) measured, independent of size and hit rate.
  Buys no kernel speed on GB arrays; buys half the bytes. Zero fill is cheap on this GPU
  (0.7 ms/GiB), so the remaining argument is merge / replication / memory only. A size-adaptive rule
  (16 bits while the array is large anyway) is not supported by kernel numbers either way: decide
  it on MB4 + full-suite false-positive cost.

## 7. Caveats / unverified
- No ncu (see section 0). Stall-reason attribution is inferred, not measured.
- Single GPU, GPU otherwise idle. In the engine other tasks share the GPU; rates are best case.
- `hash_partition` reference is the bare cudf call on a narrow all-int64 table with a warm
  allocator, not `gpu_partition_impl::hash_partition` with batch construction.
- Probe measured for int64 keys and single-filter `compute_mask` only (not the fused multi-filter
  kernel). FPR measured with random unique keys.
- `blk64` FPR not measured (not needed: no speed gain).
- One process per case; noise estimated from recurring baselines (0.1-0.3 %), not from repeating
  every case.
