#!/bin/bash
# Usage: gated2.sh "<flag files, space separated>" <command...>   Like gated.sh, but waits for every listed flag file in
# runs/eager (e.g. "GO2 GO3"); gives up when a matching NO<flag> appears. Never holds the lock while waiting.
R=/opt/dlami/nvme/dfperf; flags=$1; shift
for try in $(seq 1 600); do
  for f in $flags; do [ -f $R/runs/eager/NO$f ] && { echo "NO$f"; exit 3; }; done
  ok=1; for f in $flags; do [ -f $R/runs/eager/$f ] || ok=0; done
  if [ $ok = 1 ]; then flock $R/machine.lock "$@"; exit $?; fi
  sleep 20
done
