# PROTO_XFER_COMPOSE: step 0 of DESIGN_TRANSFER_PR (measuring spike, throwaway) [xfer-PR]

2026-11-28. Branch `proto/xfer-compose` (from `proto/xfer-d2d` b7d0eb81), worktree `$R/wt-xfer-compose`.
Runs / scripts: `$R/runs/xfer_compose/{bin,mb,...}`. Question (user Q1): cross-island OR on the CPU or on the leaders' GPUs?

## 1. What was built (one binary, switch `SIRIUS_PROTO_XFER_D2D`)

| Value | Stage A (in island) | Stage B (between island leaders) | Stage C |
|---|---|---|---|
| unset / `off` | - | tip code path (serial HOST star) | - |
| `pipe` (GPU OR, = proto/xfer-d2d unchanged) | P2P merge | each leader downloads to its pinned window; the other leader uploads into device scratch and ORs on its GPU; event ordered, 0 host waits | P2P fan-out |
| `compose` (CPU OR, the design's form) | P2P merge | each input leader downloads to its pinned window (W = 2 chunks in flight); publishing thread waits the download events, ORs the windows on the CPU (`host_or` of proto/xfer-host, AVX-512 + NT stores, 8 threads default, `SIRIUS_PROTO_XFER_OR_THREADS`), every leader uploads the OR output | P2P fan-out |

- Single contributing island (`Kc == 1`): `compose` runs the same event graph as `pipe` (pass-through, no OR, no host wait).
- `compose` is written for any number of islands; `SIRIUS_PROTO_XFER_SINGLETONS=1` pretends no pair has P2P (all-singleton plan
  through the same code = the design's no-P2P path).
- Code: `src/cuda/sirius_dynamic_bloom_filter.cu` (`island_reduce::hostor_download`, `hostor_cross`), `src/cuda/proto_xfer.cpp`
  + `src/memory/proto_xfer.hpp` (copied from proto/xfer-host; only `host_or` / `or_threads` used).
- DEBUG finish line gained `SPIKE finish_ms= host_or_ms= host_wait_ms= host_window_bytes=`.

## 2. Correctness gate (bit identity, `bin/gate.sh`, `mb/gate_*.log`)

`[mbid]`: every replica array `memcmp`'d against the `off` (tip loop) result; modes off / pipe / compose; 2 keys of different size
(1 to 8+ chunks, both parities, window wrap), every contribution mask, root 0 and last GPU.
4 GPUs 384 cases, GPUs 0,1,2 96, GPUs 0,2,3 96, 4 GPUs forced singletons (compose) 256: **bad = 0 in all** (re-run after each code change).

## 3. Microbench MB4 (builder-level `finish` wall, G = 4, root GPU 0, 8 MiB chunks, median of 5 after 1 warm-up)

`bin/mb4c.sh _final` -> `mb/mb4_g4_final.log`, `mb/mb4_single_final.log`. ms (ms/GiB); spread max-min <= 1.8% unless noted.

| batches on | MiB | off (tip loop) | pipe (GPU OR) | compose (CPU OR) | compose, forced singletons (no P2P) |
|---|---|---|---|---|---|
| all 4 (Kc = 2) | 57 | 10.47 (188) | 3.06 (54.9) | 3.09 (55.5) | 3.02 (54.3) |
| all 4 | 174 | 31.58 (186) | 8.92 (52.5) | 9.07 (53.4) | 7.92 (46.6) |
| all 4 | 281 | 51.00 (186) | 14.28 (52.0) | 14.40 (52.5) | 12.35 (45.0) |
| all 4 | 1434 | 259.86 (186) | 72.21 (51.6) | 72.41 (51.7) | 60.00 (42.8) |
| GPUs 0+2 (Kc = 2, no merge) | 57 | 5.79 (104) | 2.50 (44.8) | 2.72 (48.9) | 2.72 (48.9) |
| GPUs 0+2 | 174 | 17.52 (103) | 7.38 (43.4) | 7.55 (44.4) | 7.26 (42.7) |
| GPUs 0+2 | 281 | 28.34 (103) | 11.87 (43.3) | 12.00 (43.7) | 11.34 (41.3) |
| GPUs 0+2 | 1434 | 144.62 (103) | 60.18 (43.0) | 59.81 (42.7) | 55.65 (39.7) |
| pair 0-1 (Kc = 1) | 57 / 174 / 281 / 1434 | 104 / 103 / 103 / 103 ms/GiB | 44.6 / 41.6 / 40.8 / 40.3 | 44.2 / 41.5 / 40.8 / 40.3 | 53.4 / 46.0 / 44.7 / 42.8 |
| root only (Kc = 1) | 57 / 174 / 281 / 1434 | 61.6 / 61.0 / 61.0 / 60.9 | 41.0 / 39.8 / 39.5 / 39.2 | 40.8 / 39.7 / 39.5 / 39.2 | 43.2 / 40.1 / 39.5 / 38.7 |
| GPU 2 only (Kc = 1) | 57 / 174 / 281 / 1434 | 104 / 103 / 103 / 103 | 41.0 / 39.9 / 39.6 / 39.3 | 40.8 / 39.9 / 39.6 / 39.3 | 43.4 / 40.2 / 39.5 / 38.7 |

Small arrays (`mb/mb4_small_final.log`, median of 9, ms): off / pipe / compose
- root only: 1 MiB 0.083 / 0.155 / 0.142; 4 MiB 0.273 / 0.269 / 0.267; 16 MiB 1.074 / 0.735 / 0.723.
- all 4: 1 MiB 0.269 / 0.226 / 0.277; 4 MiB 0.869 / 0.394 / **0.533**; 16 MiB 3.372 / 1.060 / **1.507**.
  CPU OR is 35-40% slower than GPU OR on 4-16 MiB arrays with two inputs (per-chunk host wait on 2 MiB chunks), still 1.6-2.2x
  faster than the tip. Absolute: <= 0.45 ms per such filter.

Design gates (section 11, microbench rows): all-four 281 / 1434 <= 56: compose 52.5 / 51.7 PASS (pipe 52.0 / 51.6). All-four 57
<= 62: 55.5 PASS. Root only <= 44: 39.2-40.8 PASS. One pair / one GPU of the other pair <= 46: 40.3-44.2 / 39.3-40.8 PASS. Forced
singletons all four <= 60: 42.8-54.3 PASS. Fixed cost 1 MiB <= tip + 0.10 ms: +0.06 / +0.01 PASS (with stream creates).

### 3.1 Two things the first cut got wrong (both change the design text)

1. **Merge lookahead.** First cut enqueued merge k+2 only after the host OR of chunk k (stage A coupled to the host loop):
   all-four 55.3-62.9 ms/GiB (gate fail at 57 and 281 MiB). Enqueueing the merges 2 chunks ahead of the downloads (legal bound:
   a download binds to its merge's event; one scratch-parity cycle) -> 51.7-55.5 = ties pipe. `SIRIUS_PROTO_XFER_MERGE_AHEAD=0`
   reproduces the slow form (`mb/mb4_g4_a0_t4.log` vs `a2_t4`: 57.4 -> 51.8 at 1434 MiB).
2. **CPU OR threads: more is worse.** ms/GiB at 57 / 281 / 1434 MiB (`mb/mb4b_*`):

| OR threads | islands, all 4 (Kc = 2) | islands, GPUs 0+2 (Kc = 2) | singletons, all 4 (Kc = 4) | singletons, GPUs 0-2 (Kc = 3) |
|---|---|---|---|---|
| 2 | 70.4 / 58.5 / 55.1 | 64.3 / 54.2 / 51.4 | | |
| 3 | 56.6 / 51.7 / 50.7 | 48.6 / 43.9 / 42.8 | | |
| 4 | 56.5 / 52.8 / 52.0 | 48.2 / 43.7 / 42.8 | 67.9 / 55.2 / 52.8 | 55.5 / 45.2 / 43.1 |
| 6 | 58.8 / 54.9 / 54.9 | 49.8 / 46.7 / 46.2 | 55.5 / 45.0 / 42.9 | 53.3 / 44.7 / 42.8 |
| 8 | 59.3 / 55.0 / 54.5 | 53.6 / 47.8 / 47.6 | 54.6 / 45.1 / 42.8 | 54.2 / 45.1 / 43.1 |
| 12 | 66.8 / 57.8 / 56.9 | 69.4 / 61.7 / 63.0 | | |
| 16 (first cut, before merge lookahead) | 67.3 / 56.3 / 54.4 | 65.3 / 63.1 / 69.4 | | |

   With 8 threads the OR itself is 2x faster than with 4 (17 vs 33 ms per 1434 MiB) but the finish is 5-10% slower: the extra
   (spinning) threads slow the transfers. Best point = 2 threads per input: `min(8, 2 x Kc)`, used from here on (default of the
   spike; `SIRIUS_PROTO_XFER_OR_THREADS` overrides).
   Host time on the publishing thread, all four, 1434 MiB, 4 threads: OR 33 ms + event waits 8 ms of a 72 ms finish.

Memory held (trace line), G = 4, two pair islands, all contributing, 8 MiB chunks: compose pinned 48 MiB, root scratch 16 MiB,
other leader 16 MiB (lease). pipe: pinned 32 MiB, root 32 MiB, other leader 32 MiB. Tip: 8 / 8 / 0.

## Checkpoint log

- 01:20 worktree + branch created (submodules: `git -c protocol.file.allow=always submodule update --init --recursive -- cucascade
  duckdb sirius-duckdb/duckdb sirius-duckdb/extension-ci-tools sirius-duckdb/vcpkg substrait .claude/claude-tools`).
- 01:29 first build ok; commit 2c817a03 (compose mode). Gate bad=0. First microbench: compose 55-63 vs pipe 52-55 (all four).
- 01:48 commit bfbf2a67 (merge lookahead 2, OR threads min(8, 2 x Kc), trace line). Gate bad=0. Final microbench tables (section 3).
- 01:50 engine ABBA started: `bin/abba3_block.sh e1 6 21 3 7` (labels `e1_{tip,cpu,gpu}{A,B}`); next: `e1 6 8 10 9`, `e1 6 6 2`,
  then `bin/logpass.sh 21 3 7 8 10 9`; tables: `bin/abba3_table.py e1`, `bin/log_table.py 21 3 7 8 10 9`.
