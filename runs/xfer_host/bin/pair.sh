#!/bin/bash
# Usage: pair.sh <label> <q> <iterations> <arm> [<arm> ...]   -- run under flock (one hold = one query, all arms)
#   A* = tip ($R/repo); H* = proto, SIRIUS_PROTO_XFER_HOST=hostor; G* = proto gpuor; U* = proto auto; O* = proto binary, switch off
label=$1; q=$2; iters=$3; shift 3
R=/opt/dlami/nvme/dfperf; B=$R/runs/xfer_host/bin; P=$R/wt-xfer-host
for arm in "$@"; do
  case $arm in
    A*) $B/one.sh $label $arm $R/repo $q $iters;;
    H*) SIRIUS_PROTO_XFER_HOST=hostor $B/one.sh $label $arm $P $q $iters;;
    G*) SIRIUS_PROTO_XFER_HOST=gpuor $B/one.sh $label $arm $P $q $iters;;
    U*) SIRIUS_PROTO_XFER_HOST=auto $B/one.sh $label $arm $P $q $iters;;
    O*) $B/one.sh $label $arm $P $q $iters;;
  esac
done
