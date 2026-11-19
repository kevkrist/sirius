#!/bin/bash
# Usage: unit_tip.sh <out-file> <catch2 args...>  -- unit tests on the tip build ($R/repo), read-only use of its binary.
out=$1; shift
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
export SIRIUS_TEST_LOG_DIR=$R/runs/op1b/unit_logs/tip; mkdir -p $SIRIUS_TEST_LOG_DIR
cd $R/repo
pixi run build/release/test/cpp/sirius_unittest "$@" > $out 2>&1
echo "UNIT_DONE tip rc=$? $(date +%T)" >> $out
