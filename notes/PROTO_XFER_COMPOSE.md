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

(results below are filled in as they are measured)

## Checkpoint log

- 01:40 worktree + branch created (submodules: `git -c protocol.file.allow=always submodule update --init --recursive -- cucascade
  duckdb sirius-duckdb/duckdb sirius-duckdb/extension-ci-tools sirius-duckdb/vcpkg substrait .claude/claude-tools`).
- 02:00 compose mode written (uncommitted), scripts in `runs/xfer_compose/bin`, first build started under the lock.
