#!/bin/bash
# p2p.sh <label> <iters> <q> [<q> ...]: tip-behaviour (synchronous publication) on the PEER pair GPUs 0,1 only, using the
# OP1A prototype binary (read-only use) for its SIRIUS_PROTO_NUM_GPUS override. Site measurement, no A/B. Run as ONE locked command.
label=$1; iters=$2; shift 2
R=/opt/dlami/nvme/dfperf; B=$R/runs/vop2/bin
echo "p2p label=$label begin $(date +%T) q=$*" >> $R/runs/vop2/units.log
for q in "$@"; do
  env REPO=$R/wt-op1a CUDA_VISIBLE_DEVICES=0,1 SIRIUS_PROTO_NUM_GPUS=2 SIRIUS_DF_ASYNC_PUBLISH=0 SIRIUS_LOG_LEVEL=debug $B/one_query.sh $label $q $iters
done
echo "p2p label=$label end $(date +%T)" >> $R/runs/vop2/units.log
