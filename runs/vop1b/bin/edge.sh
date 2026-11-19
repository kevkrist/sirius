#!/bin/bash
# Usage: flock $R/machine.lock edge.sh <label> <q> <arm> [<arm> ...]   tip* = $R/repo, pro* = wt-op1b mode 1, pro0* = wt-op1b mode 0
set -u
label=$1; q=$2; shift 2
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
export SIRIUS_CONFIG_FILE=/home/ubuntu/sirius/specific_config.yaml
export EDGE_Q=$q
TEL=/home/ubuntu/sirius/results/telemetry
for arm in "$@"; do
  case $arm in
    tip*)  tree=$R/repo; unset SIRIUS_DF_DEFER_PUBLISH;;
    pro0*) tree=$R/wt-op1b; export SIRIUS_DF_DEFER_PUBLISH=0;;
    *)     tree=$R/wt-op1b; export SIRIUS_DF_DEFER_PUBLISH=1;;
  esac
  out=$R/runs/vop1b/$label/q${q}_$arm; mkdir -p $out
  before=$(mktemp); ls -1 $TEL | sort > $before
  cd $tree
  timeout 600 pixi run python $R/runs/vop1b/bin/edge.py $tree $out > $out/edge.stdout 2>&1
  rc=$?
  mkdir -p $R/telemetry/vop1b_$label/q${q}_$arm
  ls -1 $TEL | sort | comm -13 $before - | while read d; do mv $TEL/$d $R/telemetry/vop1b_$label/q${q}_$arm/; done
  rm -f $before
  echo "label=$label q=$q arm=$arm rc=$rc $(date +%T)" >> $R/runs/vop1b/$label/status.txt
done
