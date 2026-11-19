#!/bin/bash
# Base timing: per query, two fresh locked processes (A, B), each 1 warm + 3 hot (iterations=4), info log level.
B=/opt/dlami/nvme/dfperf/runs/base/bin
L=/opt/dlami/nvme/dfperf/machine.lock
for q in "$@"; do
  flock $L $B/one_query.sh base_a $q 4
  flock $L $B/one_query.sh base_b $q 4
done
