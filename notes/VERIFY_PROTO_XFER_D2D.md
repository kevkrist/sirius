# VERIFY_PROTO_XFER_D2D: adversarial verification of proto/xfer-d2d (per-island D2D + host hop between pairs)

2026-11-20. Verifier (independent of the prototype author). Prototype: worktree `/opt/dlami/nvme/dfperf/wt-xfer-d2d`,
branch `proto/xfer-d2d` head `b7d0eb81` (NOT modified by me; binaries as built by the author 2026-11-19 20:33-20:34,
no source file newer than them). Tip reference binary: `/opt/dlami/nvme/dfperf/repo` (`ae939824`).
My scripts: `$R/runs/vxfer_d2d/bin/`, raw output `$R/runs/vxfer_d2d/`, Quent `$R/telemetry/vxfer-d2d*`.
My own tests: `$R/runs/vxfer_d2d/src/vx_tests.cpp` (copy `bin/vx_tests.cpp.txt`), compiled OUT OF TREE with the
prototype's compile flags and linked with the prototype's unchanged test objects into `vxbin/sirius_unittest_vx`
(`bin/vx_build.sh`); nothing is written into the prototype worktree.
Every build and GPU process ran under `flock $R/machine.lock`. Config untouched; only env switches.

STATUS: IN PROGRESS. Sections fill in as lock turns arrive (9 waiters on the lock at 01:45).

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
