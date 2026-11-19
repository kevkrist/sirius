# PROTO_OP4: filter kind for 0.2-2M-key builds (OP4) + 8 vs 16 Bloom bits per key (OP5)

2026-11-18. Worktree `/opt/dlami/nvme/dfperf/wt-op4`, branch `proto/op4-filter-kind` (local only), commits
`14b78909` (cherry-pick of mb/bloom bench) + `843cece7` (prototype). Tip binary = `$R/repo` (`ae939824`).
Every build / GPU process ran under `flock machine.lock`. Scripts, raw output: `$R/runs/op4/` (`bin/`, `mb/`, `kind_*`,
`bpk8_*`, `nsys_*`, `log_*`); Quent sessions moved to `$R/telemetry/op4/<label>/q<N>/`.

## 0. Verdict

- **OP4: drop.** No wall gain: q17 -1.4 ms (-0.3%), q8 +0.5 ms (+0.1%), q11 -1.7 ms (-1.0%), all inside noise, although
  the probe kernel time really fell (q17 -33 GPU-ms / -8.4 ms per device, q8 -36 / -10.5). The scans that carry these
  probes are bound by the host-to-GPU copy of the pinned table, not by kernels. Same on "this machine" and "bounce
  excluded" (no bounce involved: these filters are < 4 MB).
- **The "two exact-set probe rates" do not exist as a kernel problem.** All earlier rows/s figures are low by 2x (cub
  `for_each` runs 2 rows per thread), and the two groups are input shape: prior keep-mask and clustered keys.
- **OP5 (8 bits/key): gain is bounce time only.** q21 -143 ms (-9.9%), q3 -27.6 (-4.2%), q7 -22.0 (-3.0%) on this
  machine; the build-site (finish chain) shrinks by 155 / 29 / 28 ms, so everything outside the build site got
  0 to 12 ms *slower* (false positives). Bounce excluded: no gain, small loss. Do not land for speed.

## 1. (a) Why exact-set probes looked like 35 vs 72 G rows/s

### 1.1 Unit error in the earlier notes (VERIFY_transfer sec. 3, OPPORTUNITIES OP4, kernels)
`cub::DeviceFor::Bulk` uses `ForPolicy{256, 2}` (`cub/device/dispatch/tuning/tuning_for.cuh`): **2 items per thread**.
Rows = 2 x gridX x blockX. Checked on the microbench under nsys: 48M rows -> grid 93750 x block 256 = 24.0M threads.
Every production probe rate quoted before (Bloom, set, needle) must be doubled. Insert (`add_if_n`) rates are not
affected by this (different kernel; not re-checked here).

Corrected production rates (existing tip nsys captures, all `integral_probe_adapter<int,int>` + `set_lookup` unless noted;
same kernel instance in both groups, capacity factor 4 in every log line `built set:`):

| query | set keys | corrected G rows/s | shape |
|---|---|---|---|
| q17, q8, q21, q11 | 200K / 1.33M / 400K / 400K | 69-72 | random keys, no prior mask |
| q7, q19, q20 | 800K / 481K / 347-399K | 144-183 | prior keep-mask (scan predicates ran first: inferred from MB, not logged) |
| q18 (int64 set) | 63K | 156-180 | ascending `l_orderkey` / `o_orderkey`: ~4 equal keys in a row |

Kernel overlap with other kernels on the same GPU is 0-6% in both groups (`bin/overlap.py`): not a concurrency effect.

### 1.2 Microbench (`df_bloom_bench suite=setprobe`, GPU 0, 48M probe rows, 8 reps after 1 warm, one process per case)
Production launch (`detail::run_membership_probe` = `cub::DeviceFor::Bulk` + `membership_probe_functor`), stores:
`prod` = `sirius_dynamic_in_list_filter` ctor + `compute_mask`; `bloomprod` = `sirius_dynamic_bloom_filter`;
`dh64/dh32` = production `set_type` with 64 / 32-bit extent; `lp64/lp32` = linear probing; `pow2` = hand-written
power-of-two linear-probe table; `bloomset` = Bloom prefilter + exact set. Exact variants are checked row-for-row
against a reference mask; Bloom variants are checked for zero false negatives. 28 cases, 156 result rows, 0 failures,
spread inside a case < 5%. Raw: `runs/op4/mb/r_set1.txt` (cases `c_set1.txt`, table `bin/show.py`).

G rows/s, int32 key and probe, hit rate 0.2%, no prior, random keys:

| keys | set MiB | prod | dh32 | lp32 | pow2 | Bloom 16 bpk | Bloom+set |
|---|---|---|---|---|---|---|---|
| 63K | 1.0 | 72 | 70 | 72 | 99 | 197 (0.12 MiB) | 157 |
| 200K | 3.1 | 70 | 68 | 69 | 96 | 117 | 107 |
| 400K | 6.1 | 68 | 68 | 68 | 99 | 116 | 110 |
| 800K | 12.2 | 68 | 67 | 68 | 99 | 114 | 112 |
| 1.33M | 20.3 | 68 | 67 | 67 | 97 | 115 | 110 |
| 2.0M | 30.5 | 67 | 66 | 66 | 89 | 115 | 112 |

- **Set size (63K-2M) does not matter**: flat 67-72. Everything fits L2.
- **Load factor matters**: capacity factor 2 / 3 / 4 = 47 / 60 / 68 (production always got 4 in TPC-H).
- **Kernel variant does not matter**: 64 vs 32-bit modulo, double hashing vs linear probing: all within 3%. The
  64-bit-division suspicion (CUDA best-practices: avoid 64-bit integer divide) is not the limiter here.
- **Key width**: int64 set / int64 probe 67 (63K keys), same as int32.
- **Prior keep-mask** (481K keys): keep 100% / 50% / 30% / 10% / 2% = 68 / 127 / 139 / 158 / 215 (Bloom 115 / 179 /
  189 / 205 / 268). Killed rows skip the lookup. This is the q7 / q19 / q20 group.
- **Clustered keys**: int64, 63K keys, runs of 4 equal keys: 144 (int64 probe), 168 (int32 probe), Bloom 161 / 236.
  This is q18.
- **Hit rate**: 0 / 10 / 50 / 100% = 68 / 70 / 76 / 85; Bloom flat 115; Bloom+set 110 / 98 / 69 / 49.
- At equal shape an in-cache Bloom is 1.7x the exact set (115 vs 68); a pow2 table is 1.45x (99) and stays exact.
- Bloom FPR measured: 0.13% at 16 bpk, 3.3% at 8 bpk.
- nsys on the microbench (`mb/nsys_set400k.sqlite`): kernel medians 0.685 ms set, 0.693 dh32, 0.475 pow2, 0.404 Bloom,
  0.426 Bloom+set for 48M rows = 70 / 69 / 101 / 119 / 113 G rows/s; wall-clock figures above agree within 3%.
- **ncu: still refused.** `ncu --set full`:
  `==ERROR== ERR_NVGPUCTRPERM - The user does not have permission to access NVIDIA GPU Performance Counters on the
  target device 0.` (`/proc/driver/nvidia/params`: `RmProfilingAdminOnly: 1`). No sudo used. No counter data.

Production reproduces the microbench exactly: q17 with the Bloom override probes at 117 G rows/s (MB 117), tip set 71 (MB 70).

## 2. (b) Is the GPU the bound of those scan pipelines? No.

Existing tip nsys captures, last hot iteration, per device, window = first to last `set_lookup` kernel
(`bin/gpubound2.py`):

| query | window ms | kernels busy | H2D/P2P memcpy busy | set_lookup ms | of it under a memcpy |
|---|---|---|---|---|---|
| q17 | 61-117 | 15-25% | 91-95% | 10.8-14.0 | 74-84% |
| q8 | 426-454 | 8% | 97-98% | 21.4-23.3 | 86-97% |
| q21 | 191-235 | 17-19% | 95-96% | 19.8-22.8 | 84-88% |
| q18 | 175-231 | 14-20% | 74-86% | 13.5-16.7 | 71-78% |
| q11 | 67-69 | 14-19% | 67-73% | 5.6-6.1 | 0-44% |

The device receives pinned-table bytes at 28-37 GB/s for 91-98% of the window; kernels fill 8-25% and run under the
copies. Probe kernel time is not on the critical path (q11 partly: 5.6 ms per device, two scans).
Direct test: q17 and q8 with Bloom instead of the set lose 33-36 GPU-ms of probe kernel per iteration and the wall time
does not move (section 3).

## 3. (c) Kind-policy prototype

Override: env `SIRIUS_PROTO_BLOOM_RANGE=lo:hi` in `choose_membership` (`construction_plan.cpp`): a build that would get
a hash IN-list and has lo..hi valid keys gets a 16 bpk Bloom. (Note: `SET dynamic_filter_inlist_max_l2_fraction` is a
public SQL option, so fraction 0 would also work without code; the range form keeps q18's 63K-key set exact.)
Verified in a debug log pass: q17 j7 `kind=bloom size=401184`, 0 `built set` lines.

Timing, tip binary (A) vs prototype with range 100000:3000000 (B), fresh process, 1 warm + 5 hot, first hot dropped,
2 blocks per side (q17, q8 as A B B A; q11 as four independent lock waiters), median of 8 hot per side:

| q | A ms | B ms | B-A | blocks A | blocks B | spread A / B ms | result |
|---|---|---|---|---|---|---|---|
| 17 | 497 | 496 | -1.4 (-0.3%) | 497 / 497 | 496 / 496 | 19 / 19 | identical |
| 8 | 935 | 935 | +0.5 (+0.1%) | 933 / 936 | 934 / 936 | 14 / 11 | identical |
| 11 | 175 | 173 | -1.7 (-1.0%) | 175 / 176 | 175 / 173 | 13 / 15 | same rows, tie order differs (also A1 vs A2) |

nsys, one capture each (3 iterations), probe kernels per iteration:

| q | tip | Bloom override | wall under nsys |
|---|---|---|---|
| 17 | set 84.3 GPU-ms, 21.8 per device, 71 G rows/s | Bloom 51.4, 13.4 per device, 117 G rows/s | 524 / 534 vs 524 / 532 ms |
| 8 | set 87.9 + Bloom 9.5 GPU-ms, 23.3 per device | Bloom 61.2, 15.8 per device | 1049 vs 957 ms (iteration 1 only; noisy) |

False positives (debug pass, rows kept by the scan filters, 2 iterations): q17 6.28M -> 14.4M of 6.26 B (2.3x); q18
with the range widened to include it 0.71M -> 15.1M of 10.9 B (21x). Results identical in both. q18 was not timed.

Not run (lock contention, about 1 process per 4-5 min): q21 and the controls q18, q19, q7, q20; no full suite (no gain
to confirm). Not prototyped: an exact pow2 table in the engine. Its kernel gain (68 -> 99) is smaller than the Bloom's
(68 -> 115), and the Bloom's bought no wall time, so it cannot either on this configuration.

Where it could matter: a configuration without the host-pinned copy in the scan (GPU-resident or much faster ingest).
Then the kernel bound is real: per suite pass <= 123 ms per device of set probes, of which up to ~40% (Bloom) or ~30%
(pow2, exact) is recoverable where no prior mask exists. Unmeasured here.

## 4. (d) OP5: 8 vs 16 Bloom bits per key

Override: env `SIRIUS_PROTO_BLOOM_BPK=8` in `bloom_blocks_for` (`bloom_sizing.hpp`), read once per process; plan,
builder and one-shot filters all size through it. Tip (A, 16) vs prototype (B, 8), same protocol:

| q | A ms | B ms | B-A | blocks A / B | Quent build-site union A -> B ms | outside the build site |
|---|---|---|---|---|---|---|
| 21 | 1439 | 1296 | -143.0 (-9.9%) | 1445, 1435 / 1296, 1297 | 344 -> 189 (-155) | +12 |
| 3 | 660 | 632 | -27.6 (-4.2%) | 662, 660 / 631, 633 | 61.3 -> 32.1 (-29) | +2 |
| 7 | 745 | 723 | -22.0 (-3.0%) | 745, 747 / 722, 723 | 68.9 -> 41.2 (-28) | +6 |

Results identical. Quent figures are one hot iteration of one process per side (`quent_bpk8_*.json`).

Split for q21 (nsys, 1 capture per side, per iteration):
- **Bounce / finish chain**: `accum::finish` 295.5 -> 160.5 ms (-135). HOST-staged on this machine: excluded.
- **Insert kernel**: 129.3 -> 120.7 GPU-ms; busiest device 34.4 -> 32.2 ms (-2.2).
- **Probe kernel**: Bloom 87.3 -> 45.1 GPU-ms; busiest device 22.4 -> 11.5 ms (1.39 GiB -> 0.70 GiB array, 73 -> 145 G
  rows/s). Mostly hidden under the scan's memcpy (section 2).
- **False positives**: Bloom-probed rows +181M per iteration (6352M -> 6533M): rows the first filter let through
  reach the second one; 3.3% vs 0.13% FPR in the microbench.
- Net: wall -143 = finish -135, build-site remainder about -20 (insert + smaller merge), everything else +12.

Reading: on this machine 8 bpk gives q21 -9.9%, q3 -4.2%, q7 -3.0%; extrapolating half of each query's finish time to
the 9 Bloom queries not run (q4, q5, q8, q9, q10, q12, q16, q20, q22; finish sum 185 ms at 16 bpk) gives about -90 ms
more, suite about -280 ms (-1.8%), **all of it finish-chain time**. Bounce excluded and counting no finish time at
all: +2 to +12 ms per query (a small loss, at the noise edge). If the P2P-equivalent copy is counted (model in
VERIFY_methodology: q21 85-104 ms, q3 17-22 at 16 bpk), half of it would be a real gain: q21 about -45, q3 about -10,
minus the false-positive cost: a model, not measurable here. After OP1 (finish off the critical path) most of this
disappears.

Not run: the other 9 Bloom queries and the 10 queries without a Bloom (unchanged by construction), no 32 bpk, no
nsys at 8 bpk except q21.

## 5. Correctness gates

- Query results byte-identical to tip: q17, q8 (kind), q21, q3, q7 (8 bpk), q17 / q18 debug passes. q11: same row
  set, tie order differs between any two runs (known, also tip vs tip).
- No lost filters: `construction started` counts equal on both sides (q17 6/6, q8 30/30, q11 24/24, q21 24/24, q3
  12/12, q7 30/30 per process); no fallback / error lines.
- Unit tests, prototype build, no env set: `[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]` 638 cases, 141565
  assertions, all passed. With `SIRIUS_PROTO_BLOOM_BPK=8`: `[construction_plan],[dynamic_filter][construction][bloom]`
  21 cases, 1648 assertions, all passed.
- Microbench: every exact variant equals the reference mask; no Bloom false negatives.

## 6. Files changed and owning PR

| file | change | owner |
|---|---|---|
| `src/op/dynamic_filter/construction_plan.cpp` | env range override in `choose_membership` | #2569 (plan policy) |
| `src/op/dynamic_filter/bloom_sizing.hpp` | env bits-per-key override, `bloom_blocks_for` no longer constexpr | pre-existing constant; policy would be #2569 |
| `test/bench/df_bloom_bench.cu`, `cmake/sirius-tests.cmake` | `suite=setprobe` | prototype tooling |

Nothing in #2570 (session / observer / call sites) is touched. Probe kernels and `set_type` are pre-existing code.

## 7. Unverified / limits

- Kind A/B covers q17, q8, q11 only; q21 and the four controls were not timed.
- "Prior keep-mask" as the cause of the q7 / q19 / q20 rates is inferred from the microbench sweep matching the
  corrected rates; the engine does not log whether a prior mask was passed.
- GPU-bound check uses per-device interval unions from one capture per query; it shows the copies cover the window,
  not which thread waits on what.
- OP5 extrapolation to the 9 Bloom queries not run assumes finish time scales with bytes (it did for q21, q3, q7).
- Timing runs interleaved with other agents' runs under the lock; A and B blocks of one query were 10-45 min apart.
- No ncu data (permission).
- Unit tests with the kind-range override set were not run (the override only swaps one supported kind for another).
