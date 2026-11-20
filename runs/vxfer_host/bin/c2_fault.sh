#!/bin/bash
# CUDA status fault-injection tests (separate binary) per arm: off auto hostor gpuor (run under flock)
B=/opt/dlami/nvme/dfperf/runs/vxfer_host/bin
export UTBIN=build/release/test/cpp/sirius_cuda_fault_test
for arm in ${1:-off auto hostor gpuor}; do
  env=""; [ $arm != off ] && env="SIRIUS_PROTO_XFER_HOST=$arm"
  UT_TIMEOUT=200 $B/ut_run.sh fault_multi_$arm 0,1,2,3 "[cuda_status_fault][multi_gpu]~[.]" $env
  UT_TIMEOUT=200 $B/ut_run.sh fault_single_$arm 0 "[cuda_status_fault][dynamic_filter]~[multi_gpu],[cuda_status_fault][device_transfer]~[multi_gpu]" $env
done
