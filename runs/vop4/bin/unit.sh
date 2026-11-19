#!/bin/bash
# unit.sh <name> <tags>  (under flock)  env SIRIUS_PROTO_* passes through; all 4 GPUs visible
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache TMPDIR=$R/tmp
export SIRIUS_LOG_DIR=$R/runs/vop4/unit_logs/$1; mkdir -p $SIRIUS_LOG_DIR
cd ${WT:-$R/wt-op4}
pixi run build/release/test/cpp/sirius_unittest "$2" > $R/runs/vop4/unit_$1.out 2>&1
echo "rc=$? $(date +%T)" >> $R/runs/vop4/unit_$1.out
