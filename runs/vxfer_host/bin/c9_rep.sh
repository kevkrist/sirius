#!/bin/bash
# engine race probe: <label> <iterations> <arm> <q...>; one process per query with debug log, many iterations (run under flock)
B=/opt/dlami/nvme/dfperf/runs/vxfer_host/bin
label=$1; iters=$2; arm=$3; shift 3
for q in "$@"; do SIRIUS_LOG_LEVEL=debug $B/pair.sh $label $q $iters $arm; done
