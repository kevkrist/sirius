#!/bin/bash
# c2_vx.sh <label> <visible gpus> <tag> [VAR=VALUE...]: verifier tests (out-of-tree binary linked against the prototype's objects)
B=/opt/dlami/nvme/dfperf/runs/vxfer_d2d/bin
label=$1; gpus=$2; tag=$3; shift 3
UTBIN=/opt/dlami/nvme/dfperf/runs/vxfer_d2d/vxbin/sirius_unittest_vx UT_TIMEOUT=${UT_TIMEOUT:-420} $B/ut_run.sh $label $gpus "[$tag]" "$@"
