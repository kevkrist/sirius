#!/bin/bash
# Batch b1: option 1 vs scan throttle (K=1, K=2) vs option 2, anchored by tip and pro. One lock hold per query.
R=/opt/dlami/nvme/dfperf; B=$R/runs/eager/bin
for q in "$@"; do
  case $q in
    3) arms="tip t1 t2 o2 o1 pro pro o1 o2 t2 t1 tip" ;;
    *) arms="tip t1 t2 o2 pro pro o2 t2 t1 tip" ;;
  esac
  $B/gated2.sh "GO2 GO3" $B/unit.sh b1 6 $q $arms > $R/runs/eager/logs/b1_q$q.out 2>&1 &
  sleep 0.5
done
wait
echo "B1_DONE $(date +%T)"
