#!/bin/bash
# flock $R/machine.lock build_ut.sh : rebuild sirius_unittest only (bench changes)
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
cd $R/wt-op1a
pixi run cmake --build --preset release --target sirius_unittest > $R/runs/op1a/logs/build_ut.log 2>&1
echo "build_ut rc=$? $(date +%T)" >> $R/runs/op1a/logs/build_status.txt
