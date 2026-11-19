#!/bin/bash
# chain.sh <pid-to-wait-for> <spec> [<spec> ...]   spec = label:q:iters:arm,arm,...[:g2]   (one locked hold per spec)
R=/opt/dlami/nvme/dfperf; B=$R/runs/op1a/bin
pid=$1; shift
tail --pid=$pid -f /dev/null 2>/dev/null
for spec in "$@"; do
  IFS=: read label q iters arms g2 <<< "$spec"
  if [ "$g2" = g2 ]; then
    CUDA_VISIBLE_DEVICES=0,1 SIRIUS_PROTO_NUM_GPUS=2 flock $R/machine.lock $B/pair.sh $label $q $iters ${arms//,/ }
  else
    flock $R/machine.lock $B/pair.sh $label $q $iters ${arms//,/ }
  fi
done
