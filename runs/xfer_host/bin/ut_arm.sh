#!/bin/bash
# DF unit tests on the prototype binary for one arm: off | hostor | gpuor | auto. Run under flock.
B=/opt/dlami/nvme/dfperf/runs/xfer_host/bin
F1='[dynamic_filter]~[multi_gpu]~[cuda_status_fault]~[.]'
F2='[dynamic_filter][multi_gpu],[device_transfer],[publisher],[construction_plan],[physical_partition],[build_filter]'
arm=$1; env=""; [ $arm != off ] && env="SIRIUS_PROTO_XFER_HOST=$arm"
UT_TIMEOUT=900 $B/ut_run.sh ut_${arm}_single 0 "$F1" $env
UT_TIMEOUT=900 $B/ut_run.sh ut_${arm}_multi 0,1,2,3 "$F2" $env
