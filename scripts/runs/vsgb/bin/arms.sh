#!/bin/bash
# Usage: arms.sh <tag> <iters> <q> <arm> [<arm> ...]  -- runs INSIDE one lock hold (caller wraps with flock).
# Label <tag>_<arm>_<ordinal>. Arms back to back, fresh process each.
tag=$1; iters=$2; q=$3; shift 3
B=/opt/dlami/nvme/dfperf/runs/vsgb/bin
declare -A n=()
echo "START tag=$tag q=$q arms=$* $(date +%F_%T)"
for arm in "$@"; do
  n[$arm]=$(( ${n[$arm]:-0} + 1 ))
  $B/oneq.sh $arm ${tag}_${arm}_${n[$arm]} $q $iters
  echo "  arm=$arm rc=$? $(date +%T)"
done
echo "END tag=$tag q=$q $(date +%F_%T)"
