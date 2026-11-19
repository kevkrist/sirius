#!/bin/bash
# Usage (under the machine lock): build2.sh <worktree-name> <logname>   e.g. build2.sh wt-eager2 eager2_build
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp; mkdir -p $TMPDIR
cd $R/$1
t0=$(date +%s)
pixi run make > $R/logs/$2.log 2>&1
rc=$?
echo "BUILD_DONE rc=$rc $(date +%T) secs=$(( $(date +%s) - t0 ))" >> $R/logs/$2.log
exit $rc
