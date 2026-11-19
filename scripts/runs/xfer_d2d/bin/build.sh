#!/bin/bash
# build wt-xfer-d2d (run under flock). arg: "all" (make) or a cmake target list (default: sirius_unittest + extension via make)
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
cd $R/wt-xfer-d2d && pixi run make > $R/logs/build_xfer_d2d.log 2>&1
echo "rc=$? $(date +%T)" >> $R/logs/build_xfer_d2d.log
