#!/bin/bash
# Usage: pair.sh <label> <q> <iterations> <arm> [<arm> ...]
#   arms: A* = tip ($R/repo); B* = proto; E* = proto with SIRIUS_DF_ASYNC_EARLY=1; S* = proto with SIRIUS_DF_ASYNC_PUBLISH=0
# MUST be run under flock $R/machine.lock: runs the arms back to back, one fresh process each.
label=$1; q=$2; iters=$3; shift 3
R=/opt/dlami/nvme/dfperf; B=$R/runs/op1a/bin
for arm in "$@"; do
  case $arm in
    A*) $B/one.sh $label $arm $R/repo $q $iters;;
    E*) SIRIUS_DF_ASYNC_EARLY=1 $B/one.sh $label $arm ${PROTO:-$R/wt-op1a} $q $iters;;
    S*) SIRIUS_DF_ASYNC_PUBLISH=0 $B/one.sh $label $arm ${PROTO:-$R/wt-op1a} $q $iters;;
    *)  $B/one.sh $label $arm ${PROTO:-$R/wt-op1a} $q $iters;;
  esac
done
