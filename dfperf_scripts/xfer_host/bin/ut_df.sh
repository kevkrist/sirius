#!/bin/bash
# DF unit tests on the prototype binary: switch off, hostor, gpuor (same Catch2 filters). Run under flock.
# arg1: which arms (default "off hostor gpuor")
B=/opt/dlami/nvme/dfperf/runs/xfer_host/bin
F1='[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]'
F2='[dynamic_filter][multi_gpu],[device_transfer],[publisher],[construction_plan],[physical_partition],[build_filter]'
for arm in ${1:-off hostor gpuor}; do
  env=""; [ $arm != off ] && env="SIRIUS_PROTO_XFER_HOST=$arm"
  $B/ut_run.sh ut_${arm}_single 0 "$F1" $env
  $B/ut_run.sh ut_${arm}_multi 0,1,2,3 "$F2" $env
done
