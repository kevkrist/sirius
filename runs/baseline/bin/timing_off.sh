#!/bin/bash
# DF-off blocks (the SET is a TEST-ONLY option: needs SIRIUS_ENABLE_TEST_OPTIONS=1). Fresh locked process per query.
B=/opt/dlami/nvme/dfperf/runs/baseline/bin
L=/opt/dlami/nvme/dfperf/machine.lock
export SIRIUS_ENABLE_TEST_OPTIONS=1
export SIRIUS_PRE_SQL="SET enable_dynamic_filter = false;"
for q in "$@"; do
  flock $L $B/one_query.sh baseline_off $q 4
  flock $L $B/one_query.sh baseline_off_b $q 4
done
