#!/bin/bash
# DF unit-test gate on wt-combined; each invocation takes the lock.
R=/opt/dlami/nvme/dfperf; U=$R/runs/combined/unit
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache TMPDIR=$R/tmp
cd $R/wt-combined; T=build/release/test/cpp/sirius_unittest
run() { name=$1; shift; flock $R/machine.lock env "$@" > $U/$name.log 2>&1; echo "$name rc=$? $(tail -3 $U/$name.log | grep -E 'passed|failed' | tr -d '\n')" >> $U/summary.txt; }
run sgpu_default CUDA_VISIBLE_DEVICES=0 pixi run $T "[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]"
run mgpu_default pixi run $T "[dynamic_filter][multi_gpu]"
run mgpu_pipe SIRIUS_PROTO_XFER_D2D=pipe pixi run $T "[dynamic_filter][multi_gpu]"
run mgpu_all SIRIUS_PROTO_XFER_D2D=pipe SIRIUS_EAGER_PROBE=1 pixi run $T "[dynamic_filter][multi_gpu]"
run sgpu_shadowoff CUDA_VISIBLE_DEVICES=0 SIRIUS_PROTO_SHADOW_OFF=1 pixi run $T "[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]"
run xfer_relay SIRIUS_PROTO_XFER_D2D=pipe pixi run $T "[xfer_relay],[mbid]"
echo UNITDONE >> $U/summary.txt
