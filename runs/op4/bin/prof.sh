#!/bin/bash
# prof.sh nsys|ncu <name> <bench args...>   (under flock)
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache TMPDIR=$R/tmp
WT=$R/wt-op4; BIN=$WT/build/release/test/bench/df_bloom_bench; OUT=$R/runs/op4/mb
mode=$1; name=$2; shift 2
cd $WT; export CUDA_VISIBLE_DEVICES=0
if [ $mode = nsys ]; then
  pixi run nsys profile --trace=cuda,nvtx --sample=none --cudabacktrace=none --force-overwrite=true \
    --export=sqlite -o $OUT/nsys_$name $BIN "$@" > $OUT/nsys_$name.log 2>&1
else
  pixi run /usr/local/cuda/bin/ncu --target-processes all --set full --kernel-name-base demangled \
    --kernel-name "regex:membership_probe_functor" --launch-skip 20 --launch-count 8 -f -o $OUT/ncu_$name $BIN "$@" > $OUT/ncu_$name.log 2>&1
  echo "ncu rc=$?" >> $OUT/ncu_$name.log
fi
