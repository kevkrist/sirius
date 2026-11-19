#!/bin/bash
# Usage: abba2.sh <label> <iterations> <q> [<q> ...]  -- per query two locked holds: (A1 B1) then (B2 A2)
label=$1; iters=$2; shift 2
R=/opt/dlami/nvme/dfperf; B=$R/runs/op1a/bin; L=$R/machine.lock
for q in "$@"; do
  flock $L $B/pair.sh $label $q $iters A1 B1
  flock $L $B/pair.sh $label $q $iters B2 A2
done
