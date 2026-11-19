#!/bin/bash
# Usage: unit.sh <mode> <out-file> <catch2 args...>   -- DF unit tests on the op1b build with SIRIUS_DF_DEFER_PUBLISH=<mode>
# MUST run under: flock /opt/dlami/nvme/dfperf/machine.lock unit.sh ...
mode=$1; out=$2; shift 2
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
export SIRIUS_TEST_LOG_DIR=$R/runs/op1b/unit_logs/m$mode; mkdir -p $SIRIUS_TEST_LOG_DIR
cd $R/wt-op1b
SIRIUS_DF_DEFER_PUBLISH=$mode pixi run build/release/test/cpp/sirius_unittest "$@" > $out 2>&1
echo "UNIT_DONE mode=$mode rc=$? $(date +%T)" >> $out
