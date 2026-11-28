#!/bin/bash
# logpass.sh <q...>: untimed debug-log pass (one lock hold per query), 3 iterations, modes off / pipe / compose in the spike binary.
X=/opt/dlami/nvme/dfperf/runs/xfer_compose; R=/opt/dlami/nvme/dfperf
for q in "$@"; do
  for mode in off pipe compose; do
    flock $R/machine.lock env SIRIUS_LOG_LEVEL=debug SIRIUS_PROTO_XFER_D2D=$mode REPO=$R/wt-xfer-compose $X/bin/one_query.sh log_$mode $q 3
  done
done
echo "logpass $* done $(date +%T)" >> $X/blocks.txt
