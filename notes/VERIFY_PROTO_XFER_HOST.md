# VERIFY_PROTO_XFER_HOST: adversarial verification of proto/xfer-host (pipelined host bounce)

2026-11-20. Verifier (independent of the prototype author). Prototype: worktree `/opt/dlami/nvme/dfperf/wt-xfer-host`,
branch `proto/xfer-host` head `699b3dd8` (NOT modified by me; binaries as built by the author 2026-11-19 21:21, no source newer
than `libsirius.so`). Tip reference binary: `/opt/dlami/nvme/dfperf/repo` (`ae939824`).
My scripts: `$R/runs/vxfer_host/bin/`, raw output `$R/runs/vxfer_host/`, Quent `$R/telemetry/vxfer-host*`.
Every GPU process ran under `flock $R/machine.lock`. Config untouched; only env switches.

STATUS: IN PROGRESS (checkpoint 1). Sections fill in as lock turns arrive (13 waiters on the lock).

## 0. Checkpoint log

- 01:06-01:08 q21 `A1 U1 O1 O2 U2 A2` (6 iterations each, first hot dropped, n=8 per arm):
  tip 1441.4 [1417-1456], auto 1226.9 [1219-1243] = **-214.6 ms (-14.9%)**; proto binary switch off 1441.8 = +0.4 ms.
  Results identical (md5) across the 6 processes. Claim for q21 (-208.5 ms) reproduced.
- Offline re-analysis of the author's nsys captures (q21, 1 capture per arm) with my own script `bin/vnsys.py`
  (direction taken from CUPTI `srcKind`/`dstKind` = pinned vs device memory, which is reliable where `copyKind` says PtoP):
  - finish range sum per hot iteration: tip 293 / 308 ms, auto 91 / 74 ms.
  - finish copies: tip D2H 5.96 GiB + H2D 8.95 GiB = 14.91 GiB (10 x B); auto D2H 5.96 + H2D 5.96 = 11.93 GiB (8 x B).
    Every finish leg is pinned<->device in both arms (no device-to-device leg).
  - tip: the 4 GPUs' copy-busy times inside the finish add up to 340 ms within 293 ms of wall (serial legs);
    auto: 70 ms on each of the 4 GPUs inside 81 ms of wall (parallel legs, 43 GiB/s per GPU both directions together).
  - finish-thread waits: tip 1932 `cudaStreamSynchronize` = 274-289 ms; auto 1504 `cudaEventSynchronize` = 34-44 ms + 18 stream syncs = 1 ms.
  - every other copy of the query (not issued by the finish): same bytes and same summed copy time in both arms
    (H2D 81.54 GiB / 2980 ms vs 2981 ms; D2H 5.70 GiB; P2P 2.85 GiB; local 33.17 GiB). `convert:gpu_to_gpu:host_staging`
    (the CONCAT move) 479 / 442 ms (tip) vs 479 / 467 ms (auto): no slowdown visible; no such range overlaps a finish in either arm.
