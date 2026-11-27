#!/bin/bash
# Usage (under the machine lock): arm.sh <arm> <label> <q> <iters>
# T,H,E,EH = author's binary ($R/wt-hybrid) with the author's switches.
# X  = $R/wt-vhybrid, eager + one-shot + SIRIUS_VERIFY_EAGER_ONESHOT=1 (probe released right after one-shot publication).
# EHV = $R/wt-vhybrid, eager + one-shot, verify switch off (binary-equivalence control for EH).
arm=$1; label=$2; q=$3; iters=$4
B=/opt/dlami/nvme/dfperf/runs/vhybrid/bin; R=/opt/dlami/nvme/dfperf
unset SIRIUS_PROTO_ONESHOT SIRIUS_EAGER_PROBE SIRIUS_EAGER_CONCAT_FIRST SIRIUS_PROTO_XFER_D2D SIRIUS_PROTO_SHADOW_OFF SIRIUS_VERIFY_EAGER_ONESHOT SIRIUS_LOG_LEVEL
REPO=$R/wt-hybrid
case $arm in
  T) ;;
  H) export SIRIUS_PROTO_ONESHOT=1;;
  E) export SIRIUS_EAGER_PROBE=1;;
  EH) export SIRIUS_EAGER_PROBE=1 SIRIUS_PROTO_ONESHOT=1;;
  X) export SIRIUS_EAGER_PROBE=1 SIRIUS_PROTO_ONESHOT=1 SIRIUS_VERIFY_EAGER_ONESHOT=1; REPO=$R/wt-vhybrid;;
  EHV) export SIRIUS_EAGER_PROBE=1 SIRIUS_PROTO_ONESHOT=1; REPO=$R/wt-vhybrid;;
  *) echo bad arm; exit 2;;
esac
REPO=$REPO exec $B/one_query.sh $label $q $iters
