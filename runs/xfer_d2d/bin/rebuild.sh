#!/bin/bash
# incremental rebuild (under flock): full make (library + unittest + extension)
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
cd $R/wt-xfer-d2d && pixi run make > $R/logs/build_xfer_d2d_${1:-re}.log 2>&1
echo "rc=$? $(date +%T)" >> $R/logs/build_xfer_d2d_${1:-re}.log
