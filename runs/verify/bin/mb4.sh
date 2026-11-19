#!/bin/bash
# usage: mb4.sh <label> <visible gpus> <catch filter> [VAR=VALUE ...]  -- copy of mblife/run.sh with output in runs/verify/mb
# Must be invoked under: flock /opt/dlami/nvme/dfperf/machine.lock
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp; mkdir -p $TMPDIR $R/runs/verify/mb
label=$1; gpus=$2; filter=$3; shift 3
for kv in "$@"; do export "$kv"; done
export CUDA_VISIBLE_DEVICES=$gpus
T=/home/ubuntu/sirius/results/telemetry
marker=$(mktemp -p $TMPDIR verify.marker.XXXX)
cd $R/wt-mb-life
pixi run build/release/test/cpp/sirius_unittest "$filter" > $R/runs/verify/mb/$label.log 2>&1
rc=$?
new=$(find $T -mindepth 1 -maxdepth 1 -type d -newer $marker)
if [ -n "$new" ]; then mkdir -p $R/telemetry/verify_mb; mv $new $R/telemetry/verify_mb/; fi
rm -f $marker
echo "rc=$rc label=$label"
