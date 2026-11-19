#!/bin/bash
# Usage: gated.sh <command...>   Queue on the machine lock; run the command only if runs/eager/GO exists. If the lock is
# granted before GO exists, release at once and re-queue (never holds the lock while waiting). Exits if NOGO appears.
R=/opt/dlami/nvme/dfperf
for try in $(seq 1 400); do
  [ -f $R/runs/eager/NOGO ] && { echo "NOGO"; exit 3; }
  flock $R/machine.lock bash -c "[ -f $R/runs/eager/GO ] || exit 77; $*"; rc=$?
  [ $rc -ne 77 ] && exit $rc
  sleep 30
done
