#!/bin/bash
# Usage: arms.sh <tag> <iterations> <q> <arm> [<arm> ...]   -- runs INSIDE one lock hold (caller wraps with flock).
# Arms: tip (= $R/repo), pro (wt-op1b mode 1), pro0 (wt-op1b mode 0), g2pro0/g2pro (GPUs 0-1 only, PEER),
# h2pro0/h2pro (CUDA_VISIBLE_DEVICES=0,2 + 2 GPUs: non-P2P pair, HOST staging). Label <tag>_<arm><ordinal>.
tag=$1; iters=$2; q=$3; shift 3
B=/opt/dlami/nvme/dfperf/runs/vop1b/bin; R=/opt/dlami/nvme/dfperf
declare -A n=()
for arm in "$@"; do
  n[$arm]=$(( ${n[$arm]:-0} + 1 )); label=${tag}_${arm}${n[$arm]}
  case $arm in
    tip)  REPO=$R/repo $B/one_query.sh $label $q $iters ;;
    pro)  REPO=$R/wt-op1b SIRIUS_DF_DEFER_PUBLISH=1 $B/one_query.sh $label $q $iters ;;
    pro0) REPO=$R/wt-op1b SIRIUS_DF_DEFER_PUBLISH=0 $B/one_query.sh $label $q $iters ;;
    g2pro0) REPO=$R/wt-op1b SIRIUS_PROTO_NUM_GPUS=2 SIRIUS_DF_DEFER_PUBLISH=0 $B/one_query.sh $label $q $iters ;;
    g2pro)  REPO=$R/wt-op1b SIRIUS_PROTO_NUM_GPUS=2 SIRIUS_DF_DEFER_PUBLISH=1 $B/one_query.sh $label $q $iters ;;
    h2pro0) REPO=$R/wt-op1b CUDA_VISIBLE_DEVICES=0,2 SIRIUS_PROTO_NUM_GPUS=2 SIRIUS_DF_DEFER_PUBLISH=0 $B/one_query.sh $label $q $iters ;;
    h2pro)  REPO=$R/wt-op1b CUDA_VISIBLE_DEVICES=0,2 SIRIUS_PROTO_NUM_GPUS=2 SIRIUS_DF_DEFER_PUBLISH=1 $B/one_query.sh $label $q $iters ;;
  esac
done
