#!/bin/bash
# abba.sh <tag> <iters> <q> <order>   (run the WHOLE script under flock; one lock hold = one adjacent block)
# order e.g. "A B B A" or "B A A B". A = tip binary, no env. B = B_REPO (default wt-op4) with B_ENV ("K=V K=V").
# labels: <tag>_<side><n>, n = running index of that side in this call + OFFSET
tag=$1; iters=$2; q=$3; order=${4:-A B B A}; off=${OFFSET:-0}
D=/opt/dlami/nvme/dfperf/runs/vop4/bin; R=/opt/dlami/nvme/dfperf
na=$off; nb=$off
for s in $order; do
  if [ $s = A ]; then na=$((na+1)); env -u SIRIUS_PROTO_BLOOM_RANGE -u SIRIUS_PROTO_BLOOM_BPK REPO=$R/repo $D/one_query.sh ${tag}_A$na $q $iters
  else nb=$((nb+1)); env ${B_ENV:-} REPO=${B_REPO:-$R/wt-op4} $D/one_query.sh ${tag}_B$nb $q $iters; fi
done
