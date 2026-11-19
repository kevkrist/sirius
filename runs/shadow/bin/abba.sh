#!/bin/bash
# Usage: abba.sh <tag> <iters> <q> [<q> ...]
# Per query: tip A, prod A, prod B, tip B; each a fresh process under its own lock acquisition.
# Labels: <tag>_tipA/_prodA/_prodB/_tipB under runs/shadow/.
tag=$1; iters=$2; shift 2
R=/opt/dlami/nvme/dfperf; B=$R/runs/shadow/bin; L=$R/machine.lock
for q in "$@"; do
  REPO=$R/repo       flock $L $B/one_query.sh ${tag}_tipA $q $iters
  REPO=$R/wt-prod-70 flock $L $B/one_query.sh ${tag}_prodA $q $iters
  REPO=$R/wt-prod-70 flock $L $B/one_query.sh ${tag}_prodB $q $iters
  REPO=$R/repo       flock $L $B/one_query.sh ${tag}_tipB $q $iters
done
