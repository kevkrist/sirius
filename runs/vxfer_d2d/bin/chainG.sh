#!/bin/bash
# chainG.sh <name> <label> <iters> "<arms>" "<q q ...>" ["<q q ...>" ...] : one lock hold per GROUP of queries (keep a group <= ~5 min).
name=$1; label=$2; iters=$3; arms=$4; shift 4
R=/opt/dlami/nvme/dfperf; B=$R/runs/vxfer_d2d/bin
for group in "$@"; do
  t0=$(date +%T)
  flock $R/machine.lock bash -c "echo 'q[$group] LOCKED' \$(date +%T) queued $t0 >> $R/runs/vxfer_d2d/$name.log; for q in $group; do $B/pair.sh $label \$q $iters $arms; done; echo 'q[$group] END' \$(date +%T) >> $R/runs/vxfer_d2d/$name.log"
done
echo CHAIN_DONE $(date +%T) >> $R/runs/vxfer_d2d/$name.log
