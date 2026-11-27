#!/bin/bash
# Usage: abba.sh <tag> <iters> "<arms>" <q> [<q> ...]   (takes the machine lock per half block)
# Per query: forward pass over the arms (labels <tag>_<arm>_a), then reverse pass (<tag>_<arm>_b).
tag=$1; iters=$2; arms=$3; shift 3
B=/opt/dlami/nvme/dfperf/runs/combined/bin; R=/opt/dlami/nvme/dfperf
rev=$(echo $arms | tr ' ' '\n' | tac | tr '\n' ' ')
for q in "$@"; do
  flock $R/machine.lock bash -c "for a in $arms; do $B/arm.sh \$a ${tag}_\${a}_a $q $iters; done"
  flock $R/machine.lock bash -c "for a in $rev; do $B/arm.sh \$a ${tag}_\${a}_b $q $iters; done"
  echo "q$q done $(date +%T)" >> $R/runs/combined/$tag.progress
done
echo ALLDONE >> $R/runs/combined/$tag.progress
