# COMBINED: shadow rule + xfer-d2d + eager probe in one binary, measured against the tip

2026-11-27. Worktree `/opt/dlami/nvme/dfperf/wt-combined`, branch `proto/combined`. Scripts `$R/runs/combined/bin/`,
raw runs `$R/runs/combined/<tag>_<arm>_{a,b}/`, Quent `$R/telemetry/combined_<label>/q<N>/`, tables `$R/runs/combined/out/`.
Every build and GPU process under `flock $R/machine.lock`. Config file untouched; env switches only.
STATUS: see the checkpoint log at the end (authoritative for what is finished).

## 1. Recipe (exact)

```bash
R=/opt/dlami/nvme/dfperf
git -C $R/repo worktree add -b proto/combined $R/wt-combined prod/shadow-2570      # cd89bbc8
cd $R/wt-combined
git cherry-pick 09a04c32 c15b053e a6b8c1da b7d0eb81    # proto/xfer-d2d (microbench commit 09a04c32 included: no conflict)
git cherry-pick f05cfd12                               # proto/eager-probe
# -> ede040e3 4a205119 e3e5d0f7 bc7cf1ff e013f8d0 ; ZERO conflicts in all five picks
# + c5aa5bdf proto(combined): SIRIUS_PROTO_SHADOW_OFF=1 (measurement switch, 7 lines in judge_shadow(),
#   src/op/dynamic_filter/construction_plan.cpp: no key is judged shadowed -> tip plan). Not for any PR.
git submodule update --init --recursive
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache TMPDIR=$R/tmp
flock $R/machine.lock pixi run make > $R/logs/build_combined.log 2>&1              # $R/tmp/build_combined.sh
```

Switches in the one binary (all unset = shadow rule only, since the shadow rule is production code without a switch):

| piece | on | off |
|---|---|---|
| S shadow rule | default | `SIRIUS_PROTO_SHADOW_OFF=1` (added here) |
| X xfer-d2d | `SIRIUS_PROTO_XFER_D2D=pipe` | unset |
| E eager probe (option 1, joins with an enabled filter plan) | `SIRIUS_EAGER_PROBE=1` | unset |

Arms: `T` = tip binary `$R/repo` (pr2570 ae939824); `O` = combined binary, all three off; `S X E SX SE XE SXE` = combined
binary with those pieces on. `bin/arm.sh <arm> <label> <q> <iters>`; `bin/abba.sh <tag> <iters> "<arms>" <q...>` runs per
query a forward pass over the arms then a reverse pass (lock held per pass), each process 1 warm + 5 hot, first hot
dropped -> 4 kept per process, 8 hot per arm.

## 0. Checkpoint log (chronological; append only)

- 11-27: worktree + branch created, 5 picks clean, switch commit c5aa5bdf, build started (`$R/logs/build_combined.log`).
