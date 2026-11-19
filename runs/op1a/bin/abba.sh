#!/bin/bash
# Usage: abba.sh <label> <iterations> <q> [<q> ...]  -- per query: tip, proto, proto, tip; one locked fresh process each
label=$1; iters=$2; shift 2
R=/opt/dlami/nvme/dfperf; B=$R/runs/op1a/bin; L=$R/machine.lock
TIP=$R/repo; PROTO=${PROTO:-$R/wt-op1a}
for q in "$@"; do
  flock $L $B/one.sh $label A1 $TIP $q $iters
  flock $L $B/one.sh $label B1 $PROTO $q $iters
  flock $L $B/one.sh $label B2 $PROTO $q $iters
  flock $L $B/one.sh $label A2 $TIP $q $iters
done
