#!/bin/bash
# Usage: unit.sh <tag> <iterations> <q> <arm> [<arm> ...]   (run under the machine lock: arms back to back, fresh process each)
tag=$1; iters=$2; q=$3; shift 3
B=/opt/dlami/nvme/dfperf/runs/eager/bin; R=/opt/dlami/nvme/dfperf
declare -A n=()
for arm in "$@"; do
  n[$arm]=$(( ${n[$arm]:-0} + 1 )); label=${tag}_${arm}${n[$arm]}
  case $arm in
    tip)  REPO=$R/repo $B/one_query.sh $label $q $iters ;;
    pro)  REPO=$R/wt-eager SIRIUS_EAGER_PROBE=1 $B/one_query.sh $label $q $iters ;;
    pro0) REPO=$R/wt-eager SIRIUS_EAGER_PROBE=0 $B/one_query.sh $label $q $iters ;;
    pro2) REPO=$R/wt-eager SIRIUS_EAGER_PROBE=2 $B/one_query.sh $label $q $iters ;;
    pros) REPO=$R/wt-eager SIRIUS_EAGER_PROBE=1 SIRIUS_EAGER_CONCAT_FIRST=0 $B/one_query.sh $label $q $iters ;;
    o1)   REPO=$R/wt-eager2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=1 $B/one_query.sh $label $q $iters ;;
    o2)   REPO=$R/wt-eager2 SIRIUS_DF_DEFER_PUBLISH=1 SIRIUS_EAGER_PROBE=1 $B/one_query.sh $label $q $iters ;;
    ob)   REPO=$R/wt-eager2 SIRIUS_DF_DEFER_PUBLISH=1 SIRIUS_EAGER_PROBE=0 $B/one_query.sh $label $q $iters ;;
    t1)   REPO=$R/wt-eager2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=1 SIRIUS_EAGER_SCAN_THROTTLE=1 $B/one_query.sh $label $q $iters ;;
    t2)   REPO=$R/wt-eager2 SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=1 SIRIUS_EAGER_SCAN_THROTTLE=2 $B/one_query.sh $label $q $iters ;;
    o2t1) REPO=$R/wt-eager2 SIRIUS_DF_DEFER_PUBLISH=1 SIRIUS_EAGER_PROBE=1 SIRIUS_EAGER_SCAN_THROTTLE=1 $B/one_query.sh $label $q $iters ;;
  esac
done
