# VERIFY_PROTO_XFER_HOST: adversarial verification of proto/xfer-host (pipelined host bounce of the DF finish)

2026-11-20. Verifier, independent of the prototype author. Subject: worktree `/opt/dlami/nvme/dfperf/wt-xfer-host`, branch
`proto/xfer-host` head `699b3dd8`, notes `PROTO_XFER_HOST.md`. **The prototype was not modified and not rebuilt**: I used the
author's binaries (built 2026-11-19 21:21; no source file is newer than `libsirius.so`; md5 in `runs/vxfer_host/out/md5_start.txt`).
Tip reference: `/opt/dlami/nvme/dfperf/repo` (`ae939824`). Config `/home/ubuntu/sirius/specific_config.yaml` untouched; only the
prototype's environment switches. Every GPU process ran under `flock $R/machine.lock`, one short script per hold
(logs of the holds: `runs/vxfer_host/chain*.log`).
Scripts `$R/runs/vxfer_host/bin/`, raw output `$R/runs/vxfer_host/{v1,w1,r1,g1,log,rep,nsys,out}`, Quent `$R/telemetry/vxfer-host*`.
This file is generated from `runs/vxfer_host/notes_template.md` by `bin/make_notes.py` (tables computed from the raw runs).

## 0. Verdict

**CONFIRMED, with small corrections.** I could not refute the gain or the correctness.

| Claim | Author | Verifier | Status |
|---|---|---|---|
| Suite, tip vs `auto` | -417.5 ms (-2.73%) | **15298 -> 14902 ms = -396.4 ms (-2.59%)** | confirmed (about 95% of the claimed saving) |
| q21 | -208.5 (-14.5%) | **-214.6 (-14.9%)**, n=8 | confirmed |
| q3 / q7 / q8 | -41.0 / -40.6 / -25.8 | **-41.5 / -42.6 / -26.0**, n=8 | confirmed |
| q10 | -33.0 (-3.9%) | **-20.8 (-2.4%)**, n=8 | weaker (each inside the other's +-10 ms noise) |
| Controls q6 / q2 | +0.2 / -3.0 | +0.1 / -1.5, n=8 | confirmed |
| Proto binary, switch off | q21 -1.9, q3 -7.5 | q21 +0.4, q3 -3.4, n=8 | confirmed neutral |
| Bloom arrays bit-identical on every GPU | 540 + 450 finishes | 7470 finishes re-checked (other GPU orders, 3 GPUs, 2 processes at once) | confirmed |
| Query results identical | 21 of 22, q11 tie order | same (q11: sorted content identical, 936989 rows) | confirmed |
| Finish wall q21 / q3 | 293-308 -> 74-91; 62 -> 22 ms | q21 291-308 -> 73-74 (own capture); q7 54 -> 14.5 | confirmed |
| Real transfer speed-up | 10B -> 8B, legs overlap | same; other copies of the query unchanged in bytes and time | confirmed |
| Microbench 186 -> 45-58 ms/GiB | 5 reps | 185.5-186.0 -> 44.2-46.7 (5 reps) | confirmed |
| DF unit tests `auto`: 4 of 159 fail | workspace expectations | same 4; `gpuor` same 4; **`hostor` 5** | confirmed + 1 more |
| "No effect on full P2P: prototype not reached" | measured G=2 | **not always true**: reached when the PEER scratch is refused (section 5.3) | corrected |
| Failure paths after enqueueing | untested | fault-injection binary run: 11 of 12 pass in `auto`, `hostor`, `gpuor` (section 5.4) | now covered for G=2 |
| Contention with a concurrent transfer | unmeasured | microbench with a second process saturating the links: hostor x2.0-2.2, tip loop x4.3 (section 6.3) | new, in favour |
| Recommended W=2, 8 OR threads | microbench only | q21, q3 in TPC-H: equal to the defaults within 2 ms | new, confirmed |
| Replica scope + batched gather in TPC-H | unmeasured | switch off (`SIRIUS_PROTO_XFER_REPLICA=0`): same times as `auto` | new: zero TPC-H effect |

What stays unverified is in section 9; production issues in section 8.

## 1. Timing: ABBA, tip binary vs prototype with `SIRIUS_PROTO_XFER_HOST=auto`

Protocol: fresh process per query, 6 iterations (1 warm + 5 hot), first hot dropped, median of the kept hot iterations.
Arms: A = tip binary, U = proto `auto` (defaults W=4, 16 OR threads, replica switch on), O = proto binary with the switch unset.
n=8 = two processes per arm, the two A/U pairs in opposite order (A U U A or U A A U inside one lock hold for q21 3 7 8 6 2; for
the other queries the two halves ran in different holds, 20-60 minutes apart). n=4 = one pair.

| q | A tip med ms (n) [min-max] | U auto med (n) [min-max] | U-A ms (%) | per-process medians A / U | author U-A | results |
|---|---|---|---|---|---|---|
| 1 | 469.2 (8) [465-480] | 476.4 (8) [471-488] | +7.2 (+1.53%) | 468 472 / 476 477 | -4.9 | identical |
| 2 | 147.8 (8) [146-151] | 146.3 (8) [145-149] | -1.5 (-1.00%) | 148 148 / 146 147 | -3.0 | identical |
| 3 | 665.6 (8) [658-755] | 624.0 (8) [618-714] | -41.5 (-6.24%) | 666 666 / 624 624 | -41.0 | identical |
| 4 | 325.6 (8) [321-329] | 310.0 (8) [307-314] | -15.5 (-4.77%) | 325 326 / 311 310 | -14.2 | identical |
| 5 | 691.6 (8) [679-714] | 679.1 (8) [664-701] | -12.4 (-1.79%) | 691 693 / 679 681 | -12.1 | identical |
| 6 | 380.0 (8) [379-386] | 380.0 (8) [379-390] | +0.1 (+0.02%) | 379 382 / 380 380 | +0.2 | identical |
| 7 | 747.3 (8) [742-751] | 704.7 (8) [699-713] | -42.6 (-5.71%) | 747 746 / 706 704 | -40.6 | identical |
| 8 | 935.8 (8) [934-939] | 909.9 (8) [904-912] | -26.0 (-2.77%) | 936 936 / 911 909 | -25.8 | identical |
| 9 | 1420.1 (8) [1407-1432] | 1410.0 (8) [1382-1432] | -10.1 (-0.71%) | 1417 1420 / 1412 1408 | -12.8 | identical |
| 10 | 850.7 (8) [846-864] | 829.9 (8) [811-839] | -20.8 (-2.45%) | 850 859 / 830 826 | -33.0 | identical |
| 11 | 171.0 (8) [168-185] | 174.9 (8) [166-186] | +3.9 (+2.28%) | 173 169 / 175 177 | +3.1 | order differs (sorted identical) |
| 12 | 701.2 (8) [699-706] | 694.9 (8) [690-700] | -6.3 (-0.89%) | 701 702 / 695 694 | -9.6 | identical |
| 13 | 1295.8 (8) [1257-1519] | 1296.2 (8) [1280-1315] | +0.5 (+0.04%) | 1302 1292 / 1291 1303 | +5.8 | identical |
| 14 | 691.9 (8) [676-708] | 688.3 (8) [677-708] | -3.6 (-0.52%) | 688 692 / 688 688 | -5.6 | identical |
| 15 | 527.7 (8) [526-530] | 527.4 (8) [527-529] | -0.3 (-0.05%) | 527 529 / 528 527 | +0.0 | identical |
| 16 | 311.4 (8) [302-315] | 308.3 (8) [297-313] | -3.1 (-0.99%) | 308 313 / 308 305 | -4.2 | identical |
| 17 | 498.3 (8) [487-509] | 497.0 (8) [488-508] | -1.3 (-0.26%) | 498 498 / 497 497 | -0.8 | identical |
| 18 | 1090.4 (8) [1072-1105] | 1087.1 (8) [1077-1102] | -3.4 (-0.31%) | 1082 1094 / 1090 1086 | -4.6 | identical |
| 19 | 881.0 (4) [876-891] | 880.4 (4) [876-888] | -0.6 (-0.07%) | 881 / 880 | +2.0 | identical |
| 20 | 716.0 (4) [701-734] | 716.6 (4) [703-732] | +0.5 (+0.07%) | 716 / 717 | -3.2 | identical |
| 21 | 1441.4 (8) [1417-1456] | 1226.9 (8) [1219-1243] | -214.6 (-14.89%) | 1441 1441 / 1230 1227 | -208.5 | identical |
| 22 | 338.6 (8) [337-340] | 333.6 (8) [333-335] | -5.0 (-1.48%) | 338 339 / 333 334 | -4.8 | identical |
| sum | 15298 | 14902 | -396.4 (-2.59%) | | -417.5 | |

Alternative estimator (mean of per-process medians): -393.4 ms.

- Suite: **15298 -> 14902 ms = -396.4 ms (-2.59%)** (author: 15280 -> 14863 = -417.5, -2.73%). Campaign references: base 15225, tip 15372 ms.
- Queries with a HOST-staged Bloom (3 4 5 7 8 9 10 12 16 21 22): -397.9 ms (author -407). The other 11 queries: +1.5 ms
  (noise: the only prototype code that can run there is the exact-kind replica copy).
- Switch off (arm O, n=8): q21 1441.8 vs 1441.4 (+0.4 ms), q3 662.2 vs 665.6 (-3.4 ms).
- Spread: per-arm min-max in the table. The slow maxima of q3 (one iteration of 700-755 ms in each of the three arms) and of q13
  (one tip iteration of 1519 ms) are single iterations; medians are not affected.
- Results: `result.txt` md5 identical across every process of every arm for 21 queries (2-13 processes per query over all my runs).
  q11: tip and proto differ in row order only (sorted content md5 identical, 936989 rows), as the author reported.

Other arms (same protocol, one lock hold each):

| Arm pair | q21 | q3 | Reading |
|---|---|---|---|
| U (W=4, 16 threads, 160 MiB pinned) vs W (`WINDOW=2`, `OR_THREADS=8`, 80 MiB) | 1228.8 vs 1227.9 (n=8 / 8) | 624.6 vs 626.2 (n=4 / 4) | the recommended configuration keeps the whole gain |
| U vs R (`SIRIUS_PROTO_XFER_REPLICA=0`: Bloom pipeline only) | 1231.8 vs 1231.0 (n=8 / 8) | 624.6 vs 620.5 (n=4 / 4) | the TPC-H gain is the Bloom pipeline alone; replica scope + batched gather add nothing here |
| G (`gpuor`) vs H (`hostor`) | gpuor 1308.6 [1295-1319] vs hostor 1236.3 [1220-1245] (n=8 / 8) | - | against the tip's 1441.4: gpuor -132.9 ms, hostor -205.2 ms; results identical: True |

## 2. Is the transfer really faster, or is waiting shifted? (nsys)

Own captures: q21 and q7, one capture per arm, 1 warm + 2 hot (`runs/vxfer_host/nsys`), analysed with my own script
`bin/vnsys.py` (not the author's `nsys_df.py`). I also re-analysed the author's q21 / q3 captures with it: same numbers.
Copy direction is taken from CUPTI `srcKind` / `dstKind` (pinned vs device memory), which is reliable where `copyKind` reads PtoP.

| | q21 tip | q21 auto | q7 tip | q7 auto |
|---|---|---|---|---|
| query wall, 2 hot (ms) | 1473, 1455 | 1258, 1259 | 775, 745 | 738, 709 |
| `accum::finish` sum per iteration (ms) | 290.7, 308.0 | 73.3, 74.2 | 54.0, 54.0 | 14.5, 14.5 |
| finish copies D2H + H2D (GiB) | 5.96 + 8.95 = 14.91 | 5.96 + 5.96 = 11.93 | 1.11 + 1.70 = 2.81 | 1.11 + 1.15 = 2.26 |
| copy-busy time per GPU inside the finish (ms) | 84, 84, 57, 115 (sum 340) | 70, 70, 70, 71 | 16, 16, 11, 22 | 13-14 each |
| host waits on the finish thread | 1932 stream syncs, 272-289 ms | 1504 event syncs 17-28 ms + 18 stream syncs 1 ms | 394 stream syncs, 50 ms | 264 event syncs 4.4 ms + 18 stream syncs 1 ms |
| CPU OR (ms) | - | 23-29 (192 chunks) | - | 4.5 (35 chunks) |
| every other copy of the query: H2D GiB / summed copy ms | 81.54 / 2980, 2976 | 81.54 / 2978, 2983 | 56.84 / 2103, 2105 | 56.84 / 2107, 2101 |
| `convert:gpu_to_gpu:host_staging` ranges (CONCAT move), sum ms | 479, 455 | 477, 477 | 198, 206 | 206, 194 |

(The author's q21 captures, re-analysed with my script: 1932 syncs 274-289 ms -> 1504 event syncs 34-44 ms.)

- **By transport: every finish leg is pinned<->device in both arms** (no device-to-device leg); bytes 10 x B -> 8 x B.
- The speed-up is real overlap: at the tip the four GPUs' copy-busy times add up to more than the finish wall (serial legs, the host
  blocks in a stream sync after each); with the prototype each GPU copies for 70 ms of the 73-74 ms of finish (parallel legs,
  42-43 GiB/s per GPU, both directions together).
- Nothing else slowed down: every copy not issued by the finish has the same bytes and the same summed duration in both arms;
  the CONCAT move ranges are unchanged. The query saving equals the finish saving (q21 -215 ms vs -226 ms of finish; q7 -43 vs -40).
- In today's engine almost no other transfer runs during a finish in either arm (8-22 copies, 2-5 GiB of device-local copies;
  no `convert:gpu_to_gpu` range and no compressed-scan upload overlaps a finish). So TPC-H at the tip's placement says nothing
  about contention with concurrent transfers; that is measured in the microbench (section 6.3).
- A 34 ms `sirius_physical_concat::execute` range and a 115 ms `hash_partition` range seen in the author's `auto` capture
  appear in the tip captures too: sporadic, not caused by the prototype.

## 3. Correctness of the filter contents

### 3.1 Unit level (bit identity against the tip loop, every array of every GPU)

`[xfer_proto]` = 5 array shapes x 6 contribution sets x 2 roots x 9 configurations, each array `memcmp`'d against the tip loop.

| Run | GPUs (CUDA_VISIBLE_DEVICES) | Finishes | Result |
|---|---|---|---|
| natural order | 0,1,2,3 (G=4) | 540 | identical |
| pairs interleaved (logical neighbours have no P2P) | 2,0,3,1 (G=4) | 540 | identical |
| 3 GPUs: "everyone" = 3 contributors, one pair incomplete | 1,2,3 and 0,1,2 (G=3) | 540 + 540 | identical |
| 2 GPUs, one of each pair, order reversed | 3,1 (G=2) | 450 | identical |
| 2 processes at once on the same 4 GPUs, 4 rounds (link, CPU and GPU contention) | 0,1,2,3 | 8 x 540 | identical |
| 1 process alone (its 2 neighbours failed to start) | 0,1,2,3 | 540 | identical |
| `[xfer_proto_exact]` (exact kinds, masks on every GPU) | 0,1,2,3 | 96 sessions, 576 masks | identical |

Total 7470 Bloom finishes re-checked, 0 mismatches. (A first stress attempt with 3 processes at once lost 3 of 6 processes at
start-up: each test process pre-allocates a 40.8 GB device pool, so only two fit on a GPU. A harness limit, not a prototype failure.)

Gap of this test (the author's, unchanged): it synchronises every insert before `finish`, so "uploads ordered after in-flight
inserts / zero fill by events" is not exercised at unit level. The engine checks below and the delay-injection cases of the fault
binary (section 5.4) cover it indirectly.

### 3.2 Engine level (TPC-H SF1000, real concurrency)

The engine does not log filter checksums and I did not add code. Instead: compressed scans log, per chunk,
`join filters kept X/Y rows (N of M probes ran)`. That count is a function of the filter replica on the GPU that scanned the chunk.

- Trace log pass, 2 iterations per arm, q21 q3 q7 q10 q8: the per-chunk multisets (Y, X, N, M) are **identical between tip and
  auto and between iterations**, while the scan tasks move to other GPUs between iterations (the per-GPU row counts of the
  `DYNAMIC_FILTER` tasks permute). q21: 48 chunks, 423,297,173 of 9,124,908,864 rows kept; q3 160,514,096 / 1,927,817,321;
  q7 182,211,392 / 2,254,690,081; q10 86,644,247 / 683,433,385; q8 161,578,660 / 6,605,705,240.
- Repetition for races: `auto`, 12 iterations in one process, debug log, q21 q3 q7 q10 q8 q4 q9 = 84 iterations, 156 pipelined
  finishes. Every iteration's multiset equals iteration 0, which equals the tip's (q4, q9: no tip log, self-consistency only).
  The publication root landed on all four GPUs (both pairs) in every query but q4 (3 of 4). No error / fallback / OOM line. Results = tip's.
- Schedules that TPC-H exercises (debug line of the prototype): `hostor` with 3 sources (all the large filters) and with 2 non-root
  sources (q9, 20 MiB), `fanout` (q3, q7, q8: 22-57 MiB). **`gpuor` never ran in these 7 queries**: its event graph (the most
  intricate one) is covered by the unit identity test and the microbench only.

This is functional evidence on 0.7-9.1 billion probe rows per query, not a byte compare of the engine's arrays.

## 4. Existing DF unit tests and fault injection

4 GPUs, filter `[dynamic_filter][multi_gpu],[device_transfer],[publisher],[construction_plan],[physical_partition],[build_filter]`
(159 cases), prototype binary:

| Arm | Failing cases | Which |
|---|---|---|
| switch off (author's run) | 0 | - |
| `auto` (mine) | 4 | the author's 4: workspace expectations (`host_staging_bytes == 1 chunk`, `device_scratch_bytes == 1 chunk`, one scratch request), and `publication scratch allocation failure retires ordinary-pool partials` (publishes where the tip gives up) |
| `gpuor` | 4 | the same 4 |
| `hostor` | **5** | the same 4 + `a tracked task thread admits a merge scratch larger than its reservation as its task would` (section 5.3) |

None of the failing assertions compares filter contents; the union checks inside the same cases pass.

Fault-injection binary `sirius_cuda_fault_test` (`[cuda_status_fault][multi_gpu]`, 12 cases; the author had not run it):
switch off 12/12; `auto`, `hostor`, `gpuor` 11/12 each. Single GPU (`[cuda_status_fault]` DF + device_transfer, 3 cases): all pass in every arm.

## 5. Race / lifetime / failure-path review (code reading + the runs above)

### 5.1 Event ordering without host syncs: no defect found
Read `host_pipeline` (`sirius_dynamic_bloom_filter.cu`), `host_bounce_scope` (`device_transfer.cpp`), the batched gather
(`exact_key_stager.cu`) and the OR pool (`proto_xfer.cpp`).
- Every slot reuse waits for the events of the readers of the previous occupant (`up_done[slot]`, `_in_consumed[slot]`,
  `_merged[buffer]`), recorded W (or `scratch_slots`) chunks earlier; `cudaStreamWaitEvent` captures the record made at call
  time, so re-recording the same event object later is safe. Dependencies always point to earlier chunks: no cycle.
- A GPU's array chunk is overwritten by its upload only after its own download of that chunk (hostor: the host waited for it;
  gpuor: transitively through `_in_staged -> _in_consumed -> merge -> _out_staged`).
- Uploads start after the partial's zero fill and inserts (`ready` event of the partial stream); later readers and the release
  of the arrays follow the uploads (`fin` event folded back into the partial stream); `finish` returns only after every joined
  stream is synchronised, so nothing is in flight when the pinned window and the upload streams are destroyed.
- OR pool: the `_open` / `_active` handshake is a correct Dekker pair (helper: `_active++` then read `_open`; caller: `_open = 0`
  then wait `_active == 0`); `_job` is cleared only after that; concurrent publications are serialised by a mutex. Non-temporal
  stores are fenced before the upload is queued.

### 5.2 Buffer lifetime and stream ownership: no defect found, two notes
- Pinned window, events and upload streams live in the pipeline object; it is released (leaked) together with staging / scratch when a
  stream cannot be joined, like the tip.
- If the `host_pipeline` constructor throws after it created some upload streams (stream creation failure, not `bad_alloc`), the
  streams are destroyed without their GPU being current (the destructor body that sets the device does not run). Not reached in
  my runs; a product version needs a guard member.
- Events of other GPUs are destroyed with the root GPU current (`_in_staged`, `_out_staged`): the tip's `chunk_pipeline` does the same.

### 5.3 Behaviour change on a P2P pair under device-memory pressure (correction to "prototype not reached")
`finish_impl` first tries the PEER pipeline; when its device scratch is refused (`rmm::out_of_memory`) it sets `direct = false`
and continues with the HOST path. At the tip the HOST loop then asks for one chunk of scratch, is refused again and the
publication is skipped (`accumulations_skipped_admission`). With the switch on, the HOST path is the prototype: `hostor` and
`fanout` need no device scratch, so the filter **is published through the host**. Test
`a tracked task thread admits a merge scratch larger than its reservation as its task would` fails for exactly this reason in the
`hostor` arm (`accumulations_skipped_admission`: 0 instead of 1). `auto` passes it only because the case has one source (`gpuor` asks for
scratch, is refused, falls back to the tip loop); with two or more sources `auto` = `hostor` and behaves like the failing arm
(code reading; no test has that shape).
So on a full-P2P machine the prototype is reached whenever the PEER scratch is refused, and it then spends a host bounce in the
Publishing Task where the tip gives up at once. Arguably the better outcome (a filter instead of none), but it is a policy change
that must be decided, and "zero effect on full P2P" holds only without memory pressure. Not timed.

### 5.4 Failure paths after enqueueing (author: untested)
Fault binary, G=2 fixture, forced HOST (so the prototype path runs: `gpuor` or `fanout` in `auto`, `hostor` in the `hostor` arm):
- `accumulated publication faults drain all registered streams and retain the CUDA cause` (error injected at copy batch 1 / 3 / 12 and
  at an unjoined stream sync): pass in all three arms: the error escapes with its code, every submitted stream is joined, pinned HOST
  bytes and device bytes return to the baseline, partial streams are idle.
- `delayed accumulation ingress merge and egress preserve every bitmap byte` (80 ms delays injected at copy batches): pass, exact union.
- `HOST exhaustion refuses preparation without changing any partial`: pass (fallback).
- The one failure, `HOST publication submits every target upload before waiting for any`, requires exactly 1 submission during an
  injected delay (the tip loop's order); the pipelines submit 18 (hostor / auto) or 66 (gpuor). Expectation, not a defect.
Not covered: failures with 4 GPUs and `hostor` over 3 sources; a failing `cudaEventSynchronize` inside the hostor loop; an
unjoinable upload stream.

### 5.5 Pinned memory exhaustion
`allocate_multiple_blocks` throws `rmm::out_of_memory` (a `std::bad_alloc`) when the pool is short; the prototype catches it before
anything is enqueued, syncs the publication stream and runs the tip loop. Exercised by the forced-failure configurations of the
identity gate (120 of the 540 finishes per run, simulated by the switch) and by the HOST-exhaustion fault case. A shortage between
two windows cannot happen: the window is one allocation.

## 6. Microbenchmark

### 6.1 Replication (MB4P, G=4, root GPU 0, 5 reps, median, ms/GiB; `out/mb4p_quiet.log`)

| Bloom | batches on | tip loop | gpuor | hostor | author (tip -> hostor) |
|---|---|---|---|---|---|
| 281 MiB | 1 / 2 / 3 / 4 | 61.0 / 103.5 / 144.3 / 186.0 | 39.4 / 47.3 / 75.8 / 96.0 | 39.4 / 48.3 / 47.7 / 46.7 | 61 / 104 / 145 / 186 -> 39 / 48 / 46 / 47 |
| 1434 MiB | 1 / 2 / 3 / 4 | 60.9 / 103.5 / 144.1 / 185.5 | 38.7 / 44.6 / 75.6 / 95.2 | 38.7 / 46.3 / 45.6 / 44.2 | 61 / 104 / 145 / 186 -> 39 / 46 / 46 / 45 |

Checksums of all 4 arrays agree with the tip loop in every row. Within 1-2% of the author's table. (57 and 174 MiB not re-run.)

### 6.2 Memory held (reported by the prototype: MB4P rows and the engine's debug line)
fan-out 32 MiB pinned (12 MiB for a 22 MiB filter); gpuor 64 / 96 / 128 MiB pinned + 16 / 32 / 48 MiB device scratch for 1 / 2 / 3 sources;
hostor 96 / 128 / 160 MiB pinned, no device scratch. In TPC-H every hostor finish with 3 sources holds **160 MiB of pinned HOST for a
84 MiB .. 1.39 GiB filter alike** (48 MiB for the 20 MiB filter of q9); the finishes of one query do not overlap (finish sum = union in
nsys), so the peak is 160 MiB per query. Taken from the fixed-block pool **without a reservation**, like the tip's 8 MiB.
With W=2 (80 MiB) the TPC-H times are the same (section 1). I did not measure the pool's peak independently.

### 6.3 Under a concurrent transfer (new; `bin/c8b_contend.sh`, `out/mb4p_fg2_under_*.log`)
A second process runs the 1434 MiB / 4-contributor finish in a loop for the whole measurement (alive at the end, checked), in `hostor`
mode (all links busy in both directions) or `gpuor` mode. Foreground: the usual rows, 5 reps, ms/GiB (factor against quiet):

| Foreground (281 and 1434 MiB) | quiet | background hostor | background gpuor |
|---|---|---|---|
| tip loop, batches on 4 | 186 | 799-802 (x4.3) | 625-636 (x3.4) |
| gpuor, batches on 4 | 95-96 | 242-248 (x2.6) | 177-184 (x1.9) |
| hostor, batches on 4 | 44-47 | 97-99 (x2.1-2.2) | 80-82 (x1.7-1.9) |
| tip loop, batches on 2 | 104 | 333-339 (x3.2) | 220-226 (x2.1) |
| gpuor, batches on 2 | 45-47 | 116-123 (x2.4-2.8) | 78-82 (x1.7) |
| hostor, batches on 2 | 46-48 | 96-98 (x2.0-2.1) | 87-90 (x1.8-1.9) |

hostor degrades by the fair share of a saturated link (x2); the tip loop degrades twice as much, so the advantage grows
from 4.2x to 8.2x. With the tip loop as background (serial legs): tip 186 -> 215, hostor 44-47 -> 53-56 (first run, 4 contributors).
Caveats: the load is a second process (separate CUDA context, so GPU time-slicing is part of the factor), it is a symmetric copy
load, not a compressed-scan upload, and it is not the engine. It supports, and does not prove, that the gain survives eager probe
scans (S6).

## 7. What the gain is made of (for the PR split)

- All of it is the Bloom `host_pipeline` (`SIRIUS_PROTO_XFER_REPLICA=0` gives the same TPC-H times).
- In TPC-H the schedules that run are `hostor` (2-3 sources) and `fanout` (0 sources). `gpuor` does not run in the 7 DF-heavy queries I
  logged; it is needed only for "one non-root source + root contributed".
- q21 with `gpuor` alone: -132.9 ms against the tip; with `hostor` alone: -205.2 ms (`auto` -214.6). The CPU OR is what carries the gain with 3 sources.
- W=2 / 8 threads is as fast as W=4 / 16 in TPC-H.

## 8. Production issues (beyond the author's list, which I agree with)

1. **Policy under device-memory pressure** (5.3): with the pipeline, a refused PEER scratch no longer skips the filter; it bounces
   through the host. Decide it, also for full-P2P machines; 5 tests pin the old behaviour (4 in `auto`).
2. **The OR pool is process-wide and serialised**: concurrent publications queue behind one mutex, and 15 helper threads spin for
   500 us after every chunk, i.e. for the whole finish (65-80 ms for q21). Free on this 96-core host; on a host with few cores
   it competes with the pipeline threads. Not measurable here. Needs an engine pool, a bound, and a measurement on a small host.
3. **`gpuor` is the least exercised and the most intricate schedule** (no TPC-H coverage, half the speed of hostor with 3-4
   contributors, needs device scratch, degrades more under contention). If the PR keeps it for the one-source case it needs its own
   engine-level test; using hostor for every multi-contributor case is simpler (they tie with one source: 46-48 vs 45-47 ms/GiB).
4. **Test gap**: the identity test syncs all inserts before `finish`. A product test must start `finish` with inserts and zero fill
   still in flight (the fault binary's delay injection can do it), with 4 GPUs and 3 sources, and inject failures inside the hostor loop.
5. **Pinned window sized by contributors, not by the filter**: 160 MiB for an 84 MiB filter; unreserved. Reserve it under the task's
   host reservation and use W=2 (same speed).
6. `host_pipeline` constructor is not exception-safe for a failing stream creation (5.2).
7. 1500 `cudaEventSynchronize` calls per large finish on the Publishing Task's thread: fine now (35-45 ms inside them, they pace the
   CPU OR), but they are host waits on a critical-path thread; their cost with eager scans is unknown.
8. The user decision of 2026-11-19 makes D2D-in-pairs the lead design; this pipeline is then the cross-pair leg / no-P2P fallback.
   Nothing I measured contradicts using it there; the composition (pair-local pre-merge feeding a 2-input hostor) is unbuilt and unmeasured.

## 9. Not verified

- Byte identity of the Bloom arrays inside the engine (no checksum log; I used kept-row multisets instead).
- `gpuor` inside TPC-H beyond the q21 arm of section 1; the one-shot Bloom `replicate_to_devices` with the stage-once scope is not
  byte-compared (author's gap, unchanged).
- Failure injection with 4 GPUs / 3 sources in hostor; an unjoinable upload stream; real (not simulated) pinned-pool exhaustion.
- Interaction with eager probe scheduling (S6) in the engine: only the microbench of 6.3.
- NUMA hosts, hosts with few cores, full-P2P 4-GPU machines, machines with one root port per GPU.
- Queries with n=4 in the table carry about +-10 ms each; see the table for which.
- The binaries are the author's build; I checked only that no source is newer than the library, not a clean rebuild.

## 10. What ran (lock holds 2026-11-20 01:06 - 03:13)

ABBA v1: 22 queries (n=8 for q1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 21 22; n=4 for q19 20), O arm on q21 / q3; W arm q21 / q3; R arm q21 / q3; G / H arms q21. Identity gates: 5 GPU sets + 2 stress
runs. Fault binary: 4 arms x (multi + single). DF unit tests multi-GPU: `auto`, `hostor`, `gpuor`. nsys: q21, q7 (2 arms). Trace log
pass: q21 q3 q7 q10 q8 (2 arms). Debug repetition: 7 queries x 12 iterations. MB4P: quiet + 2 contention runs (+ 1 first attempt whose
hostor background ended early: its 1434 MiB rows are discarded). One timing chain was restarted before its first hold (no data lost).
