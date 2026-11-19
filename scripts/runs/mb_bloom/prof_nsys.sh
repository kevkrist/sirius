#!/bin/bash
# usage: prof_nsys.sh <name> <bench args...>
source /opt/dlami/nvme/dfperf/runs/mb_bloom/env.sh
BIN=$WT/build/release/test/bench/df_bloom_bench
name=$1; shift
mkdir -p $OUT/nsys; cd $WT
export CUDA_VISIBLE_DEVICES=0
pixi run nsys profile --trace=cuda,nvtx --sample=none --cudabacktrace=none --force-overwrite=true \
  --export=sqlite -o $OUT/nsys/$name $BIN "$@" > $OUT/nsys/$name.log 2>&1
