#!/bin/bash
# Usage: pair.sh <label> <q> <iterations> <arm> [<arm> ...]   -- run under flock (one hold = one query, all arms)
#   A* = tip binary ($R/repo); P* = proto, SIRIUS_PROTO_XFER_D2D=pipe; S* = star; E* = exchange; O* = proto binary, switch unset
label=$1; q=$2; iters=$3; shift 3
R=/opt/dlami/nvme/dfperf; B=$R/runs/vxfer_d2d/bin; P=$R/wt-xfer-d2d
for arm in "$@"; do
  case $arm in
    A*) env -u SIRIUS_PROTO_XFER_D2D $B/one.sh $label $arm $R/repo $q $iters;;
    P*) SIRIUS_PROTO_XFER_D2D=pipe $B/one.sh $label $arm $P $q $iters;;
    S*) SIRIUS_PROTO_XFER_D2D=star $B/one.sh $label $arm $P $q $iters;;
    E*) SIRIUS_PROTO_XFER_D2D=exchange $B/one.sh $label $arm $P $q $iters;;
    O*) env -u SIRIUS_PROTO_XFER_D2D $B/one.sh $label $arm $P $q $iters;;
  esac
done
