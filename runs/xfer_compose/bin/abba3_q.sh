#!/bin/bash
# Usage (under ONE flock): abba3_q.sh <tag> <iters> <q> [<q> ...]
# Per query 6 fresh processes: tip ($R/repo) / compose (CPU OR) / pipe (GPU OR) / pipe / compose / tip.
tag=$1; iters=$2; shift 2
B=/opt/dlami/nvme/dfperf/runs/xfer_compose/bin; R=/opt/dlami/nvme/dfperf
for q in "$@"; do
  env -u SIRIUS_PROTO_XFER_D2D REPO=$R/repo $B/one_query.sh ${tag}_tipA $q $iters
  SIRIUS_PROTO_XFER_D2D=compose REPO=$R/wt-xfer-compose $B/one_query.sh ${tag}_cpuA $q $iters
  SIRIUS_PROTO_XFER_D2D=pipe REPO=$R/wt-xfer-compose $B/one_query.sh ${tag}_gpuA $q $iters
  SIRIUS_PROTO_XFER_D2D=pipe REPO=$R/wt-xfer-compose $B/one_query.sh ${tag}_gpuB $q $iters
  SIRIUS_PROTO_XFER_D2D=compose REPO=$R/wt-xfer-compose $B/one_query.sh ${tag}_cpuB $q $iters
  env -u SIRIUS_PROTO_XFER_D2D REPO=$R/repo $B/one_query.sh ${tag}_tipB $q $iters
done
