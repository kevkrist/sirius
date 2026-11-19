#!/bin/bash
# Build the op1b worktree. Run under: flock /opt/dlami/nvme/dfperf/machine.lock build.sh
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp; mkdir -p $TMPDIR
cd $R/wt-op1b
pixi run make > $R/logs/op1b_build.log 2>&1
rc=$?
echo "BUILD_DONE rc=$rc $(date +%T)" >> $R/logs/op1b_build.log
exit $rc
