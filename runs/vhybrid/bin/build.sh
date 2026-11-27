#!/bin/bash
# build wt-vhybrid (run under flock)
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache TMPDIR=$R/tmp
cd $R/wt-vhybrid && pixi run make > $R/logs/build_vhybrid.log 2>&1
echo "build rc=$? $(date +%T)" >> $R/runs/vhybrid/build.status
