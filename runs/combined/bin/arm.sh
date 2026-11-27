#!/bin/bash
# Usage (under the machine lock): arm.sh <arm> <label> <q> <iters>
# Arms: T = tip binary ($R/repo). Others = $R/wt-combined binary with env switches:
#   S shadow rule, X xfer-d2d (SIRIUS_PROTO_XFER_D2D=pipe), E eager probe (SIRIUS_EAGER_PROBE=1); O = all three off.
arm=$1; label=$2; q=$3; iters=$4
B=/opt/dlami/nvme/dfperf/runs/combined/bin; R=/opt/dlami/nvme/dfperf
unset SIRIUS_PROTO_XFER_D2D SIRIUS_EAGER_PROBE SIRIUS_PROTO_SHADOW_OFF SIRIUS_EAGER_CONCAT_FIRST
if [ $arm = T ]; then REPO=$R/repo exec $B/one_query.sh $label $q $iters; fi
case $arm in *S*) ;; *) export SIRIUS_PROTO_SHADOW_OFF=1;; esac
case $arm in *X*) export SIRIUS_PROTO_XFER_D2D=pipe;; esac
case $arm in *E*) export SIRIUS_EAGER_PROBE=1;; esac
REPO=$R/wt-combined exec $B/one_query.sh $label $q $iters
