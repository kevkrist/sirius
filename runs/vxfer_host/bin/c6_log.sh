#!/bin/bash
# engine log pass: <label> <q...>; arms A (tip binary) and U (proto auto), trace log, 2 iterations (run under flock)
B=/opt/dlami/nvme/dfperf/runs/vxfer_host/bin
label=$1; shift
for q in "$@"; do SIRIUS_LOG_LEVEL=trace $B/pair.sh $label $q 2 A1 U1; done
