#!/bin/bash
# Usage: abba.sh <tag> <iters> "<arms>" <q> [<q> ...]   (takes the machine lock per half block)
tag=$1; iters=$2; arms=$3; shift 3
B=/opt/dlami/nvme/dfperf/runs/hybrid/bin; R=/opt/dlami/nvme/dfperf
rev=$(echo $arms | tr ' ' '\n' | tac | tr '\n' ' ')
for q in "$@"; do
  flock $R/machine.lock bash -c "for a in $arms; do $B/arm.sh \$a ${tag}_\${a}_a $q $iters; done"
  flock $R/machine.lock bash -c "for a in $rev; do $B/arm.sh \$a ${tag}_\${a}_b $q $iters; done"
  echo "q$q done $(date +%T)" >> $R/runs/hybrid/$tag.progress
done
echo ALLDONE >> $R/runs/hybrid/$tag.progress
