#!/bin/bash
# Usage: flock $R/machine.lock build.sh [make-target]
set -u
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp; mkdir -p $TMPDIR
cd $R/wt-op1a
pixi run make "$@" > $R/runs/op1a/logs/build.log 2>&1
rc=$?
echo "build rc=$rc $(date +%T)" >> $R/runs/op1a/logs/build_status.txt
exit $rc
