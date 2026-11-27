#!/bin/bash
# Usage: plan.sh <planfile>  each line "<tag> <iters> <q> <arm:n> ..." = one hold of the machine lock
R=/opt/dlami/nvme/dfperf; B=$R/runs/veager/bin
while read -r tag iters q specs; do
  [ -z "$tag" ] && continue
  flock $R/machine.lock $B/unit.sh $tag $iters $q $specs >> $R/runs/veager/logs/${tag}_q$q.out 2>&1 < /dev/null
  echo "$tag q$q [$specs] done $(date +%T)"
done < "$1"
echo "PLAN_DONE $1 $(date +%T)"
