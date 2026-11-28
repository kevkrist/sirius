#!/bin/bash
# Usage: abba_q.sh <tag> <order: tppt|pttp> <iters> <q> [<q> ...]
# Per query one lock hold, 4 fresh processes: tip ($R/repo) and prod ($R/wt-prod-xfer) in the given order.
tag=$1; order=$2; iters=$3; shift 3
B=/opt/dlami/nvme/dfperf/runs/xfer_prod/bin; R=/opt/dlami/nvme/dfperf
for q in "$@"; do
  flock $R/machine.lock bash -c "
    half=A
    for arm in $(echo $order | sed 's/./& /g'); do
      [ \$arm = t ] && repo=$R/repo name=tip || { repo=$R/wt-prod-xfer; name=prod; }
      seen=\$(ls -d $R/runs/xfer_prod/${tag}_\${name}A/tpch_*_q$q 2>/dev/null | wc -l)
      [ \$seen -gt 0 ] && half=B || half=A
      REPO=\$repo $B/one_query.sh ${tag}_\${name}\$half $q $iters
    done"
  echo "abba $tag q$q done $(date +%T)" >> $R/runs/xfer_prod/blocks.txt
done
