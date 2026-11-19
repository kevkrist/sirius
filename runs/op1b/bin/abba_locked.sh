#!/bin/bash
# Usage: abba_locked.sh <tag> <iterations> <q> [<q> ...]   (env ARMS, default "tip pro pro tip")
# One lock hold per query: the arms run back to back as fresh processes. Arms: tip (= $R/repo), pro (wt-op1b, default
# mode 1), pro0 / pro2 (wt-op1b with SIRIUS_DF_DEFER_PUBLISH=0 / 2), g2pro0 / g2pro (wt-op1b on GPUs 0-1 only via
# SIRIUS_PROTO_NUM_GPUS=2, mode 0 / 1). Labels: <tag>_<arm><ordinal of that arm>.
tag=$1; iters=$2; shift 2
B=/opt/dlami/nvme/dfperf/runs/op1b/bin; R=/opt/dlami/nvme/dfperf; L=$R/machine.lock
ARMS=${ARMS:-"tip pro pro tip"}
for q in "$@"; do
  cmd=""
  declare -A n=()
  for arm in $ARMS; do
    n[$arm]=$(( ${n[$arm]:-0} + 1 )); label=${tag}_${arm}${n[$arm]}
    case $arm in
      tip)  cmd+="REPO=$R/repo $B/one_query.sh $label $q $iters; " ;;
      pro)  cmd+="REPO=$R/wt-op1b SIRIUS_DF_DEFER_PUBLISH=1 $B/one_query.sh $label $q $iters; " ;;
      pro0) cmd+="REPO=$R/wt-op1b SIRIUS_DF_DEFER_PUBLISH=0 $B/one_query.sh $label $q $iters; " ;;
      pro2) cmd+="REPO=$R/wt-op1b SIRIUS_DF_DEFER_PUBLISH=2 $B/one_query.sh $label $q $iters; " ;;
      g2pro0) cmd+="REPO=$R/wt-op1b SIRIUS_PROTO_NUM_GPUS=2 SIRIUS_DF_DEFER_PUBLISH=0 $B/one_query.sh $label $q $iters; " ;;
      g2pro)  cmd+="REPO=$R/wt-op1b SIRIUS_PROTO_NUM_GPUS=2 SIRIUS_DF_DEFER_PUBLISH=1 $B/one_query.sh $label $q $iters; " ;;
    esac
  done
  unset n
  flock $L bash -c "$cmd"
done
