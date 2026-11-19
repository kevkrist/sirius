#!/bin/bash
# nsys_block.sh <label> <iters> <q...>  env passes through (REPO, SIRIUS_PROTO_*)
label=$1; iters=$2; shift 2
for q in "$@"; do flock /opt/dlami/nvme/dfperf/machine.lock /opt/dlami/nvme/dfperf/runs/op4/bin/nsys_query.sh $label $q $iters; done
