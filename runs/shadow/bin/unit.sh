#!/bin/bash
# Usage: unit.sh <worktree-name> <single|mgpu|tags> [catch2 filter]   -- DF unit tests of one worktree.
# Run under: flock /opt/dlami/nvme/dfperf/machine.lock unit.sh ...
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
wt=$1; mode=$2; filter=${3:-}
out=$R/runs/shadow/unit/$wt; mkdir -p $out
cd $R/$wt
case $mode in
  single) CUDA_VISIBLE_DEVICES=0 pixi run build/release/test/cpp/sirius_unittest "[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]" > $out/single.txt 2>&1 ;;
  mgpu)   pixi run build/release/test/cpp/sirius_unittest "[dynamic_filter][multi_gpu]" > $out/mgpu.txt 2>&1 ;;
  tags)   CUDA_VISIBLE_DEVICES=0 pixi run build/release/test/cpp/sirius_unittest "$filter" > $out/tags.txt 2>&1 ;;
esac
rc=$?
f=$out/$mode.txt
echo "$wt $mode rc=$rc $(date +%T) $(git rev-parse --short HEAD) $(tail -3 $f | tr '\n' ' ')" >> $R/runs/shadow/unit/status.txt
exit $rc
