#!/bin/bash
# Usage: flock $R/machine.lock edge.sh <label> <arm> [<arm> ...]   A* = tip, S* = proto sync, B* = proto async
set -u
label=$1; shift
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
export SIRIUS_CONFIG_FILE=/home/ubuntu/sirius/specific_config.yaml
TEL=/home/ubuntu/sirius/results/telemetry
for arm in "$@"; do
  case $arm in
    A*) tree=$R/repo; unset SIRIUS_DF_ASYNC_PUBLISH;;
    S*) tree=$R/wt-op1a; export SIRIUS_DF_ASYNC_PUBLISH=0;;
    *)  tree=$R/wt-op1a; unset SIRIUS_DF_ASYNC_PUBLISH;;
  esac
  out=$R/runs/vop1a/$label/$arm; mkdir -p $out
  before=$(mktemp); ls -1 $TEL | sort > $before
  cd $tree
  timeout 600 pixi run python $R/runs/vop1a/bin/edge.py $tree $out > $out/edge.stdout 2>&1
  rc=$?
  mkdir -p $R/telemetry/vop1a_$label/$arm
  ls -1 $TEL | sort | comm -13 $before - | while read d; do mv $TEL/$d $R/telemetry/vop1a_$label/$arm/; done
  rm -f $before
  echo "label=$label arm=$arm rc=$rc $(date +%T)" >> $R/runs/vop1a/$label/status.txt
done
