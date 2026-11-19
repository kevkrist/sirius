#!/bin/bash
# Build the eager worktree. Run under: flock /opt/dlami/nvme/dfperf/machine.lock build.sh   (no-op if already built)
export R=/opt/dlami/nvme/dfperf
grep -q "BUILD_DONE rc=0" $R/logs/eager_build.log 2>/dev/null && exit 0
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp; mkdir -p $TMPDIR
cd $R/wt-eager
pixi run make > $R/logs/eager_build.log 2>&1
rc=$?
echo "BUILD_DONE rc=$rc $(date +%T)" >> $R/logs/eager_build.log
exit $rc
