#!/bin/bash
# race probe: N concurrent identity-gate processes on the same 4 GPUs, R rounds (run under flock)
B=/opt/dlami/nvme/dfperf/runs/vxfer_host/bin
n=${1:-3}; rounds=${2:-2}; tag=${3:-stress}
for r in $(seq 1 $rounds); do
  for i in $(seq 1 $n); do
    UT_TIMEOUT=400 $B/ut_run.sh ${tag}_r${r}_p$i 0,1,2,3 "[xfer_proto]" SIRIUS_BENCH_DF_GPUS=4 &
  done
  wait
done
