#!/bin/bash
# Queue one gated ABBA unit per query (each takes the machine lock for its own arms only) and wait for all of them.
R=/opt/dlami/nvme/dfperf; B=$R/runs/eager/bin
for q in "$@"; do
  case $q in
    3|16) arms="tip pro pros pros pro tip" ;;
    14|18|2) arms="tip pro pro2 pro2 pro tip" ;;
    *) arms="tip pro pro tip" ;;
  esac
  $B/gated.sh $B/unit.sh a1 6 $q $arms > $R/runs/eager/logs/a1_q$q.out 2>&1 &
  sleep 0.5
done
wait
echo "A1_DONE $(date +%T)"
