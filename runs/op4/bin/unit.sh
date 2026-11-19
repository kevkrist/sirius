#!/bin/bash
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache TMPDIR=$R/tmp
export SIRIUS_LOG_DIR=$R/runs/op4/unit_logs; mkdir -p $SIRIUS_LOG_DIR
cd $R/wt-op4
CUDA_VISIBLE_DEVICES=0 pixi run build/release/test/cpp/sirius_unittest "$@" 2>&1 | tail -6
