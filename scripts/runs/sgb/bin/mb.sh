#!/bin/bash
# usage: run.sh <label> <visible gpus> <catch filter> [VAR=VALUE ...]   (NSYS=1 in env -> nsys profile)
# Must be invoked under: flock /opt/dlami/nvme/dfperf/machine.lock
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp; mkdir -p $TMPDIR $R/runs/sgb/mb $R/runs/sgb/mbnsys
label=$1; gpus=$2; filter=$3; shift 3
for kv in "$@"; do export "$kv"; done
export CUDA_VISIBLE_DEVICES=$gpus
T=/home/ubuntu/sirius/results/telemetry
marker=$(mktemp -p $TMPDIR sgbmb.marker.XXXX)
cd $R/wt-sgb
BIN=build/release/test/cpp/sirius_unittest
if [ "$NSYS" = 1 ]; then
  pixi run nsys profile --trace=cuda,nvtx --sample=none --cudabacktrace=none --force-overwrite=true \
    --export=sqlite -o $R/runs/sgb/mbnsys/$label $BIN "$filter" > $R/runs/sgb/mb/$label.log 2>&1
else
  pixi run $BIN "$filter" > $R/runs/sgb/mb/$label.log 2>&1
fi
rc=$?
if [ -d $T ]; then
  new=$(find $T -mindepth 1 -maxdepth 1 -type d -newer $marker)
  if [ -n "$new" ]; then mkdir -p $R/telemetry/sgb/mb; mv $new $R/telemetry/sgb/mb/; fi
fi
rm -f $marker
echo "rc=$rc label=$label"
