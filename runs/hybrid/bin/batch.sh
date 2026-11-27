#!/bin/bash
# Usage: batch.sh <tag> <iters> "<arms>" <mode: abba|dbg> <queries per lock hold> <q> ...
# abba: per chunk of queries one hold with the arms forward (labels <tag>_<arm>_a), one hold reversed (<tag>_<arm>_b).
# dbg: one hold per chunk, SIRIUS_LOG_LEVEL=debug, labels <tag>_<arm>.
tag=$1; iters=$2; arms=$3; mode=$4; per=$5; shift 5
B=/opt/dlami/nvme/dfperf/runs/hybrid/bin; R=/opt/dlami/nvme/dfperf
rev=$(echo $arms | tr ' ' '\n' | tac | tr '\n' ' ')
while [ $# -gt 0 ]; do
  chunk="${@:1:$per}"; shift $(( $# < per ? $# : per ))
  if [ $mode = dbg ]; then
    flock $R/machine.lock bash -c "for q in $chunk; do for a in $arms; do SIRIUS_LOG_LEVEL=debug $B/arm.sh \$a ${tag}_\${a} \$q $iters; done; done"
  else
    flock $R/machine.lock bash -c "for q in $chunk; do for a in $arms; do $B/arm.sh \$a ${tag}_\${a}_a \$q $iters; done; done"
    flock $R/machine.lock bash -c "for q in $chunk; do for a in $rev; do $B/arm.sh \$a ${tag}_\${a}_b \$q $iters; done; done"
  fi
  echo "$chunk done $(date +%T)" >> $R/runs/hybrid/$tag.progress
done
echo ALLDONE >> $R/runs/hybrid/$tag.progress
