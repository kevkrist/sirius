#!/bin/bash
# Usage: dbg.sh <tag> <iters> "<arms>" <q> ...   DEBUG-level log pass (untimed), one lock hold per query.
tag=$1; iters=$2; arms=$3; shift 3
B=/opt/dlami/nvme/dfperf/runs/hybrid/bin; R=/opt/dlami/nvme/dfperf
for q in "$@"; do
  flock $R/machine.lock bash -c "for a in $arms; do SIRIUS_LOG_LEVEL=debug $B/arm.sh \$a ${tag}_\${a} $q $iters; done"
  echo "q$q done $(date +%T)" >> $R/runs/hybrid/$tag.progress
done
echo ALLDONE >> $R/runs/hybrid/$tag.progress
