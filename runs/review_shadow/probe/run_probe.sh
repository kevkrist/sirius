#!/bin/bash
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
P=$R/runs/review_shadow/probe; cd $R/wt-prod-70
CUDA_VISIBLE_DEVICES=0 pixi run $P/rv_unittest "[rv_probe],[scan_merge][shadow]" > $P/run.txt 2>&1; echo "rc=$?" >> $P/run.txt
