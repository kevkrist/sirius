#!/bin/bash
# abba.sh <tag> <iters> <q...>   env: A_REPO A_SQL A_ENV  B_REPO B_SQL B_ENV (ENV = "K=V K=V")
# Per query: A B B A, each a fresh locked process. Labels <tag>_A1 <tag>_B1 <tag>_B2 <tag>_A2.
tag=$1; iters=$2; shift 2
B=/opt/dlami/nvme/dfperf/runs/op4/bin; L=/opt/dlami/nvme/dfperf/machine.lock
run() { # side label q
  if [ "$1" = A ]; then repo=${A_REPO:-/opt/dlami/nvme/dfperf/repo}; sql=${A_SQL:-}; ev=${A_ENV:-}
  else repo=${B_REPO:-/opt/dlami/nvme/dfperf/repo}; sql=${B_SQL:-}; ev=${B_ENV:-}; fi
  env $ev REPO=$repo SIRIUS_PRE_SQL="$sql" flock $L $B/one_query.sh $2 $3 $iters
}
for q in "$@"; do
  run A ${tag}_A1 $q; run B ${tag}_B1 $q; run B ${tag}_B2 $q; run A ${tag}_A2 $q
done
