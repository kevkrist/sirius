#!/bin/bash
# Usage: block.sh <label> <iterations> <q> [<q> ...]   -- one locked fresh process per query
label=$1; iters=$2; shift 2
B=/opt/dlami/nvme/dfperf/runs/base/bin
for q in "$@"; do flock /opt/dlami/nvme/dfperf/machine.lock $B/one_query.sh $label $q $iters; done
