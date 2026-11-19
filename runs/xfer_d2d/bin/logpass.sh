#!/bin/bash
# logpass.sh <q...>: untimed debug-log pass of the prototype (MODE, default pipe), 2 iterations: which route each finish took.
X=/opt/dlami/nvme/dfperf/runs/xfer_d2d; R=/opt/dlami/nvme/dfperf
for q in "$@"; do
  SIRIUS_LOG_LEVEL=debug SIRIUS_PROTO_XFER_D2D=${MODE:-pipe} REPO=$R/wt-xfer-d2d $X/bin/one_query.sh log_${MODE:-pipe} $q 2
done
