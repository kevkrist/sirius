#!/bin/bash
# Usage: abba_block.sh <label> "<arm> <arm> ..." <q> [<q> ...]   -- run under flock: every arm of every listed query in one hold
label=$1; arms=$2; shift 2
for q in "$@"; do /opt/dlami/nvme/dfperf/runs/xfer_host/bin/pair.sh $label $q 6 $arms; done
