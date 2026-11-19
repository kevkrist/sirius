#!/bin/bash
# Under the machine lock: incremental rebuild of wt-eager2; log logs/eager2_build_<tag>.log
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
cd $R/wt-eager2
t0=$(date +%s)
pixi run make > $R/logs/eager2_build_$1.log 2>&1
rc=$?
echo "BUILD_DONE rc=$rc $(date +%T) secs=$(( $(date +%s) - t0 ))" >> $R/logs/eager2_build_$1.log
exit $rc
