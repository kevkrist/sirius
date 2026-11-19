#!/bin/bash
# Query-interleaved timing: per query, fresh locked process each: tip A, DF-off A, tip B, DF-off B. 1 warm + 3 hot.
B=/opt/dlami/nvme/dfperf/runs/baseline/bin
L=/opt/dlami/nvme/dfperf/machine.lock
for q in "$@"; do
  flock $L $B/one_query.sh baseline_tip $q 4
  SIRIUS_PRE_SQL="SET enable_dynamic_filter = false;" flock $L $B/one_query.sh baseline_dfoff $q 4
  flock $L $B/one_query.sh baseline_tip_b $q 4
  SIRIUS_PRE_SQL="SET enable_dynamic_filter = false;" flock $L $B/one_query.sh baseline_dfoff_b $q 4
done
