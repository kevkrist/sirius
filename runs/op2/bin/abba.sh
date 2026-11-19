#!/bin/bash
# Usage: abba.sh <tag> <iters> <q> [<q> ...]  (env: PROTO_ENV="VAR=val ..." applied to the prototype runs only)
# Per query: tip A, proto A, proto B, tip B; each a fresh locked process. Labels: <tag>_tipA/_protoA/_protoB/_tipB
tag=$1; iters=$2; shift 2
B=/opt/dlami/nvme/dfperf/runs/op2/bin; L=/opt/dlami/nvme/dfperf/machine.lock; R=/opt/dlami/nvme/dfperf
for q in "$@"; do
  REPO=$R/repo   flock $L $B/one_query.sh ${tag}_tipA $q $iters
  env ${PROTO_ENV:-} REPO=$R/wt-op2 flock $L $B/one_query.sh ${tag}_protoA $q $iters
  env ${PROTO_ENV:-} REPO=$R/wt-op2 flock $L $B/one_query.sh ${tag}_protoB $q $iters
  REPO=$R/repo   flock $L $B/one_query.sh ${tag}_tipB $q $iters
done
