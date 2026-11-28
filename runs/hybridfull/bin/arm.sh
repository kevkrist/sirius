#!/bin/bash
# Usage (under the machine lock): arm.sh <arm> <label> <q> <iters>
# One binary ($R/wt-hybrid-full, proto/hybrid-full), env switches. Shadow rule ON in every arm except O/T.
#   T   = tip binary $R/repo (ae939824, pre-shadow)      O = this binary, shadow off, all else off (= tip plan)
#   S   = shadow only (= current PR heads)               E = S + eager
#   EH  = S + eager + one-shot (release after one-shot publication)
#   EX  = S + eager + transfer(pipe)                     EHX = EH + transfer(pipe)
#   X = S + transfer(pipe), eager off   H = S + one-shot   HX = H + transfer   HXF = HX + fast one-shot replication
#   EHXF = EHX + pipelined host hop in the one-shot replica fan-out (SIRIUS_PROTO_ONESHOT_FASTREP=1)
arm=$1; label=$2; q=$3; iters=$4
B=/opt/dlami/nvme/dfperf/runs/hybridfull/bin; R=/opt/dlami/nvme/dfperf
unset SIRIUS_PROTO_ONESHOT SIRIUS_EAGER_PROBE SIRIUS_EAGER_CONCAT_FIRST SIRIUS_PROTO_XFER_D2D SIRIUS_PROTO_SHADOW_OFF SIRIUS_VERIFY_EAGER_ONESHOT SIRIUS_PROTO_ONESHOT_FASTREP
REPO=$R/wt-hybrid-full
case $arm in
  T) REPO=$R/repo;;
  O) export SIRIUS_PROTO_SHADOW_OFF=1;;
  S) ;;
  X) export SIRIUS_PROTO_XFER_D2D=pipe;;
  H) export SIRIUS_PROTO_ONESHOT=1;;
  HX) export SIRIUS_PROTO_ONESHOT=1 SIRIUS_PROTO_XFER_D2D=pipe;;
  HXF) export SIRIUS_PROTO_ONESHOT=1 SIRIUS_PROTO_XFER_D2D=pipe SIRIUS_PROTO_ONESHOT_FASTREP=1;;
  E) export SIRIUS_EAGER_PROBE=1;;
  EH) export SIRIUS_EAGER_PROBE=1 SIRIUS_PROTO_ONESHOT=1 SIRIUS_VERIFY_EAGER_ONESHOT=1;;
  EX) export SIRIUS_EAGER_PROBE=1 SIRIUS_PROTO_XFER_D2D=pipe;;
  EHX) export SIRIUS_EAGER_PROBE=1 SIRIUS_PROTO_ONESHOT=1 SIRIUS_VERIFY_EAGER_ONESHOT=1 SIRIUS_PROTO_XFER_D2D=pipe;;
  EHXF) export SIRIUS_EAGER_PROBE=1 SIRIUS_PROTO_ONESHOT=1 SIRIUS_VERIFY_EAGER_ONESHOT=1 SIRIUS_PROTO_XFER_D2D=pipe SIRIUS_PROTO_ONESHOT_FASTREP=1;;
  *) echo bad arm; exit 2;;
esac
REPO=$REPO exec $B/one_query.sh $label $q $iters
