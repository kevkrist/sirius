#!/bin/bash
# Usage: pair.sh <label> <q> <iterations> <arm> [<arm> ...]   -- MUST run under flock $R/machine.lock (one hold = one query)
#   A* = tip ($R/repo); B* = proto async (default); S* = proto with SIRIUS_DF_ASYNC_PUBLISH=0
#   env G2DEV=0,1 : run every arm on that GPU pair (CUDA_VISIBLE_DEVICES + SIRIUS_PROTO_NUM_GPUS=2; proto binary only)
label=$1; q=$2; iters=$3; shift 3
R=/opt/dlami/nvme/dfperf; B=$R/runs/vop1a/bin; P=$R/wt-op1a
if [ -n "${G2DEV:-}" ]; then export CUDA_VISIBLE_DEVICES=$G2DEV SIRIUS_PROTO_NUM_GPUS=2; fi
for arm in "$@"; do
  case $arm in
    A*) $B/one.sh $label $arm $R/repo $q $iters;;
    S*) SIRIUS_DF_ASYNC_PUBLISH=0 $B/one.sh $label $arm $P $q $iters;;
    *)  $B/one.sh $label $arm $P $q $iters;;
  esac
done
