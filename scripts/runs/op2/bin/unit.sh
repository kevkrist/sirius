#!/bin/bash
# DF unit tests of the prototype build with the switches off (default) -- run under flock.
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
out=$R/runs/op2/unit; mkdir -p $out
cd $R/wt-op2
CUDA_VISIBLE_DEVICES=0 pixi run build/release/test/cpp/sirius_unittest "[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]" > $out/df_single.txt 2>&1
echo "df_single rc=$? $(tail -3 $out/df_single.txt | tr '\n' ' ')" >> $out/status.txt
CUDA_VISIBLE_DEVICES=0 pixi run build/release/test/cpp/sirius_unittest "[publisher],[construction_plan],[physical_partition],[build_filter]" > $out/df_publisher.txt 2>&1
echo "publisher/plan/partition/build_filter rc=$? $(tail -3 $out/df_publisher.txt | tr '\n' ' ')" >> $out/status.txt
pixi run build/release/test/cpp/sirius_unittest "[dynamic_filter][multi_gpu]" > $out/df_mgpu.txt 2>&1
echo "df_mgpu rc=$? $(tail -3 $out/df_mgpu.txt | tr '\n' ' ')" >> $out/status.txt
