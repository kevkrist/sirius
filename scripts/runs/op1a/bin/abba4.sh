#!/bin/bash
# Usage: abba4.sh <label> <iterations> <q> [<q> ...]  -- per query ONE locked hold: A1 B1 B2 A2 (4 fresh processes)
label=$1; iters=$2; shift 2
R=/opt/dlami/nvme/dfperf; B=$R/runs/op1a/bin; L=$R/machine.lock
for q in "$@"; do
  flock $L $B/pair.sh $label $q $iters A1 B1 B2 A2
done
