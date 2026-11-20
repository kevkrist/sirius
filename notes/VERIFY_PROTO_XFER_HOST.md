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
- 01:18 identity gates re-run by me (same test, fresh process): `[xfer_proto]` G=4 540 finishes / 2592 arrays byte-identical;
  `[xfer_proto_exact]` 96 sessions / 576 masks identical. Fault-injection binary `sirius_cuda_fault_test`
  (`[cuda_status_fault][multi_gpu]`, 12 cases): switch off 12/12 pass; `auto` 11/12 - the one failure is
  `HOST publication submits every target upload before waiting for any` (`submissions_during_delay() == 1`, got 18:
  it pins the tip loop's submission order). The injected-failure cases (error at copy batch 1 / 3 / 12, unjoined stream sync)
  pass with the switch on: streams joined, pinned HOST bytes and device bytes back to baseline.
- 01:19-01:38 ABBA so far (ms, median of kept hot iterations): q3 665.6 -> 624.0 (-41.5, n=8; switch off -3.4);
  q7 747.3 -> 704.7 (-42.6, n=8); q4 -14.2, q5 -12.2, q10 -20.0, q9 -4.7, q12 -5.7, q13 -10.4, q18 +7.9 (n=4 each).
- 01:29 engine log pass (trace, 2 iterations per arm) q21 + q3: per-chunk `join filters kept X/Y` multisets identical between tip and
  auto and between iterations (q21: 48 chunks, 423,297,173 of 9,124,908,864 rows kept; q3: 24 chunks, 160,514,096 of 1,927,817,321),
  while the scan tasks rotate over the GPUs between iterations. Schedules seen: q21 two hostor finishes (160 MiB pinned each),
  q3 one fan-out (32 MiB) + one hostor (160 MiB).
