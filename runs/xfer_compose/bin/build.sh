#!/bin/bash
# build wt-xfer-compose (run under flock)
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
cd $R/wt-xfer-compose && pixi run make > $R/logs/build_xfer_compose.log 2>&1
echo "rc=$? $(date +%T)" >> $R/logs/build_xfer_compose.log
