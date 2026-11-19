#!/bin/bash
# TRACE log pass of the eager prototype (SIRIUS_EAGER_PROBE=1), 2 iterations per query, one lock hold per query.
R=/opt/dlami/nvme/dfperf; B=$R/runs/eager/bin
for q in "$@"; do
  SIRIUS_LOG_LEVEL=trace SIRIUS_EAGER_PROBE=1 REPO=$R/wt-eager flock $R/machine.lock $B/one_query.sh logpass $q 2 > /dev/null 2>&1 &
  sleep 0.3
done
wait
echo "LOGPASS_DONE $(date +%T)"
