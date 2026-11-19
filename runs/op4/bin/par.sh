#!/bin/bash
# par.sh <tag> <iters> <q> : A1 B1 B2 A2 as four independent lock waiters (order decided by the lock). env A_*/B_* as abba.sh
tag=$1; iters=$2; q=$3
B=/opt/dlami/nvme/dfperf/runs/op4/bin; L=/opt/dlami/nvme/dfperf/machine.lock
for l in A1 B1 B2 A2; do
  if [ ${l:0:1} = A ]; then repo=${A_REPO:-/opt/dlami/nvme/dfperf/repo}; sql=${A_SQL:-}; ev=${A_ENV:-}
  else repo=${B_REPO:-/opt/dlami/nvme/dfperf/repo}; sql=${B_SQL:-}; ev=${B_ENV:-}; fi
  env $ev REPO=$repo SIRIUS_PRE_SQL="$sql" flock $L $B/one_query.sh ${tag}_$l $q $iters &
done
wait
