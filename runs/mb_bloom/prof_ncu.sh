#!/bin/bash
# usage: prof_ncu.sh <name> <kernel-regex> <launch-skip> <launch-count> <bench args...>
source /opt/dlami/nvme/dfperf/runs/mb_bloom/env.sh
BIN=$WT/build/release/test/bench/df_bloom_bench
name=$1; regex=$2; skip=$3; count=$4; shift 4
mkdir -p $OUT/ncu; cd $WT
export CUDA_VISIBLE_DEVICES=0
pixi run /usr/local/cuda/bin/ncu --target-processes all --set full --kernel-name-base demangled --kernel-name "regex:$regex" \
  --launch-skip $skip --launch-count $count -f -o $OUT/ncu/$name $BIN "$@" > $OUT/ncu/$name.log 2>&1
/usr/local/cuda/bin/ncu --import $OUT/ncu/$name.ncu-rep --page details --csv > $OUT/ncu/$name.csv 2>> $OUT/ncu/$name.log
