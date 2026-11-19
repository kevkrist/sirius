#!/bin/bash
# Usage: batch2.sh <tag> <iters> <chunk> "<q q ...>" <arm> [<arm> ...]
# For each query: the arms once in the given order, then once mirrored (ABBA). The process list is cut into
# chunks of <chunk> processes; each chunk is one hold of the machine lock (short holds, others get turns).
tag=$1; iters=$2; chunk=$3; qs=$4; shift 4
R=/opt/dlami/nvme/dfperf; B=$R/runs/eager/bin; mkdir -p $R/runs/eager/logs
arms=("$@"); specs=()
for a in "${arms[@]}"; do specs+=("$a:1"); done
for ((i=${#arms[@]}-1; i>=0; i--)); do specs+=("${arms[$i]}:2"); done
for q in $qs; do
  for ((i=0; i<${#specs[@]}; i+=chunk)); do
    flock $R/machine.lock $B/unit2.sh $tag $iters $q "${specs[@]:i:chunk}" >> $R/runs/eager/logs/${tag}_q$q.out 2>&1
  done
  echo "q$q done $(date +%T)"
done
echo "BATCH_DONE $tag $(date +%T)"
