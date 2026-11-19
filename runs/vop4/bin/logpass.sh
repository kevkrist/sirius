#!/bin/bash
# logpass.sh <q>  (under flock): tip / kind / bpk8 at trace level, 2 iterations each
q=$1; D=/opt/dlami/nvme/dfperf/runs/vop4/bin; R=/opt/dlami/nvme/dfperf
export SIRIUS_LOG_LEVEL=trace
REPO=$R/repo $D/one_query.sh lp_tip $q 2
SIRIUS_PROTO_BLOOM_RANGE=100000:3000000 REPO=$R/wt-op4 $D/one_query.sh lp_kind $q 2
SIRIUS_PROTO_BLOOM_BPK=8 REPO=$R/wt-op4 $D/one_query.sh lp_bpk8 $q 2
