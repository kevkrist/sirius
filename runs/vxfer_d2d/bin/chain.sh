#!/bin/bash
# chain.sh <name> <label> <iters> "<arms>" <q...> : one lock hold per query; logs START/END per hold. NOT under flock itself.
name=$1; label=$2; iters=$3; arms=$4; shift 4
R=/opt/dlami/nvme/dfperf; B=$R/runs/vxfer_d2d/bin
for q in "$@"; do
  t0=$(date +%T)
  flock $R/machine.lock bash -c "echo q$q LOCKED \$(date +%T) queued $t0 >> $R/runs/vxfer_d2d/$name.log; $B/pair.sh $label $q $iters $arms; echo q$q END \$(date +%T) >> $R/runs/vxfer_d2d/$name.log"
done
echo CHAIN_DONE $(date +%T) >> $R/runs/vxfer_d2d/$name.log
