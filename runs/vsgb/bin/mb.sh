#!/bin/bash
# Usage: mb.sh <label> <visible gpus> <catch filter> [VAR=VALUE ...]  -- run INSIDE a lock hold. MB8 from the prototype binary.
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp; mkdir -p $TMPDIR $R/runs/vsgb/mb
label=$1; gpus=$2; filter=$3; shift 3
for kv in "$@"; do export "$kv"; done
export CUDA_VISIBLE_DEVICES=$gpus
TEL=/home/ubuntu/sirius/results/telemetry
before=$(mktemp -p $TMPDIR vsgb.before.XXXX); ls -1 $TEL | sort > $before
cd $R/wt-sgb
pixi run build/release/test/cpp/sirius_unittest "$filter" > $R/runs/vsgb/mb/$label.log 2>&1
echo "rc=$? label=$label gpus=$gpus $(date +%T)" | tee -a $R/runs/vsgb/mb/$label.log
mkdir -p $R/telemetry/vsgb/mb
ls -1 $TEL | sort | comm -13 $before - | while read d; do mv $TEL/$d $R/telemetry/vsgb/mb/; done
rm -f $before
