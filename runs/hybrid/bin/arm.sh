#!/bin/bash
# Usage (under the machine lock): arm.sh <arm> <label> <q> <iters>
# Arms (all on the $R/wt-hybrid binary unless TIP): T = switches off (tip behaviour); H = SIRIUS_PROTO_ONESHOT=1
# (one-shot, publish then deposit); B = SIRIUS_PROTO_ONESHOT=2 (one-shot, deposit first = pre-stack order);
# E = SIRIUS_EAGER_PROBE=1; EH = eager + one-shot(1); TIP = $R/repo binary.
arm=$1; label=$2; q=$3; iters=$4
B=/opt/dlami/nvme/dfperf/runs/hybrid/bin; R=/opt/dlami/nvme/dfperf
unset SIRIUS_PROTO_ONESHOT SIRIUS_EAGER_PROBE SIRIUS_EAGER_CONCAT_FIRST SIRIUS_PROTO_XFER_D2D SIRIUS_PROTO_SHADOW_OFF
case $arm in
  TIP) REPO=$R/repo exec $B/one_query.sh $label $q $iters;;
  T) ;;
  H) export SIRIUS_PROTO_ONESHOT=1;;
  B) export SIRIUS_PROTO_ONESHOT=2;;
  E) export SIRIUS_EAGER_PROBE=1;;
  EH) export SIRIUS_EAGER_PROBE=1 SIRIUS_PROTO_ONESHOT=1;;
  *) echo bad arm; exit 2;;
esac
REPO=$R/wt-hybrid exec $B/one_query.sh $label $q $iters
