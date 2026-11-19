#!/bin/bash
# Usage: logpass.sh <label> <q> [<q> ...]  -- prototype (mode 1) TRACE pass, 2 iterations per query; run INSIDE one lock hold.
label=$1; shift
B=/opt/dlami/nvme/dfperf/runs/vop1b/bin; R=/opt/dlami/nvme/dfperf
for q in "$@"; do
  REPO=$R/wt-op1b SIRIUS_DF_DEFER_PUBLISH=1 SIRIUS_LOG_LEVEL=trace $B/one_query.sh $label $q 2
done
