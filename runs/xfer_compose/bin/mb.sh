#!/bin/bash
# usage (under flock): mb.sh <label> <visible gpus> <catch filter> [VAR=VALUE ...]   (NSYS=1 -> nsys)
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp; mkdir -p $TMPDIR
label=$1; gpus=$2; filter=$3; shift 3
for kv in "$@"; do export "$kv"; done
export CUDA_VISIBLE_DEVICES=$gpus
T=/home/ubuntu/sirius/results/telemetry
marker=$(mktemp -p $TMPDIR xfercomp.marker.XXXX)
O=$R/runs/xfer_compose/mb
cd $R/wt-xfer-compose
BIN=build/release/test/cpp/sirius_unittest
if [ "${NSYS:-0}" = 1 ]; then
  pixi run nsys profile --trace=cuda,nvtx --sample=none --cudabacktrace=none --force-overwrite=true \
    --export=sqlite -o $R/runs/xfer_compose/nsys/$label $BIN "$filter" > $O/$label.log 2>&1
else
  pixi run $BIN "$filter" > $O/$label.log 2>&1
fi
rc=$?
if [ -d $T ]; then
  new=$(find $T -mindepth 1 -maxdepth 1 -type d -newer $marker)
  if [ -n "$new" ]; then mkdir -p $R/telemetry/xfer_compose_mb; mv $new $R/telemetry/xfer_compose_mb/; fi
fi
rm -f $marker
echo "rc=$rc label=$label $(date +%T)" | tee -a $O/status.txt
