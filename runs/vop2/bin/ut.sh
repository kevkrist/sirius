#!/bin/bash
# DF unit tests on the prototype build: switches off, then SHADOW=1 LATCH=1. Run as ONE locked command.
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
out=$R/runs/vop2/unit; mkdir -p $out
cd $R/wt-op2
U=build/release/test/cpp/sirius_unittest
run() { # <name> <env...> -- <args...>
  name=$1; shift; envs=(); while [ "$1" != "--" ]; do envs+=("$1"); shift; done; shift
  env "${envs[@]}" pixi run $U "$@" > $out/$name.txt 2>&1
  echo "$name rc=$? $(tail -3 $out/$name.txt | tr '\n' ' ') $(date +%T)" >> $out/status.txt
}
for mode in off on; do
  if [ $mode = on ]; then E=(SIRIUS_PROTO_OP2_SHADOW=1 SIRIUS_PROTO_OP2_LATCH=1); else E=(VOP2_DUMMY=1); fi
  run df_single_$mode CUDA_VISIBLE_DEVICES=0 "${E[@]}" -- "[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]"
  run df_pub_$mode CUDA_VISIBLE_DEVICES=0 "${E[@]}" -- "[publisher],[construction_plan],[physical_partition],[build_filter],[in_task_publication]~[multi_gpu]"
  run df_mgpu_$mode "${E[@]}" -- "[dynamic_filter][multi_gpu]"
done
