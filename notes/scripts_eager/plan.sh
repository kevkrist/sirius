#!/bin/bash
# Usage: plan.sh <planfile>   Each non-empty line "<tag> <iters> <q> <arm:n> ..." is one hold of the machine lock (unit2.sh).
R=/opt/dlami/nvme/dfperf; B=$R/runs/eager/bin; mkdir -p $R/runs/eager/logs
while read -r tag iters q specs; do
  [ -z "$tag" ] && continue
  flock $R/machine.lock $B/unit2.sh $tag $iters $q $specs >> $R/runs/eager/logs/${tag}_q$q.out 2>&1 < /dev/null
  echo "$tag q$q [$specs] done $(date +%T)"
done < "$1"
echo "PLAN_DONE $1 $(date +%T)"
