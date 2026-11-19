#!/bin/bash
# p2p_off.sh <label> <iters> <q> ...: as p2p.sh but with every dynamic filter off (test-only SET), to read the no-DF build-site
# task times on the PEER pair. Run as ONE locked command.
label=$1; iters=$2; shift 2
R=/opt/dlami/nvme/dfperf; B=$R/runs/vop2/bin
echo "p2p_off label=$label begin $(date +%T) q=$*" >> $R/runs/vop2/units.log
for q in "$@"; do
  env REPO=$R/wt-op1a CUDA_VISIBLE_DEVICES=0,1 SIRIUS_PROTO_NUM_GPUS=2 SIRIUS_DF_ASYNC_PUBLISH=0 SIRIUS_LOG_LEVEL=debug \
      SIRIUS_ENABLE_TEST_OPTIONS=1 SIRIUS_PRE_SQL="SET enable_dynamic_filter = false;" $B/one_query.sh $label $q $iters
done
echo "p2p_off label=$label end $(date +%T)" >> $R/runs/vop2/units.log
