#!/bin/bash
# chunk.sh <tag> <iters> "<arms>" <q...> : abba.sh for the queries not yet in <tag>.progress (resumable, < 60 min per call).
tag=$1; iters=$2; arms=$3; shift 3
R=/opt/dlami/nvme/dfperf; B=$R/runs/hybridfull/bin
for q in "$@"; do
  grep -q "^q$q done" $R/runs/hybridfull/$tag.progress 2>/dev/null && continue
  # a half-finished query is redone from scratch
  for a in $arms; do rm -rf $R/runs/hybridfull/${tag}_${a}_a/*_q$q $R/runs/hybridfull/${tag}_${a}_b/*_q$q; done
  $B/abba.sh $tag $iters "$arms" $q > /dev/null
done
