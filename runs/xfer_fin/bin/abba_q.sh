#!/bin/bash
# Usage: abba_q.sh <tag> <armA>=<worktree> <armB>=<worktree> <order: abba|baab> <iters> <q> [<q> ...]
# Per query one lock hold, 4 fresh processes. Output $R/runs/xfer_fin/<tag>_<arm><A|B>/.
tag=$1; a=$2; b=$3; order=$4; iters=$5; shift 5
R=/opt/dlami/nvme/dfperf; B=$R/runs/xfer_fin/bin
an=${a%%=*}; aw=${a#*=}; bn=${b%%=*}; bw=${b#*=}
for q in "$@"; do
  flock $R/machine.lock bash -c "
    for arm in $(echo $order | sed 's/./& /g'); do
      [ \$arm = a ] && { repo=$R/$aw; name=$an; } || { repo=$R/$bw; name=$bn; }
      seen=\$(ls -d $R/runs/xfer_fin/${tag}_\${name}A/tpch_*_q$q 2>/dev/null | wc -l)
      [ \$seen -gt 0 ] && half=B || half=A
      REPO=\$repo $B/one_query.sh ${tag}_\${name}\$half $q $iters
    done"
  echo "abba $tag q$q done $(date +%T)" >> $R/runs/xfer_fin/blocks.txt
done
