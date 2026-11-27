#!/bin/bash
# abba.sh "<arms>" <q...> : per query forward pass (half a) then reverse pass (half b); lock per pass; 1 warm + 5 hot.
arms=$1; shift
R=/opt/dlami/nvme/dfperf; B=$R/runs/vcombined/bin
rev=$(echo $arms | tr ' ' '\n' | tac | tr '\n' ' ')
for q in "$@"; do
  flock $R/machine.lock bash -c "for a in $arms; do $B/one.sh \$a a $q 6; done"
  flock $R/machine.lock bash -c "for a in $rev; do $B/one.sh \$a b $q 6; done"
  echo "q$q arms=[$arms] done $(date +%T)" >> $R/runs/vcombined/progress.txt
done
