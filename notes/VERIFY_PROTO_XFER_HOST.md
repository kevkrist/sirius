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
- 01:40-02:11 (checkpoint 3):
  - ABBA: q7 -42.6, q8 -26.0, q10 -20.8 (claimed -33.0), q6 +0.1, q2 -1.5 (all n=8).
  - Own nsys (q21, q7; 1 capture per arm): q21 finish 291 / 308 -> 73 / 74 ms, q7 54 -> 14.5 ms; other copies and the CONCAT move unchanged.
  - Microbench MB4P re-run (5 reps): tip 61 / 104 / 144 / 186 ms/GiB for batches on 1 / 2 / 3 / 4 GPUs; hostor 39 (fan-out) / 46-48 / 46-48 / 44-47;
    gpuor 39 / 45-47 / 76 / 95-96. Matches the author within 1-2%.
  - Unit tests, 4 GPUs: `auto` 4 failing cases of 159 (as claimed); `gpuor` the same 4; `hostor` 5 (extra:
    `a tracked task thread admits a merge scratch larger than its reservation as its task would`, i.e. on a P2P pair, when the PEER
    pipeline's scratch is refused, hostor publishes through the host instead of skipping: the prototype IS reached on a full-P2P machine
    in that case). Fault binary: `hostor` and `gpuor` 11/12 like `auto` (same submission-order expectation).
  - Identity gate on other GPU sets: `CUDA_VISIBLE_DEVICES=2,0,3,1` G=4 (540 finishes), `1,2,3` G=3 (540), `0,1,2` G=3 (540), `3,1` G=2 (450): all identical.
  - Recommended configuration W=2 + 8 OR threads against the defaults (W=4, 16): q21 1227.9 vs 1228.8 ms (n=8 each), q3 626.2 vs 624.6 (n=4): no difference.
  - Engine repetition (auto, 12 iterations in one process, debug log): q21, q3 per-chunk kept multisets equal to the tip's in 12/12 iterations; q7 12/12
    self-consistent; the publication root lands on all 4 GPUs across iterations; no error / fallback line.
