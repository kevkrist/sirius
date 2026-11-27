#!/bin/bash
# TRACE log pass of the eager arms (2 iterations: cold + hot), same as runs/baseline/logpass for the tip
export SIRIUS_LOG_LEVEL=trace
exec /opt/dlami/nvme/dfperf/runs/veager/bin/plan.sh /opt/dlami/nvme/dfperf/runs/veager/plans/lp.plan
