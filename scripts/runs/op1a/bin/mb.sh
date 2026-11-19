#!/bin/bash
# usage: mb.sh <label> <visible gpus> <catch filter> [VAR=VALUE ...]   -- must run under flock $R/machine.lock
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp; mkdir -p $TMPDIR $R/runs/op1a/mb
label=$1; gpus=$2; filter=$3; shift 3
for kv in "$@"; do export "$kv"; done
export CUDA_VISIBLE_DEVICES=$gpus
T=/home/ubuntu/sirius/results/telemetry
before=$(mktemp -p $TMPDIR); ls -1 $T | sort > $before
cd $R/wt-op1a
pixi run build/release/test/cpp/sirius_unittest "$filter" > $R/runs/op1a/mb/$label.log 2>&1
rc=$?
mkdir -p $R/telemetry/op1a_mb
ls -1 $T | sort | comm -13 $before - | while read d; do mv $T/$d $R/telemetry/op1a_mb/; done
rm -f $before
echo "rc=$rc label=$label $(date +%T)" >> $R/runs/op1a/mb/status.txt
