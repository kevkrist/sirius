#!/bin/bash
# Usage (under ONE flock): abba_q.sh <tag> <iters> <q> [<q> ...]
# Per query, 4 fresh processes: tip (binary $R/repo) A, proto (wt-xfer-d2d, SIRIUS_PROTO_XFER_D2D=$MODE) A, proto B, tip B.
tag=$1; iters=$2; shift 2
B=/opt/dlami/nvme/dfperf/runs/xfer_d2d/bin; R=/opt/dlami/nvme/dfperf; MODE=${MODE:-exchange}
for q in "$@"; do
  env -u SIRIUS_PROTO_XFER_D2D REPO=$R/repo $B/one_query.sh ${tag}_tipA $q $iters
  SIRIUS_PROTO_XFER_D2D=$MODE REPO=$R/wt-xfer-d2d $B/one_query.sh ${tag}_protoA $q $iters
  SIRIUS_PROTO_XFER_D2D=$MODE REPO=$R/wt-xfer-d2d $B/one_query.sh ${tag}_protoB $q $iters
  env -u SIRIUS_PROTO_XFER_D2D REPO=$R/repo $B/one_query.sh ${tag}_tipB $q $iters
done
