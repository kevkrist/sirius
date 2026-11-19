#!/bin/bash
# Usage: abba.sh <tag> <iterations> <q> [<q> ...]
# Per query: tip, proto, proto, tip; one locked fresh process each. Labels <tag>_tip1 <tag>_pro1 <tag>_pro2 <tag>_tip2.
# PROTO_ENV (optional) = env assignments for the prototype arm, e.g. "SIRIUS_DF_DEFER_PUBLISH=2".
tag=$1; iters=$2; shift 2
B=/opt/dlami/nvme/dfperf/runs/op1b/bin; R=/opt/dlami/nvme/dfperf; L=$R/machine.lock
for q in "$@"; do
  REPO=$R/repo flock $L $B/one_query.sh ${tag}_tip1 $q $iters
  env ${PROTO_ENV:-} REPO=$R/wt-op1b flock $L $B/one_query.sh ${tag}_pro1 $q $iters
  env ${PROTO_ENV:-} REPO=$R/wt-op1b flock $L $B/one_query.sh ${tag}_pro2 $q $iters
  REPO=$R/repo flock $L $B/one_query.sh ${tag}_tip2 $q $iters
done
