#!/bin/bash
# c3_ut.sh <mode> <label> <filter>: existing unit tests of the prototype binary with the switch at <mode>
B=/opt/dlami/nvme/dfperf/runs/vxfer_d2d/bin
UT_TIMEOUT=${UT_TIMEOUT:-600} $B/ut_run.sh ut_$2_$1 0,1,2,3 "$3" SIRIUS_PROTO_XFER_D2D=$1
