#!/bin/bash
# Batch c1: remaining queries with DF joins, tip vs option 2 vs throttle K=2 (completes the suite for those arms with b1).
R=/opt/dlami/nvme/dfperf; B=$R/runs/eager/bin
for q in "$@"; do
  flock $R/machine.lock $B/unit.sh c1 6 $q tip o2 t2 t2 o2 tip > $R/runs/eager/logs/c1_q$q.out 2>&1 &
  sleep 0.5
done
wait
echo "C1_DONE $(date +%T)"
