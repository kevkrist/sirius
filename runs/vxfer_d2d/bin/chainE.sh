#!/bin/bash
# chainE.sh <name> <label> <iters> "<arms>" "<ENV=VAL ...>" "<q q ...>" [...groups] : like chainG.sh with extra environment (NSYS=1, SIRIUS_LOG_LEVEL=trace)
name=$1; label=$2; iters=$3; arms=$4; envs=$5; shift 5
R=/opt/dlami/nvme/dfperf; B=$R/runs/vxfer_d2d/bin
for group in "$@"; do
  t0=$(date +%T)
  flock $R/machine.lock bash -c "echo '$label q[$group] LOCKED' \$(date +%T) queued $t0 >> $R/runs/vxfer_d2d/$name.log; for q in $group; do env $envs $B/pair.sh $label \$q $iters $arms; done; echo '$label q[$group] END' \$(date +%T) >> $R/runs/vxfer_d2d/$name.log"
done
