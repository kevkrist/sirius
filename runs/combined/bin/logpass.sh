#!/bin/bash
# Usage: logpass.sh "<arms>" <q...> : DEBUG log, 3 iterations per process, label lp_<arm>; lock per query.
arms=$1; shift; B=/opt/dlami/nvme/dfperf/runs/combined/bin; R=/opt/dlami/nvme/dfperf
for q in "$@"; do
  flock $R/machine.lock bash -c "for a in $arms; do SIRIUS_LOG_LEVEL=debug $B/arm.sh \$a lp_\$a $q 3; done"
  echo "q$q done $(date +%T)" >> $R/runs/combined/lp.progress
done
echo ALLDONE >> $R/runs/combined/lp.progress
