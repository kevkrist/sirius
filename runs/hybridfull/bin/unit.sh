#!/bin/bash
# DF unit-test gate on wt-hybrid-full; each invocation takes the lock.
R=/opt/dlami/nvme/dfperf; U=$R/runs/hybridfull/unit; mkdir -p $U
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache TMPDIR=$R/tmp
cd $R/wt-hybrid-full; T=build/release/test/cpp/sirius_unittest
run() { name=$1; shift; flock $R/machine.lock env "$@" > $U/$name.log 2>&1; echo "$name rc=$? $(tail -3 $U/$name.log | grep -E 'passed|failed' | tr -d '\n')" >> $U/summary.txt; }
run sgpu_default CUDA_VISIBLE_DEVICES=0 pixi run $T "[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]"
run mgpu_default pixi run $T "[dynamic_filter][multi_gpu]"
run mgpu_ehx SIRIUS_PROTO_XFER_D2D=pipe SIRIUS_EAGER_PROBE=1 SIRIUS_PROTO_ONESHOT=1 SIRIUS_VERIFY_EAGER_ONESHOT=1 pixi run $T "[dynamic_filter][multi_gpu]"
run mgpu_ehxf SIRIUS_PROTO_XFER_D2D=pipe SIRIUS_EAGER_PROBE=1 SIRIUS_PROTO_ONESHOT=1 SIRIUS_VERIFY_EAGER_ONESHOT=1 SIRIUS_PROTO_ONESHOT_FASTREP=1 pixi run $T "[dynamic_filter][multi_gpu]"
run sgpu_oneshot CUDA_VISIBLE_DEVICES=0 SIRIUS_PROTO_ONESHOT=1 pixi run $T "[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]"
run xfer_relay SIRIUS_PROTO_XFER_D2D=pipe pixi run $T "[xfer_relay],[mbid]"
echo UNITDONE >> $U/summary.txt
