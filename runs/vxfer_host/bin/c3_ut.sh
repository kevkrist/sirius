#!/bin/bash
# DF multi-GPU unit tests per arm (run under flock)
B=/opt/dlami/nvme/dfperf/runs/vxfer_host/bin
F2='[dynamic_filter][multi_gpu],[device_transfer],[publisher],[construction_plan],[physical_partition],[build_filter]'
for arm in "$@"; do
  env=""; [ $arm != off ] && env="SIRIUS_PROTO_XFER_HOST=$arm"
  UT_TIMEOUT=300 $B/ut_run.sh ut_${arm}_multi 0,1,2,3 "$F2" $env
done
