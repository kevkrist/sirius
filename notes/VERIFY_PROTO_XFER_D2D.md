# VERIFY_PROTO_XFER_D2D: adversarial verification of proto/xfer-d2d (per-island D2D + host hop between pairs)

2026-11-20. Verifier (independent of the prototype author). Prototype: worktree `/opt/dlami/nvme/dfperf/wt-xfer-d2d`,
branch `proto/xfer-d2d` head `b7d0eb81` (NOT modified by me; binaries as built by the author 2026-11-19 20:33-20:34,
no source file newer than them). Tip reference binary: `/opt/dlami/nvme/dfperf/repo` (`ae939824`).
My scripts: `$R/runs/vxfer_d2d/bin/`, raw output `$R/runs/vxfer_d2d/`, Quent `$R/telemetry/vxfer-d2d*`.
My own tests: `$R/runs/vxfer_d2d/src/vx_tests.cpp` (copy `bin/vx_tests.cpp.txt`), compiled OUT OF TREE with the
prototype's compile flags and linked with the prototype's unchanged test objects into `vxbin/sirius_unittest_vx`
(`bin/vx_build.sh`); nothing is written into the prototype worktree.
Every build and GPU process ran under `flock $R/machine.lock`. Config untouched; only env switches.

STATUS: COMPLETE (2026-11-20 03:40). Sections: verdict, 1 timing, 2 correctness, 3 nsys, 4 memory, 5 code findings,
6 production issues, 7 what ran / unverified, 0 checkpoint log (chronological, at the end).

## Verdict: CONFIRMED (gain and correctness), with three corrections

- **Timing confirmed.** Suite 15302 -> 14895 ms = **-407 ms (-2.66%)** by pooled medians (claim -423 / -2.76%);
  -371 ms (-2.4%) by the mean of process medians. Honest range **-370 .. -410 ms (-2.4 .. -2.7%)**; the two ABBA halves
  give -347 and -395 ms on the 17 queries with 4 processes, same-arm sums differ by 18-30 ms.
  q21 -204 (claim -198), q3 -37 (-37), q7 -39 (-36), q10 -26 (-30), q8 -24 (-24), q4 -13.5 (-13.7), q5 -13.8 (-14.4),
  q9 -18 (-26, noisy), q12 -8 (-12), q16 -5.5 (-10, inside its noise), q22 -4.7 (-5.1). Controls q6 -0.5, q2 -0.7.
  Switch off in the prototype binary vs the tip binary: q21 +4.7 ms, q3 +1.0 ms (neutral).
- **Correctness confirmed** on everything I could reach: 0 mismatching replicas in 5,472 publications (4,104 with the switch on) with the inserts
  left in flight (all 16 masks, every root, 4 / 3 / 2 GPUs, permuted device order), 480 concurrent publications (360 with the switch on),
  refused remote lease, pinned HOST shortage; engine kept-row multisets identical on q21, q3, q7, q8, q10; results
  identical on 22 queries (q11: tie order only, also tip vs tip).
- **Speed-up is real**, not shifted waiting: same bytes and same summed copy time for every other transfer, nothing
  left in flight after `finish`, the rest of the query timeline unchanged, no CONCAT move ever overlaps a finish.
- Correction 1: the author's "30 ms of q21's saving does not reach the wall / second filter not fully on the critical
  path" is wrong. Both finishes are on the critical path; the 25 ms is a **publisher lottery in the tip stack** (section 3).
- Correction 2: "fault binary 12/12 with the switch on" covers **nothing** of the island code (no case can reach it), and
  many of the 32 DF multi-GPU unit tests cannot either (2-GPU fixtures). The 956-vs-962 assertion difference is run-to-run
  variation (off 961, pipe 956, star 956, exchange 962 in my run), not a mode effect.
- Correction 3: one real defect on the failure path (HOST window released while the other leader's stream may still
  use it, section 5 item 1). Not reachable by any test today.
- Not better than the competing prototype on this machine: `proto/xfer-host` (all-host CPU OR) was verified at q21 -215,
  q3 -41.5, q7 -42.6 ms (VERIFY_PROTO_XFER_HOST) against -204, -37, -39 here. "Largest measured item of the campaign"
  is a tie within noise, not a lead. (The user prefers D2D-in-pairs in principle; this does not argue against it.)

## 1. Timing: independent ABBA, tip binary (`$R/repo`) vs prototype with `SIRIUS_PROTO_XFER_D2D=pipe`

Fresh process per query, 1 warm + 5 hot iterations, first hot dropped (4 kept per process), every process under the
machine lock, one lock hold per query or small group. `A` = tip binary, `P` = prototype pipe, `O` = prototype binary with
the switch unset. q21, q3: `A1 P1 O1 O2 P2 A2`; 15 more queries `A1 P1 P2 A2` (n=8 per arm); q14, q15, q17, q19, q20:
one `A P` or `P A` pair (n=4 per arm). Raw: `runs/vxfer_d2d/v1/`; table `bin/abba_table.py v1 AP`.

| q | A med ms (n) [min-max] | P med (n) [min-max] | P-A ms (%) | per-process medians A / P | results |
|---|---|---|---|---|---|
| 1 | 476.1 (8) [464-478] | 473.6 (8) [465-487] | -2.5 (-0.54%) | 476 475 / 477 472 | identical |
| 2 | 147.8 (8) [147-150] | 147.1 (8) [146-149] | -0.7 (-0.50%) | 148 148 / 148 147 | identical |
| 3 | 660.3 (8) [654-667] | 623.1 (8) [616-714] | -37.1 (-5.63%) | 661 660 / 621 629 | identical |
| 4 | 324.4 (8) [321-329] | 310.9 (8) [308-317] | -13.5 (-4.15%) | 323 326 / 311 313 | identical |
| 5 | 692.6 (8) [680-713] | 678.8 (8) [664-705] | -13.8 (-1.99%) | 693 693 / 679 678 | identical |
| 6 | 380.6 (8) [378-384] | 380.1 (8) [380-384] | -0.5 (-0.13%) | 380 381 / 380 382 | identical |
| 7 | 746.4 (8) [744-756] | 707.4 (8) [701-717] | -39.0 (-5.23%) | 746 747 / 706 709 | identical |
| 8 | 935.1 (8) [929-939] | 910.9 (8) [904-914] | -24.2 (-2.59%) | 937 934 / 911 911 | identical |
| 9 | 1412.9 (8) [1391-1427] | 1395.4 (8) [1384-1406] | -17.6 (-1.24%) | 1407 1416 / 1393 1401 | identical |
| 10 | 849.9 (8) [836-871] | 823.5 (8) [819-830] | -26.4 (-3.10%) | 842 855 / 822 828 | identical |
| 11 | 175.5 (8) [169-178] | 172.6 (8) [168-181] | -3.0 (-1.69%) | 171 176 / 173 172 | DIFF ['3986aa0f', '576acc0b', '79564347', 'd0b54a0d'] |
| 12 | 702.5 (8) [699-706] | 694.1 (8) [691-696] | -8.4 (-1.19%) | 702 703 / 694 693 | identical |
| 13 | 1300.9 (8) [1281-1479] | 1302.5 (8) [1282-1402] | +1.5 (+0.12%) | 1288 1304 / 1343 1289 | identical |
| 14 | 687.8 (4) [678-698] | 688.6 (4) [677-707] | +0.8 (+0.11%) | 688 / 689 | identical |
| 15 | 527.9 (4) [527-529] | 527.6 (4) [525-529] | -0.3 (-0.06%) | 528 / 528 | identical |
| 16 | 312.9 (8) [302-318] | 307.4 (8) [298-313] | -5.5 (-1.75%) | 315 303 / 304 310 | identical |
| 17 | 497.5 (4) [487-507] | 497.9 (4) [487-508] | +0.4 (+0.08%) | 497 / 498 | identical |
| 18 | 1094.1 (8) [1086-1099] | 1086.6 (8) [1080-1110] | -7.5 (-0.68%) | 1095 1094 / 1084 1093 | identical |
| 19 | 879.4 (4) [878-892] | 879.3 (4) [876-890] | -0.0 (-0.00%) | 879 / 879 | identical |
| 20 | 715.4 (4) [704-734] | 713.9 (4) [699-732] | -1.6 (-0.22%) | 715 / 714 | identical |
| 21 | 1443.3 (8) [1436-1450] | 1239.5 (8) [1228-1266] | -203.9 (-14.12%) | 1443 1442 / 1239 1239 | identical |
| 22 | 338.6 (8) [338-344] | 333.9 (8) [333-335] | -4.7 (-1.39%) | 338 339 / 334 334 | identical |
| sum | 15302 | 14895 | -407.4 (-2.66%) | | |

- Switch-off arm (`O`, same prototype binary): q21 1448.0 ms (+4.7, +0.3%), q3 661.3 ms (+1.0, +0.2%); process medians
  1449.0, 1444.7 / 660.6, 661.3.
- q13: first pair gave +55 ms (pipe process bimodal 1380 / 1305 ms); with the second pair +1.5 ms. q13 is bimodal in
  both arms (tip max 1479, pipe max 1402): a suite sum with one q13 pair is good to +-50 ms only.
- q11 result md5 differs between all four processes (tip vs tip too); sorted outputs are identical (`730f7529`).
- Against the claim per query: all eleven claimed movers reproduce in sign; q9, q12, q16 come out 3-9 ms smaller,
  q21 / q7 3-6 ms larger. Sum of the eight named queries: -375.5 ms (claim -379).
- Gates: >= 1% of a query on q3, q4, q5, q7, q8, q9, q10, q12, q21, q22 (10 queries; q16 is inside its own noise);
  >= 0.5% of the suite (77 ms) five times over.

## 2. Correctness

| Check | What | Result |
|---|---|---|
| Author's `[mbid]` gate, G=4, re-run | 4 sizes x 16 masks x roots {0, 3} x 4 modes, inserts synced before `finish` | 512 cases, 0 bad |
| Author's `[xfer_relay]` gate, re-run | 3 exact / one-shot kinds x roots {0, 3}, probe masks on every GPU | 24 masks identical, 6 relayed |
| **VX1** (mine) G=4 | inserts LEFT IN FLIGHT (3 batches x 3M rows x 2 keys per GPU, no host sync before `finish`); 3 / 20 / 57 MiB x 16 masks x 4 roots x 4 modes x 3 reps; every replica on every GPU vs the OR of the partials | 2304 finishes, 0 bad |
| VX1, 130 MiB (8 MiB chunks, 17 + 6 chunks) | 8 masks x 4 roots x 4 modes x 3 reps | 384, 0 bad |
| VX1, `CUDA_VISIBLE_DEVICES=0,2,1,3` | islands interleaved in logical ids ({0,2}, {1,3}) | 1536, 0 bad |
| VX1, 3 GPUs `1,2,3` and `0,1,3` | pair + single, every root, every mask | 576 + 576, 0 bad |
| VX1, 2 GPUs `0,2` | no P2P: `plan()` declines (workspace = off) | 96, 0 bad |
| **VX2** (mine) | 4 threads publish at once (roots 0-3, own streams, inserts in flight), 15 rounds, 20 and 57 MiB, 4 modes | 480 finishes, 0 bad, 0 exceptions |
| **VX3** (mine): refused lease on the other leader | reservable memory of every other GPU cut to 1 MiB; modes x roots {0, 3} | falls back to the tip loop (tip workspace, tip time 14.4 ms vs 4.1 ms), replicas identical, storage returned |
| **VX4** (mine): pinned HOST shortage | capacity cut to 4 / 3 / 2 / 1 / 0 tip windows (8 MiB each) | pipe needs 4, exchange 2, star 1; below that -> tip loop, replicas identical; 0 windows -> publication refused exactly as the tip; storage returned |
| Engine filter contents | trace pass tip vs pipe, 3 iterations, q21 q3 q7 q8 q10: per-chunk `join filters kept X/Y` multisets and DYNAMIC_FILTER rows per GPU | identical between arms and iterations; 30 of 30 pipe finishes on the island route, roots GPU 0-3 all seen |
| TPC-H results | md5 of `result.txt`, 82 processes | identical on 21 queries; q11 tie order only |
| DF multi-GPU unit tests, `pipe` | `[dynamic_filter][multi_gpu]~[.]` | 32 cases pass (4917 assertions); mostly 2-GPU fixtures = tip path |
| Fault-injection binary, 4 modes | `[cuda_status_fault][multi_gpu]~[.]` | 12 / 12 each; cannot reach the island code |

VX tests: `runs/vxfer_d2d/src/vx_tests2.cpp` (copy `bin/vx_tests.cpp.txt`), logs `runs/vxfer_d2d/out/vx*.log`.
Route evidence in the VX runs = the workspace observation (pinned bytes differ per mode: 8 / 8 / 16 / 32 MiB).
Engine-level "filter contents identical on every GPU" is indirect (kept rows per chunk; chunks rotate over GPUs
between iterations); the byte-level proof is the builder-level VX1.

## 3. Real speed-up or shifted waiting? (nsys; author's captures re-analysed, own captures below)

- `bin/vtail.py`: in every capture (tip and pipe) the last copy / kernel issued by the finish thread ends 0.03-0.19 ms
  BEFORE the `dynfilter::accum::finish` range ends: nothing is left in flight when `finish` returns; the two finishes
  of a query never overlap each other.
- q21 timeline per hot iteration (ms from query start), author's captures:
  | | finish 1 | finish 1 end -> finish 2 start | finish 2 | finish 2 end -> query end | wall |
  |---|---|---|---|---|---|
  | tip | 89.5 - 370.2 (280.7) | 538.8 | 26.6 | 512.0 | 1447.5 |
  | pipe | 114.8 - 191.3 (76.5) | 542.7 | 9.0 | 517.4 | 1260.4 |
  Everything outside the two finishes is unchanged (539 vs 543, 512 vs 517 ms): the time is not moved elsewhere.
  Both finishes are fully on the critical path (the author's remark that the second filter "is not fully on the
  critical path" is wrong).
- **Correction: the 25 ms that "did not reach the wall" under nsys is a publisher lottery in the tip stack, not a
  property of the prototype.** All 5 build PARTITION tasks contribute within 0.1 ms of each other; the one that
  contributes last becomes the publisher and runs `finish` only after its OWN `hash_partition` (HYPOTHESES C2).
  Three of the five hash partitions take 30 ms, two take 56-58 ms. In the author's two tip iterations the publisher was
  a 30 ms task (finish starts at 89 ms), in the two pipe iterations a 56-58 ms task (finish starts at 115-116 ms).
  Same code before the finish in both arms; which task publishes is decided by microseconds. Like for like the nsys
  wall gain is ~-213 ms = the finish saving (-222 ms). This lottery is a +-26 ms bimodal noise source on q21 in every
  arm of every q21 measurement of the campaign (my ABBA: tip 1436-1450, pipe 1228-1266).
- Own nsys captures (q7, q10; tip vs pipe; 2 hot iterations each; `out/nsys_q*_*.txt`):
  | q | arm | finish ms | host waits | D2H GiB | H2D GiB | P2P GiB | other H2D of the query, GiB / summed copy ms | CONCAT move ranges sum ms | wall ms |
  |---|---|---|---|---|---|---|---|---|---|
  | 7 | tip | 54.0 / 54.2 | 394 | 1.109 | 1.697 | 0 | 56.84 / 2103, 2108 | 198, 227 | 773.8, 752.5 |
  | 7 | pipe | 16.1 / 15.8 | 34 | 0.566 | 0.566 | 1.132 | 56.82 / 2105, 2105 | 205, 206 | 736.4, 710.3 |
  | 10 | tip | 37.2 / 37.4 | 262 | 0.759 | 1.139 | 0 | 52.33 / 1824, 1828 | 1226, 1267 | 868.8, 863.5 |
  | 10 | pipe | 11.4 / 11.0 | 38 | 0.380 | 0.380 | 0.759 | 52.33 / 1826, 1822 | 1218, 1289 | 831.2, 835.5 |
  No CONCAT-move range (`convert:gpu_to_gpu:host_staging`) overlaps a finish in any capture of either arm; nothing is in
  flight when `finish` returns (last GPU op 0.03-0.19 ms before the range end).

## 4. Memory held during a finish (measured: workspace observation in VX1 / VX3 / VX4, engine debug log)

| | pinned HOST | device scratch, root | device scratch, other leader |
|---|---|---|---|
| tip (HOST staged) | 8 MiB (8,388,608) | 1 slot: 8 MiB at >= 64 MiB filters (7.1 MiB at 57) | 0 |
| `star` | 8 MiB | 24 MiB (25,165,824) | 16 MiB (by code, not observed) |
| `exchange` | 16 MiB | 24 MiB | 24 MiB (by code) |
| `pipe` | 32 MiB (33,554,432) | 32 MiB (33,554,432) | 32 MiB (33,554,432 in the engine log for filters >= 64 MiB; 15.0 MiB for the 57 MiB single-contributor filters of q3 / q8, leased but unused; 6.0 MiB for q7's small filter) |

- The author's table is confirmed for every cell I could observe. Root scratch stays inside the task's reservation
  (`publication_scratch_bytes` = min(2, chunks) x (G-1) slots = 48 MiB at G=4; pipe needs 2 x (island size - 1) + 2 slots,
  which is <= 2 x (G-1) whenever a second island exists).
- The other leader's 32 MiB is leased at finish time on a GPU the Publishing Task does not run on (not in the upfront
  plan). Refusal is safe (VX3) but silently costs the whole gain for that filter.
- All GPU and pinned storage is back after `finish` in every VX3 / VX4 row, including fallback and refusal.
- The array memory (B per GPU) is unchanged.

## 5. Findings from reading the prototype (`git diff 09a04c32 b7d0eb81`), not all provable by a test here

Event graph of `pipe` (checked by hand, per chunk of parity p; streams per island: `ingress`, leader partial stream,
`xfer`, `up`; members' partial streams):
- merge: `ingress` waits `source_ready` (recorded on each source's partial stream = after its folded inserts) and, from
  chunk 2 on, `merged[p]` of chunk-2 (scratch reuse) -> peer copies -> `ingress_done[p]` -> leader partial stream ORs ->
  `merged[p]`.
- down: `xfer` waits `leader_ready` (once), `merged[p]` (islands with sources) and the other leader's `up_done[p]` of
  chunk-2 (window reuse) -> D2H into window[p] -> `down_done[p]`.
- up: `up` waits `leader_ready`, the other leader's `down_done[p]` -> H2D into cross slot p -> `up_done[p]` -> waits its own
  `down_done[p]` and `merged[p]` -> OR -> `settled`.
- fan-out: each member's partial stream waits `settled` -> peer copy from the leader.
- Every reuse of a scratch slot, a window and an event is ordered either by stream order or by an event captured at
  enqueue time (`cudaStreamWaitEvent` binds to the most recent `cudaEventRecord` at the call). I found no ordering hole
  in the success path; VX1 / VX2 (inserts in flight, concurrent publications) agree.

Defects / risks (all on paths no existing test reaches):
1. **Failure path, HOST window released while the other leader may still use it.** `~island_reduce` walks the islands
   in order: it drains island 0's streams, releases island 0's windows, then drains island 1's streams. In `exchange`
   and `pipe` island 1's `xfer`/`up` stream reads island 0's window (and vice versa); in `star` every leader's `xfer`
   stream uses the root's window. If `run()` throws after such a leg was enqueued (any failing CUDA call), a pinned
   block goes back to the pool while a copy is still queued on it (stale bytes uploaded / a download written into a
   block another thread may already own). Fix: drain every stream of every island first, then release.
2. **Members' fan-out copies are not drained by the destructor** (they run on the members' partial streams and read the
   leader's array). They are joined later by `finish_impl`'s `join_on_failure`, and the arrays outlive them, so this
   is safe today; it relies on that outer join staying.
3. **No fault-injection coverage**: the 12 cases cannot reach `island_reduce` (2 GPUs, or `force_host`). The author's
   "fault binary 12/12 with the switch on" proves nothing about the island error paths (it does show the switch
   does not break the tip paths).
4. Replica relay lifetime (checked in the hash IN-list fan-out): the relay source is a pending replica, and `pending`
   keeps every replica until all destination streams are drained, also on failure ("drain them before propagating"),
   so the relayed peer copy never outlives its source. Correct only while the HOST route completes before
   `enqueue_replica_copy` returns (the author's own caveat 10.9). No fault case uses enough GPUs for a relay to engage.
5. `plan()` runs N x (N-1) `try_grant_pool_access` per finish and `replica_relay` re-reads `getenv` per fan-out:
   prototype shortcuts, as the author says.
6. `relayed_copies()` is a process-wide static counter used by the test only.

## 6. Production issues (verifier's list; items 1-3 are new, the rest confirm the author's)

1. Failure path: release order of the HOST windows in `~island_reduce` (section 5 item 1). Fix before any merge.
2. No test reaches the island error paths. Needed: fault cases on >= 3 GPUs without `force_host` (failed peer copy on the
   other leader, failed event wait, failed batch enqueue mid-chunk) and a route value the tests can assert on.
3. Silent fallbacks: a refused remote lease or fewer than 4 free pinned windows drops to the tip loop with no log line
   above debug and no counter. Under memory pressure the measured gain disappears without a trace.
4. Other leader's scratch not in the upfront reservation; 6 streams + 22 events per finish; topology recomputed per
   finish; `transport` still says HOST_STAGED; three hand-written cross variants; > 2 islands only `star` (never run).
5. Tip-stack side finding (not this prototype): the publisher runs `finish` after its own `hash_partition`, and which task
   publishes is a race. On q21 that is +-26 ms per run and 30-58 ms of avoidable delay before the finish starts.
6. Not measured anywhere: the finish under eager scans (S6) or with a concurrent CONCAT move. In the current stack the two
   never overlap, so "no slowdown of other transfers" says nothing about S6, where `pipe` occupies both pair uplinks.

## 7. What ran, repetitions, variance, what is unverified

Ran (all under the machine lock): 82 timing processes (22 queries), 10 trace-log processes, 4 nsys processes, the author's
two gates, 4 fault-binary runs, 1 unit-test run, VX1 x 6 device sets, VX2, VX3, VX4 (twice; the first VX4 run did not
block the pool and is void), 2 out-of-tree builds of my test binary. Offline: the author's 4 nsys captures re-analysed.

Variance: per-query same-arm process medians differ by <= 4 ms on 9 of 17 queries; q1 5, q3 8 (one 714 ms outlier
iteration in a pipe process, one 751 ms in a switch-off process), q9 9, q10 13, q11 5, q13 16-54, q16 12, q18 9 ms. Suite: +-25 ms (same-arm sums) plus the q13 bimodality.

Unverified:
- Island error paths (CUDA failure mid-run): by code reading only.
- `star` / `exchange` scratch on the other leader: by code only. `star` / `exchange` timing: not re-measured (only `pipe`).
- > 2 islands, full-P2P at G >= 3, no-P2P at G >= 3: cannot be formed on this machine.
- MB4 microbenchmark table (section 4 of the author's note): not re-run; the engine finish times under nsys agree with it
  (q7 54.0 -> 16.0 ms, q10 37.3 -> 11.2 ms, q21 307 -> 85 ms = 3.3-3.6x).
- Relay timing (exact kinds): not re-measured; only its identity gate.
- 5 queries (q14, q15, q17, q19, q20) have one A/P pair only (all within +-2 ms).
- Integration shards and `make test` were not run.

## 0. Checkpoint log

- 01:42 queued the decisive ABBA chain T1 (q21, q3 with arms `A1 P1 O1 O2 P2 A2`; q7, q10, q8, q6, q2 with `A1 P1 P2 A2`).
- 01:47 offline re-analysis of the AUTHOR's nsys captures (q21, q3; tip and pipe; 2 hot iterations each) with the
  verifier's script `bin/vnsys.py` (direction from CUPTI `srcKind`/`dstKind`): `out/author_nsys_vnsys.txt`.
  - q21 finish ranges per hot iteration: tip 280.7 + 26.6 = 307 ms; pipe 76.5 + 8.9 = 85 ms. q3: 62.2 -> 23.4 ms. Reproduced.
  - q21 bytes by transport: tip D2H 5.96 GiB + H2D 8.95 GiB (10 x B, all pinned<->device); pipe D2H 2.98 + H2D 2.98 +
    P2P 5.96 GiB (4 x B host, 4 x B peer). Reproduced to the byte.
  - finish-thread host waits: tip 1932 `cudaStreamSynchronize` = 289 ms; pipe 38 = 58-60 ms. Reproduced.
  - every other copy of the query: same bytes, same summed copy time in both arms (q21 H2D 81.54 GiB / 2985-2991 ms tip
    vs 2977-2987 ms pipe; q3 44.11 GiB / 1613-1615 vs 1617-1618 ms). No slowdown of other transfers.
  - `convert:gpu_to_gpu:host_staging` (the CONCAT move): q21 464 / 484 ms (tip) vs 498 / 473 ms (pipe); q3 403 / 410 vs
    423 / 425 ms (+3-5%, inside the iteration-to-iteration spread of q21; q3 to re-check with my own capture). No such
    range overlaps a finish in either arm, so the finish and the CONCAT move never share the links in this stack.
  - query wall under nsys: q21 1447 / 1452 -> 1260 / 1257 ms; q3 666 / 668 -> 632 / 636 ms.
- 01:50-01:53 q21 `A1 P1 O1 O2 P2 A2` (6 iterations each, first hot dropped, n=8 per arm): tip 1443.3 [1436-1450],
  pipe 1239.5 [1228-1266] = **-203.9 ms (-14.1%)**; proto binary switch off 1448.0 = +4.7 ms (+0.3%, neutral).
  Results identical (md5) across the 6 processes. Claim for q21 (-197.9 ms) reproduced.
- 02:03 author's gates re-run by me (fresh process): `[mbid]` G=4 512 cases, 0 bad; `[xfer_relay]` 24 masks, 6 relayed.
- 02:10-02:29 ABBA so far (n=8 per arm): q3 660.3 -> 623.1 (-37.1, -5.6%; switch off +1.0); q7 746.4 -> 707.4 (-39.0);
  q9 1412.9 -> 1395.4 (-17.6, noisy). Results identical.
- 02:17 my VX1 (inserts left in flight, no host sync before `finish`; 3 sizes x 16 masks x 4 roots x 4 modes x 3 reps):
  2304 finishes, 0 bad.
- 02:22 fault binary, 4 modes: 12/12 each; assertions off 961, pipe 956, star 956, exchange 962 (the count varies
  run to run, it is not a mode effect). BUT: every fault case uses 2 GPUs (`plan()` declines below 3) or 3 GPUs with
  `force_host = true` (prototype forces mode off) -> **no fault-injection case can reach `island_reduce`**.
- 01:59 verifier test binary built out of tree (first attempt, rc=0); chains C (author's gates, fault binary, DF
  multi-GPU unit tests) and V (my tests VX1-VX3) queued.
- 02:33 my VX3 (other GPUs' reservable memory cut to 1 MiB so the other leader's scratch lease is refused; modes off /
  pipe / star / exchange x roots {0, 3}): published, every replica identical, workspace observation = the tip's
  (8 MiB pinned, 7.1 MiB root scratch), finish time = the tip's (14.4 ms vs 4.1 ms for pipe unblocked), all GPU storage
  returned. The fallback the author listed as untested works.
- 02:33 DF multi-GPU unit tests, switch `pipe`: 32 cases pass (4917 assertions). Coverage caveat: 14 of the fixtures in
  the accumulation / fault test files use 2 GPUs, 1 uses 3 with `force_host`; only the few `devices = <variable>` cases
  can reach the island route. The gate that really covers it is `[mbid]` (author) and VX1-VX3 (mine).
- 02:45 ABBA: q4 324.4 -> 310.9 (-13.5), q5 692.6 -> 678.8 (-13.8), q10 849.9 -> 823.5 (-26.4); n=8 per arm; identical results.
- 02:47 engine trace pass q21 + q3 (tip vs pipe, 3 iterations each): per-chunk `join filters kept X/Y` multisets identical
  between arms and iterations (q21 48 chunks, 423,297,173 / 9,124,908,864 rows kept; q3 24 chunks, 160,514,096 /
  1,927,817,321); DYNAMIC_FILTER operator rows per GPU identical. All 6 + 6 Bloom finishes of the pipe arm logged
  `island peer copies + HOST pipelined exchange` (roots GPU 2 and 3 in q21), 33,554,432 scratch bytes on the other leader.
- 02:53 my VX2 (4 threads publish concurrently, roots 0-3, own streams, inserts in flight, 15 rounds, 20 and 57 MiB):
  60 finishes per mode and size, 0 bad, 0 exceptions in off / pipe / star / exchange.
- 03:04-03:16 ABBA: q8 935.1 -> 910.9 (-24.2), q12 -8.4, q16 -5.5 (same-arm spread 6-12), q22 -4.7 (n=8 per arm).
- 03:11 VX1 at 130 MiB (8 MiB chunks, 17 + 6 chunks; 8 masks x 4 roots x 4 modes x 3 reps): 384 finishes, 0 bad.
- 03:16 engine trace pass q7, q8, q10 (tip vs pipe, 3 iterations each): kept multisets identical (q7 182,211,392 /
  2,254,690,081; q8 161,578,660 / 6,605,705,240; q10 86,644,247 / 683,433,385); 6 of 6 finishes per query on the island route.
- 03:27 VX1 on other device sets: `CUDA_VISIBLE_DEVICES=0,2,1,3` (islands interleaved: {0,2} and {1,3} in logical ids)
  1536 finishes, 0 bad; 3 GPUs `1,2,3` and `0,1,3` (pair + single, every root) 576 + 576, 0 bad; 2 GPUs `0,2` (no P2P;
  `plan()` declines, same workspace as off) 96, 0 bad.
