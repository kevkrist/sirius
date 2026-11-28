#!/bin/bash
# abba3_block.sh <tag> <iters> <q...>: one lock hold per query.
tag=$1; iters=$2; shift 2
B=/opt/dlami/nvme/dfperf/runs/xfer_compose/bin
for q in "$@"; do flock /opt/dlami/nvme/dfperf/machine.lock $B/abba3_q.sh $tag $iters $q; done
echo "block $tag $* done $(date +%T)" >> /opt/dlami/nvme/dfperf/runs/xfer_compose/blocks.txt
