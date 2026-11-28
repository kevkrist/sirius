#!/bin/bash
# build wt-hybrid-full (takes the lock itself)
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache TMPDIR=$R/tmp
cd $R/wt-hybrid-full && flock $R/machine.lock pixi run make > $R/logs/build_hybridfull.log 2>&1
echo "build rc=$? $(date +%T)" >> $R/runs/hybridfull/build.status
