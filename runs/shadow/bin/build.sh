#!/bin/bash
# Usage: build.sh <worktree-name> [make target]   e.g. build.sh wt-prod-69
# Run under: flock /opt/dlami/nvme/dfperf/machine.lock build.sh ...
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp; mkdir -p $TMPDIR
wt=$1; shift
log=$R/logs/build_${wt}.log
cd $R/$wt && pixi run make "$@" > $log 2>&1
rc=$?
echo "BUILD_DONE rc=$rc $(date +%T)" >> $log
exit $rc
