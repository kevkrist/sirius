#!/bin/bash
# nsys_block.sh <label> <iters> q...   lock taken per query
label=$1; iters=$2; shift 2
for q in "$@"; do flock /opt/dlami/nvme/dfperf/machine.lock /opt/dlami/nvme/dfperf/runs/profile/bin/nsys_query.sh $label $q $iters; done
echo BLOCK_DONE
