#!/bin/bash
# Run under the machine lock. Builds wt-eager2 (OP1B mechanics + eager activation after the deferred publication = option 2)
# if needed, then smoke q3 q12 q16 at DEBUG with SIRIUS_DF_DEFER_PUBLISH=1 SIRIUS_EAGER_PROBE=1; GO2 / NOGO2.
export R=/opt/dlami/nvme/dfperf; B=$R/runs/eager/bin
if ! grep -q "BUILD_DONE" $R/logs/eager2_build.log 2>/dev/null; then
  export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
  export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
  (cd $R/wt-eager2 && pixi run make > $R/logs/eager2_build.log 2>&1; echo "BUILD_DONE rc=$? $(date +%T)" >> $R/logs/eager2_build.log)
fi
grep -q "BUILD_DONE rc=0" $R/logs/eager2_build.log || { echo "build2 failed"; exit 5; }
for q in 3 12 16; do
  SIRIUS_LOG_LEVEL=debug SIRIUS_DF_DEFER_PUBLISH=1 SIRIUS_EAGER_PROBE=1 REPO=$R/wt-eager2 timeout 900 $B/one_query.sh smoke2_dbg $q 3
done
SMOKE_LABEL=smoke2_dbg python3 $B/smoke_check.py > $R/runs/eager/smoke2_check.txt 2>&1 && touch $R/runs/eager/GO2 || touch $R/runs/eager/NOGO2
# option 1 on the same binary with the scan throttle (K=1)
for q in 3 12 16; do
  SIRIUS_LOG_LEVEL=debug SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=1 SIRIUS_EAGER_SCAN_THROTTLE=1 REPO=$R/wt-eager2 timeout 900 $B/one_query.sh smoke3_dbg $q 3
done
SMOKE_LABEL=smoke3_dbg python3 $B/smoke_check.py > $R/runs/eager/smoke3_check.txt 2>&1 && touch $R/runs/eager/GO3 || touch $R/runs/eager/NOGO3
