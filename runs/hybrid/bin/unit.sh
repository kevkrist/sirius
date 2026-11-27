#!/bin/bash
# DF unit-test gate on wt-hybrid; each invocation takes the lock.
R=/opt/dlami/nvme/dfperf; U=$R/runs/hybrid/unit; mkdir -p $U
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache TMPDIR=$R/tmp
cd $R/wt-hybrid; T=build/release/test/cpp/sirius_unittest
run() { name=$1; shift; flock $R/machine.lock env "$@" > $U/$name.log 2>&1; echo "$name rc=$? $(tail -3 $U/$name.log | grep -E 'passed|failed' | tr -d '\n')" >> $U/summary.txt; }
run sgpu_off CUDA_VISIBLE_DEVICES=0 pixi run $T "[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]"
run mgpu_off pixi run $T "[dynamic_filter][multi_gpu]"
run sgpu_on1 CUDA_VISIBLE_DEVICES=0 SIRIUS_PROTO_ONESHOT=1 pixi run $T "[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]"
run mgpu_on1 SIRIUS_PROTO_ONESHOT=1 pixi run $T "[dynamic_filter][multi_gpu]"
run mgpu_on1_eager SIRIUS_PROTO_ONESHOT=1 SIRIUS_EAGER_PROBE=1 pixi run $T "[dynamic_filter][multi_gpu]"
echo UNITDONE >> $U/summary.txt
