# PROTO_OP1A: OP1 design A (asynchronous publication continuation), throwaway prototype

2026-11-18. Worktree `/opt/dlami/nvme/dfperf/wt-op1a`, branch `proto/op1-continuation` (on pr2570 `ae939824`, local
only, never pushed). Scripts and raw output: `/opt/dlami/nvme/dfperf/runs/op1a/` (`bin/`, `ab1/`, `mb/`, `logs/`);
Quent: `/opt/dlami/nvme/dfperf/telemetry/op1a_<label>/<arm>/q<N>/`. Every build and GPU process ran under
`flock /opt/dlami/nvme/dfperf/machine.lock`. Config `/home/ubuntu/sirius/specific_config.yaml` untouched.

STATUS: complete; bottom line in sections 7 and 9.

## 1. What was built

Commits on the branch (all prototype quality, no docs, no new tests):

| commit | content |
|---|---|
| `43f9d882` | async continuation + join point |
| `b8cb3c4e` (cherry-pick of `a5b37728`) | lifecycle microbench harness from `mb/lifecycle` |
| `78665a9c`, `1914a8df` | `bench_dynamic_filter_lifecycle.cpp`: MB8 (sync vs async publication, with a cross-GPU copy load) |
| `3e4dfb51` | `SIRIUS_DF_ASYNC_EARLY=1`: launch the continuation before the hash path's scatter (variant, off by default) |
| `acc10949` | `SIRIUS_PROTO_NUM_GPUS`: measurement-only override of `topology.num_gpus` (`src/sirius_config.cpp`), for section 5b |

Mechanics:

1. **Handoff** (`dynamic_filter_publication_session::publish_if_final_async`, `dynamic_filter_publisher.cpp`).
   Called by `hash_join_partition_observer::after_transform` (the PARTITION site, every layout). Under the session mutex it
   claims the pending publication exactly as `publish_if_final` does (`pending_publication.reset()`,
   `++active_operations`), sets `continuation_in_flight`, starts a `std::thread` and returns. Handoff cost measured in
   the engine: thread start delay 40-50 us (log field `start_delay_us`).
2. **Continuation** (same function, thread body). Sets the root GPU current, takes a stream from a small leaked
   per-GPU free list (filters built on it free in that stream's order later, so the stream must outlive the session),
   runs the unchanged `state::publish` (accumulator `finish`: cross-GPU merge, staged-key gather, root filters;
   `replicate_to_devices`; `fan_out`), holds the `active_operation` guard so the session settles
   (PUBLISHING -> TERMINAL, key lines) before any awaiter is released, then clears `continuation_in_flight` and notifies.
   - Leases: unchanged (the session's `build_filter_construction` owns them).
   - Scratch: `task_space.get_default_allocator()` from an untracked thread, so it is charged against the GPU's
     unreserved capacity, not the Publishing Task's reservation (8 MiB chunk + root IN-lists / zone scalars). A landable
     version needs its own reservation (`scoped_replica_reservation` on the root, sized `task_scratch_bytes() +
     root_construction_bytes()`); not done here. The PARTITION task estimate still includes the publication scratch
     (now unused by the task).
   - Failure: recoverable failures end the attempt inside `publish` as before. A fatal one is stored and rethrown by the
     first awaiter (the build CONCAT task), i.e. it reaches the query error path one pipeline later. The destructor and
     `sirius_physical_hash_join::on_finalize_operator` join the thread.
3. **Join point** (`sirius_physical_hash_join::on_input_batch_pushed`, port `build` ->
   `dynamic_filter_publication_session::await_publication`). Blocks the pushing task (the build CONCAT's sink) on a
   condition variable while the continuation is in flight; otherwise one mutex lock and a bool test.
   Why this point: `select_build_probe_action` (`sirius_physical_hash_join.cpp:617-667`) returns `wait_for_build` while
   any NOT_BUILT slot lacks its build batch, so the probe producer is first demanded after every build CONCAT task has
   pushed. Gating every build push therefore gates the probe scan, and every other consumer that today runs after the
   build CONCAT (transitive targets) with it. No scheduler change, no re-arming of hints.
   STANDARD / MIXED joins get the same ordering as today shifted by one pipeline ("before the build batch reaches the
   join" instead of "before the PARTITION deposit").
4. **Unchanged**: BUILD_FILTER site (q4, q20 j49) still publishes synchronously (`publish_if_final`): its probe is
   activated straight after the stage (slack 0.1 ms), there is nothing to overlap.
5. `SIRIUS_DF_ASYNC_PUBLISH=0` switches the prototype binary back to the synchronous path (same binary A/B).
6. Instrumentation (INFO): `[df_async] continuation join= root_gpu= handoff_epoch_ns= start_delay_us= run_ms=
   visible_epoch_ns= end_epoch_ns= failed=` and `[df_async] await join= waited_ms= released_epoch_ns=` (only when a
   push actually waited). Epoch ns are on Quent's clock, so `first scan Created - visible_epoch_ns` is exact.

Files changed, owner per `pr_split.md`:

| file | change | owner |
|---|---|---|
| `src/op/dynamic_filter/dynamic_filter_publisher.{hpp,cpp}` | `publish_if_final_async`, `await_publication`, `publication_in_flight`, `join_continuation`, continuation state + stream list, visible timestamp | #2570 (session) |
| `src/op/dynamic_filter/hash_join_partition_observer.cpp` | call the async entry; relax the terminal assert; early toggle | #2570 (observer) |
| `src/op/sirius_physical_hash_join.{hpp,cpp}` | `on_input_batch_pushed` override (join point); join the thread at finalize | #2570 (call site) |
| accumulators / kernels / plan (#2569) | **none**: `build_filter_construction::finish` and the three accumulators already order their work on their own per-GPU streams, so `finish` runs unchanged on any stream of the root GPU | - |
| `test/cpp/operator/bench_dynamic_filter_lifecycle.cpp`, `cmake/sirius-test-sources.cmake`, `complete_build_inventory.cpp` (chunk override) | microbench only (cherry-pick + MB8) | not for landing |

## 2. Where the slack is (Quent, tip, before any change)

Pipeline timelines of the tip (`runs/op1a/bin/timeline.py` on `telemetry/baseline_tip`): in every DF join the order is
build PARTITION -> **build CONCAT** -> first consumer scan. The whole slack between the Publishing Task's deposit and
the first consumer scan is the build CONCAT pipeline and nothing else:

| site | PARTITION end -> scan start | what runs there |
|---|---|---|
| q3 j15 | 184.0 -> 297.5 ms | CONCAT(10), 4 tasks, 113 ms; **92-110 ms of each task is `Preparing`** (cross-GPU move of the broadcast build, HOST-staged on this machine), compute ~3 ms |
| q21 j18 | 369.5 -> 450.7 | CONCAT(12), 81 ms; Preparing 43-79 ms |
| q8 j41 | 196.3 -> 297.4 | CONCAT(36), 101 ms |
| q7 j35 | 579.4 -> 636.3 | CONCAT(30), 57 ms |
| q10 j10 / j21 | 106.9 -> 116.4 / 437.1 -> 437.8 | CONCAT 9.5 ms / 0.6 ms |
| q4 j9 | 63.8 -> 63.8 | nothing (BUILD_FILTER, scan follows at once) |

So the work the continuation can overlap is almost entirely the build CONCAT's own cross-GPU movement. On this machine
that movement is HOST-staged too: the continuation's bounce shares the pinned-host path and the PCIe links with the
CONCAT's bounce (measured in section 4). The overlap is real work (the CONCAT must run anyway), but it is not
independent of the finish chain's transport.

## 3. A/B timing, this machine (4 GPUs, HOST-staged Bloom transport)

Protocol: fresh process per query, 6 iterations (1 warm + 5 hot, first hot dropped -> 4 hot per process), order
tip, proto, proto, tip (`A1 B1 B2 A2`) back to back inside one lock hold per query (`runs/op1a/bin/pair.sh`; q3's four
processes were in separate holds). Tip = `$R/repo` binary, proto = `$R/wt-op1a`. Block `ab1` = all 22 queries, block
`ab2` = repeat of q21, q9, q12, q16, q13. "paired" = mean over pairs of (proto process median - tip process median),
pairs (A1,B1), (A2,B2); it removes the slow drift seen inside some holds (q9: 1433 / 1410 / 1386 / 1408 ms in run
order). `runs/op1a/bin/table.py ab1 ab2` -> `runs/op1a/table_ab.md`.

| q | tip med ms (n) [min-max] | proto med ms (n) [min-max] | delta ms | delta % | paired delta ms (per pair) | per-process medians tip | proto | results identical | flags |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 472.8 (8) [462-479] | 468.1 (8) [464-475] | -4.7 | -1.00 | -4.8 (-4 -6) | 472 474 | 469 468 | yes | A: started/published 0/0; B: started/published 0/0 |
| 2 | 151.1 (8) [147-152] | 148.0 (8) [144-152] | -3.2 | -2.10 | -3.2 (-3 -3) | 151 151 | 148 148 | yes | A: started/published 60/50; B: started/published 60/50 |
| 3 | 662.5 (8) [657-750] | 636.4 (8) [628-642] | -26.1 | -3.94 | -25.2 (-25 -25) | 663 662 | 638 636 | yes | A: started/published 24/24; B: started/published 24/24 |
| 4 | 324.0 (8) [321-328] | 324.2 (8) [322-328] | +0.2 | +0.05 | +0.1 (+0 +0) | 324 324 | 324 324 | yes | A: started/published 12/12; B: started/published 12/12 |
| 5 | 692.9 (8) [677-713] | 677.6 (8) [663-702] | -15.2 | -2.20 | -15.2 (-14 -16) | 692 694 | 678 678 | yes | A: started/published 48/48; B: started/published 48/48 |
| 6 | 380.4 (8) [380-382] | 380.4 (8) [379-385] | -0.0 | -0.00 | -0.2 (-0 -0) | 381 380 | 381 380 | yes | A: started/published 0/0; B: started/published 0/0 |
| 7 | 744.3 (8) [742-749] | 720.6 (8) [716-725] | -23.7 | -3.19 | -24.3 (-27 -22) | 747 742 | 720 721 | yes | A: started/published 60/60; B: started/published 60/60 |
| 8 | 935.3 (8) [929-938] | 908.0 (8) [906-912] | -27.4 | -2.93 | -27.5 (-26 -29) | 935 937 | 909 908 | yes | A: started/published 60/60; B: started/published 60/60 |
| 9 | 1417.5 (16) [1394-1470] | 1387.9 (16) [1373-1413] | -29.6 | -2.09 | -25.8 (-23 -21 -26 -33) | 1433 1408 1412 1416 | 1410 1386 1386 1383 | yes | A: started/published 48/48; B: started/published 48/48 |
| 10 | 847.4 (8) [837-859] | 848.1 (8) [836-860] | +0.6 | +0.08 | +3.0 (+9 -3) | 847 847 | 856 844 | yes | A: started/published 24/24; B: started/published 24/24 |
| 11 | 174.1 (8) [168-184] | 171.9 (8) [168-183] | -2.2 | -1.29 | -0.5 (-0 -1) | 176 172 | 175 172 | NO | A: started/published 48/48; B: started/published 48/48 |
| 12 | 702.6 (16) [698-707] | 694.5 (16) [690-700] | -8.1 | -1.16 | -7.4 (-10 -3 -10 -7) | 706 699 703 701 | 696 697 693 694 | yes | A: started/published 24/24; B: started/published 24/24 |
| 13 | 1298.9 (16) [1282-1328] | 1293.6 (16) [1283-1401] | -5.2 | -0.40 | +6.3 (+4 +47 -2 -25) | 1303 1289 1304 1313 | 1308 1336 1302 1288 | yes | A: started/published 0/0; B: started/published 0/0 |
| 14 | 692.5 (8) [677-707] | 686.7 (8) [677-707] | -5.8 | -0.84 | -2.3 (-12 +7) | 693 687 | 681 694 | yes | A: started/published 0/0; B: started/published 0/0 |
| 15 | 527.6 (8) [526-531] | 527.7 (8) [527-529] | +0.1 | +0.02 | -0.0 (+1 -1) | 527 528 | 528 528 | yes | A: started/published 0/0; B: started/published 0/0 |
| 16 | 311.5 (16) [303-318] | 309.2 (16) [297-312] | -2.3 | -0.74 | -3.1 (-7 -9 +4 +0) | 316 314 307 310 | 309 304 311 310 | yes | A: started/published 24/24; B: started/published 24/24 |
| 17 | 497.5 (8) [488-507] | 497.5 (8) [487-506] | -0.0 | -0.01 | -0.2 (+0 -1) | 497 498 | 498 497 | yes | A: started/published 12/12; B: started/published 12/12 |
| 18 | 1093.1 (8) [1078-1099] | 1086.5 (8) [1074-1102] | -6.6 | -0.60 | -6.8 (-4 -10) | 1089 1097 | 1085 1087 | yes | A: started/published 24/24; B: started/published 24/24 |
| 19 | 880.0 (8) [876-892] | 880.1 (8) [876-891] | +0.1 | +0.02 | +0.1 (+0 -0) | 880 880 | 880 880 | yes | A: started/published 12/12; B: started/published 12/12 |
| 20 | 714.9 (8) [699-736] | 714.9 (8) [699-733] | +0.0 | +0.00 | -0.5 (+1 -2) | 714 718 | 715 716 | yes | A: started/published 60/48; B: started/published 60/48 |
| 21 | 1442.5 (16) [1432-1461] | 1413.6 (16) [1404-1430] | -28.9 | -2.00 | -27.9 (-18 -35 -27 -32) | 1434 1448 1440 1453 | 1417 1414 1413 1420 | yes | A: started/published 96/96; B: started/published 96/96 |
| 22 | 339.0 (8) [338-341] | 336.6 (8) [335-339] | -2.3 | -0.68 | -1.9 (-3 -1) | 339 339 | 336 337 | yes | A: started/published 12/12; B: started/published 12/12 |
| sum | 15302 | 15112 | -190 | -1.24 | -167 (-1.09%) | | | | |

- Results: `sirius/q<N>/result.txt` byte-identical between every tip and proto process for 21 queries; q11 differs
  between any two processes, tip vs tip included (tie order, known); sorted, all four q11 results are identical.
- Every process: rc=0, `construction started` == `constructed filters published` counts equal between tip and proto,
  0 fallback / error / retry lines.
- **Suite, 22 queries: pooled medians 15302 -> 15112 ms = -190 ms (-1.24%); sum of paired deltas -167 ms (-1.09%).**
  The 17 queries that build filters: -166 ms paired. The 5 without any DF (q1, q6, q13, q14, q15) sum to -1 ms paired:
  q13 +6.3 (four pairs +4 / +47 / -2 / -25: pure noise, its tip range is 1282-1328 ms), q1 -4.8, q14 -2.3, q6 -0.2,
  q15 0.0. Hold-level noise is therefore about +-5 ms per query outside q13.
- Resolved gains (every pair agrees in sign, outside +-3% / +-10 ms or repeated): q3, q8, q7, q21 (4 pairs), q9 (4 pairs),
  q5. Small, consistent in sign: q12 (4 pairs, -7.4), q18, q2, q22. q16: -7 / -9 in block 1, +4 / 0 in block 2, so
  -3.1 +- noise at the wall (its site shows +5.1 ms). Controls flat: q4, q10, q6, q11, q17, q19, q20, q15, q14.
- Same-binary sanity (label `sync4`, q3, order A S S A): tip 661.0 ms vs prototype binary with
  `SIRIUS_DF_ASYNC_PUBLISH=0` 662.4 ms, paired +1.1 ms (+3 / -0): the prototype build itself changes nothing.
- The prototype library was rebuilt once during block ab1 (21:04, adding only the inert `SIRIUS_PROTO_NUM_GPUS`
  override): q3, q8, q21, q7, q5, q16, q22, q9, q20, q12, q17, q4, q18 ran before it, the rest after.
- **The early-launch variant** (`SIRIUS_DF_ASYNC_EARLY=1`, continuation started before the hash path's scatter; label
  `early`, order B E E B): q21 -1.4 ms (-0.10%), q7 -4.9 ms (-0.68%). The continuation starts ~60 ms earlier on q21 and
  ends at the same time (run time 321 -> 373 ms): it makes no progress while the scatter occupies the root GPU.
  Dropped.

## 4. Attribution per site (Quent + `[df_async]` lines, block ab1, 8 hot iterations per arm)

`runs/op1a/bin/gain.py ab1` -> `runs/op1a/gain_ab1.md` (all 42 join sites). F = finish time taken off the Publishing
Task (tip minus proto task duration), C = build CONCAT window at tip (the slack), F' = continuation run time, S =
PARTITION start -> build CONCAT end. Rows with F >= 3 ms:

| site | Bloom MiB | F | C | F' (x F) | S tip -> proto | realized | ideal min(F,C) | realized / ideal |
|---|---|---|---|---|---|---|---|---|
| q3 j15 (fwd) | 280 | 51.6 | 111.2 | 144.7 (2.8x) | 168.5 -> 144.9 | 23.7 | 51.6 | 0.46 |
| q7 j35 (hash) | 278 | 50.4 | 53.0 | 81.9 (1.6x) | 116.3 -> 94.0 | 22.3 | 50.4 | 0.44 |
| q8 j41 (fwd) | 174 | 32.1 | 99.7 | 85.3 (2.7x) | 135.5 -> 112.2 | 23.2 | 32.1 | 0.72 |
| q9 j29 (fwd) | 166 | 30.7 | 51.1 | 58.5 (1.9x) | 83.1 -> 60.4 | 22.7 | 30.7 | 0.74 |
| q21 j18 (hash) | 1394 | 255.6 | 77.2 | 318.6 (1.25x) | 394.7 -> 372.2 | 22.5 | 77.2 | 0.29 |
| q21 j33 (fwd) | 133 | 24.5 | 27.3 | 44.6 (1.8x) | 53.5 -> 44.9 | 8.6 | 24.5 | 0.35 |
| q5 j32 (fwd) | 87 | 16.1 | 56.4 | 37.1 (2.3x) | 73.3 -> 61.6 | 11.7 | 16.1 | 0.73 |
| q10 j10 / j21 (hash) | 109 / 85 | 20.3 / 15.8 | 9.0 / 0.6 | 23.3 / 16.0 | 31.5 -> 25.5 / 18.1 -> 17.4 | 6.0 / 0.7 | 9.0 / 0.6 | 0.67 / - |
| q12 j8 (fwd) | 59 | 11.1 | 30.6 | 14.7 (1.3x) | 43.2 -> 36.3 | 6.9 | 11.1 | 0.62 |
| q16 j11 (fwd) | 57 | 8.2 | 73.7 | 18.8 (2.3x) | 82.6 -> 77.5 | 5.1 | 8.2 | 0.62 |
| q22 j28 (fwd) | 36 | 7.0 | 4.0 | 9.0 | 11.3 -> 9.3 | 2.0 | 4.0 | 0.52 |
| 57 MiB single batch: q3 j8, q5 j25, q8 j34 | 57 | 3.5 each | 11.5 / 48.9 / 28.7 | 6.1 / 14.3 / 10.2 | - | 2.7 / 2.3 / 2.3 | 3.5 | 0.7 |
| **all 42 sites** | | 557 | | | | **177** | 345 | **0.51** |

- Publishing Task duration, tip -> proto: q3 j15 57.1 -> 5.5 ms, q8 j41 35.6 -> 3.5, q5 j32 16.7 -> 0.6, q9 j29 31.9 -> 1.2,
  q21 j18 317 -> 61.7 (scatter), q7 j35 63.2 -> 12.8 (scatter). What stays on the forward path is the end-of-task stream
  sync on the task's own insert kernel (the OP1b extension, not touched here).
- **Contention with the build CONCAT's own cross-GPU moves.** The continuation runs 1.3-2.8x longer than the same finish
  alone (q5 j25: 4.1x), and where the continuation ends before the CONCAT the CONCAT itself is slower by 4.4-9 ms
  (q8 j41 +8.9 ms = 9%, q5 j32 +4.4 ms = 8%, q16 +3.3, q12 +4.5). Where the finish is longer than the slack the CONCAT
  sink waits at the join point (q21 j18: 262 ms, q3 j15: 35 ms, q7 j35: 27 ms). Half of the ideal gain is lost to this
  sharing (177 of 343 ms).
- Sum of realized site gains 177 ms vs measured suite delta (section 3): consistent.

**Ordering (the mandatory check).** First consumer scan task `Created` minus `visible_epoch_ns`, per join and hot
iteration, all consumer scans (direct and transitive): minimum **+0.05 ms** on the async sites (+0.03 ms on q4's
synchronous BUILD_FILTER site), **0 of 368 scan-iterations** have a scan task created before the filter is visible
(tip: 0 of 276 in PROFILE_SUITE). The scan is created 50-70 us after the join point releases the build push.

**Debug log** (`SIRIUS_LOG_LEVEL=debug`, 3 iterations, tip vs proto, label `dbg`): `join filters kept X/Y rows (N of M
probe(s) ran, complete=..)` lines are identical in count, in the N-of-M split and in the summed X and Y:
q3 72 lines (72x "1 of 1"), 481542288 / 5783451963; q21 144 lines (51x "1 of 1", 93x "1 of 2"), 1269891519 /
27374726592; q8 174 lines (24 / 150), 484735980 / 19817115720; q7 69 lines (54 / 15), 546634176 / 6764070243.
`[dynamic_filter_key]` lines (kind, size, pushed, reason) identical for all four queries.

## 5. Bounce-excluded microbench: MB8 (PEER path, GPUs 0,1)

`flock $R/machine.lock runs/op1a/bin/mb.sh mb8_g2 0,1 "[mb8]" SIRIUS_BENCH_DF_GPUS=2 SIRIUS_BENCH_DF_REPS=7`
(raw: `runs/op1a/mb/mb8_g2.log`, `ROW,MB8,...`). Session level (`begin` -> `contribute` x2 -> publication), one batch per
GPU (1 non-root source, 1 target), transport PEER. Median of 7 after one warm-up, one process, not repeated across
processes (median and minimum agree within 1-7%). `load` = two threads copying 1 GiB buffers GPU0->GPU1 and GPU1->GPU0
with `cudaMemcpyPeerAsync` in 32 MiB pieces for the whole publication (a stand-in for a build CONCAT's moves).

| Bloom MiB (site) | sync `publish_if_final` ms | async: return to the task us | async: visible ms | with load: visible ms (sync / async) | slowdown | load GB/s alone -> during |
|---|---|---|---|---|---|---|
| 57 (q3 j8, q5 j25, q8 j34, q16 j11, q12 j8) | 1.49 | 25.9 | 1.55 | 6.44 / 6.53 | 4.3x | 105.3 -> 85-87 |
| 87 (q5 j32, q10 j21) | 2.18 | 25.7 | 2.23 | 9.31 / 9.28 | 4.3x | 105.3 -> 86.5 |
| 133 (q21 j33) | 3.17 | 26.2 | 3.22 | 14.0 / 14.1 | 4.4x | 105.3 -> 85.6 |
| 174 (q8 j41, q9 j29) | 4.07 | 25.5 | 4.13 | 17.9 / 17.8 | 4.4x | 105.5 -> 84.7 |
| 281 (q3 j15, q7 j35) | 6.45 | 28.4 | 6.50 | 29.3 / 29.3 | 4.5x | 105.3 -> 84.8 |
| 1394 (q21 j18) | 30.8 | 31.2 | 30.8 | 142.4 / 142.6 | 4.6x | 105.5 -> 83.4 |

- The handoff costs the Publishing Task **25-31 us** at any size; visibility is delayed by ~50 us (thread start).
- PEER finish alone: 22-26 ms per GiB with one source and one target (agrees with MB4: 20 ms/GB per target + ~10% for a
  contributing source).
- Under a link-saturating copy load the finish is 4.3-4.6x slower and the load loses ~20% of its throughput. The exchange
  is **byte-conserving**: at 1394 MiB the Bloom moves 2 x 1.394 GiB = 2.99 GB and the load loses (105.5 - 83.4) GB/s x
  0.1424 s = 3.1 GB. A link that is already saturated has no free capacity to give: overlapping then moves the wait, it
  does not remove it. The continuation gains only where the overlapped CONCAT leaves the links idle.
- Not measurable here: 3 PEER targets (P2P exists only inside pairs 0-1 and 2-3), and how link-bound a real build CONCAT
  is on a full-P2P machine.

### 5b. In-engine PEER measurement: TPC-H SF1000 on GPUs 0,1 only (the one P2P pair)

`CUDA_VISIBLE_DEVICES=0,1 SIRIUS_PROTO_NUM_GPUS=2` (prototype-internal override of `topology.num_gpus`,
`src/sirius_config.cpp`; the config file is untouched). Both arms are the prototype binary: S = `SIRIUS_DF_ASYNC_PUBLISH=0`
(synchronous, the tip's behaviour), B = async. Order S B B S in one hold, 6 iterations, label `g2`
(`table.py --arms=S,B g2`, `ARM_A=S gain.py g2`). Transport PEER in every publication (0 "Bloom through HOST staging"
lines). 2 partitions, 2 batches per build, so one target and one non-root source: the finish is 3x smaller than a
full-P2P 4-GPU run would have, but the mechanism (finish copies vs CONCAT moves on the same links) is the real one.

| q | wall sync -> async ms (8 hot each) | paired delta | site | F taken off the task | C at sync | CONCAT with the continuation | S sync -> async |
|---|---|---|---|---|---|---|---|
| 3 | 909.9 [910-910] -> 909.7 [908-912] | -0.1 (-0 -0) | j15, 280 MiB | 6.3 | 25.7 | +5.7 | 43.1 -> 42.7 (+0.5 gained) |
| 8 | 1438.8 [1436-1440] -> 1439.0 [1437-1441] | +0.5 (+1 +0) | j41, 174 MiB | 4.0 | 22.9 | +4.0 | 32.6 -> 32.5 (+0.1) |
| 7 | 1188.4 [1183-1193] -> 1192.0 [1182-1198] | +3.6 (-0 +7) | j35, 278 MiB | 6.6 | 28.3 | +8.9 | 57.1 -> 62.2 (-5.2) |

Results identical between S and B. **On the PEER path the continuation gains nothing**: the build CONCAT's move is
link-bound, the finish's copies take link time from it one for one (CONCAT slower by 0.9-1.35x of the finish time taken
off the task), and the first consumer scan starts when it did before. Same picture as MB8's byte-conserving exchange.
(q21 on 2 GPUs was not run.)

## 6. Unit tests (prototype binary, 4 GPUs visible)

Filter `[publisher],[publication_claim],[physical_partition],[in_task_publication],[build_filter],[dynamic_filter][construction]`
(238 cases), `runs/op1a/mb/ut_{sync,async}.log`:

- `SIRIUS_DF_ASYNC_PUBLISH=0` (synchronous path of the prototype binary): **238 of 238 pass**, 98344 assertions.
- default (async): 226 pass, **12 fail, all for the contract the prototype changes on purpose**: they assert that the
  publication is complete (channel terminal, filter in the snapshot, outcome counters recorded, scratch requested from
  the task's allocator) at the moment the PARTITION task deposits or returns. Files: `test_in_task_publication.cpp`
  (lines 570, 605, 657, 701, 784, 895) and `test_physical_partition.cpp` (900, 1018, 1133, 1232). Names: "the elected
  (forward-layout) PARTITION task publishes before its (first repository) deposit", "... OOM ... retries and publishes
  once", "single-partition and broadcast builds construct at the PARTITION and end Terminal", "optional publication
  scratch OOM deposits without rescheduling", "a migrated final PARTITION accounts merge scratch in task history",
  "scheduler error drain joins collecting and publishing PARTITION tasks before reuse", and 5 similar. No crash, no
  hang. A landable version has to rewrite these around the join point (and add tests for the awaiter and for a fatal
  continuation error).

## 7. Gain summary

| | this machine (all of it is HOST-bounce time hidden in the CONCAT slack: report only) | bounce excluded / P2P-equivalent |
|---|---|---|
| q3 | -25.2 ms (-3.8%), 2 pairs -25 / -25 | measured on the PEER pair (G=2): -0.1 ms; site +0.5 ms of 6.3 ideal |
| q8 | -27.5 ms (-2.9%), -26 / -29 | PEER pair: +0.5 ms; site +0.1 ms of 4.0 ideal |
| q7 | -24.3 ms (-3.3%), -27 / -22 | PEER pair: +3.6 ms (noise); site -5.2 ms |
| q21 | -27.9 ms (-1.9%), 4 pairs -18 / -35 / -27 / -32 | not measured; MB8: finish 30.8 ms on PEER, 4.6x slower under link load |
| q9 | -25.8 ms (-1.8%), 4 pairs | not measured |
| q5 | -15.2 ms (-2.2%) | not measured |
| q16 / q12 / q18 / q2 / q22 | -3.1 (-1.0%, 4 pairs, mixed sign) / -7.4 (-1.1%, 4 pairs) / -6.8 (-0.6%) / -3.2 (-2.1%) / -1.9 (-0.6%) | not measured |
| controls q4, q10, q6, q11, q17, q19, q20, q15, q14 | +0.1, +3.0 (+9 / -3), -0.2, -0.5, -0.2, +0.1, -0.5, 0.0, -2.3 (-12 / +7) | - |
| **suite (22 queries)** | **-167 ms (-1.09%) paired; -190 ms (-1.24%) pooled medians; -166 ms over the 17 DF queries; 177 ms site-attributed** | **~0** (measured 0 on the PEER pair for 3 queries; 4-GPU full P2P not measurable here) |

Against the OPPORTUNITIES pass bar "on this machine" (q3 >= 25 ms, q8 >= 25 ms, suite >= 100 ms, 0 scans before visible,
q4 / q10 flat): met. It recovers the stack's regression against base (q3 31-35, q8 35, q5 15, q16 8 ms) to within
6-10 ms and adds q7 / q21 / q9. But the OPPORTUNITIES model for the bounce-excluded side (170-185 ms) assumed the slack
is free capacity; it is not: the slack is the build CONCAT's cross-GPU move, which uses the same transport.

## 8. Complexity and risk

- Size: ~270 lines in the session, ~25 in the call sites, nothing in #2569 code. One new thread per publication (40 us
  start), one condition variable, one extra stream per root GPU.
- What a landable version still needs (none of it done): a reservation owned by the continuation instead of untracked
  scratch; an executor slot instead of a raw `std::thread` (work lease, query-lifecycle accounting, telemetry); the
  PARTITION task estimate without the publication scratch; rewriting the 12 unit tests that encode "publication resolves
  before the PARTITION deposit" plus new tests for the awaiter, cancellation while in flight and a fatal continuation
  error; the doc contract (`dynamic-filters.md` "Publication before deposit", `pipeline-execution.md` "Dynamic-filter
  independence").
- Behavioural risks seen or reasoned, not all tested: a fatal continuation error surfaces one pipeline later, or never
  if the query ends before any build push (then only logged at join); every build CONCAT task blocks an executor thread
  at its sink while the continuation runs (q21: 4 threads for 262 ms; the tip blocks 1 thread for the same time);
  session storage release moves from PARTITION finalize to hash-join finalize; STANDARD / MIXED joins and distributed
  (cross-rank merge) paths were not exercised; no OOM / spill run.
- The join point itself held in every run: 0 early scans, identical filter application, identical results.

## 9. Status, recommendation, unverified

**Recommendation: drop for the campaign's goal (bounce ignored).** The mechanism works and is cheap (25-31 us handoff,
no lost filter), but everything it gains on this machine is HOST-staged bounce time overlapped with the build CONCAT's
own HOST-staged move (-1.1 to -1.2% of the suite), and with the bounce excluded the measured gain is zero: on the PEER
pair the CONCAT slows by the finish time taken off the Publishing Task. Keep the branch only as the known lever for
"base parity on machines without full P2P" (it would then be a #2570 follow-up: session + observer + join call site, no
#2569 change), and do not count OP1 towards OP2 / OP3 / OP5 as "hiding the finish": on P2P it hides nothing.

Unverified / limits:
- Full-P2P 4-GPU behaviour (3 targets, 12 directed links) cannot be measured here; the zero is from G=2 on pair 0-1
  (q3, q8, q7; 8 hot iterations per arm, one block) plus MB8.
- Each query has one ABBA block (2 pairs, 8 hot iterations per arm); q21, q9, q12, q16, q13 have two. Hold-level noise is
  about 5 ms (q1, no DF: -4.8 ms).
- The realized fraction (0.29-0.76 of the ideal) is specific to this machine's HOST path; nothing was tuned to raise it
  (that would be optimizing the bounce).
- Scratch accounting, failure paths, cancellation, STANDARD / MIXED joins, distributed merge: reasoned only.
