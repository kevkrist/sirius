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
- 01:59 verifier test binary built out of tree (first attempt, rc=0); chains C (author's gates, fault binary, DF
  multi-GPU unit tests) and V (my tests VX1-VX3) queued.
