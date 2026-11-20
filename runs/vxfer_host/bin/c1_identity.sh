#!/bin/bash
# identity gates, natural GPU order (run under flock)
B=/opt/dlami/nvme/dfperf/runs/vxfer_host/bin
UT_TIMEOUT=300 $B/ut_run.sh check_g4 0,1,2,3 "[xfer_proto]" SIRIUS_BENCH_DF_GPUS=4
UT_TIMEOUT=200 $B/ut_run.sh exact_g4 0,1,2,3 "[xfer_proto_exact]" SIRIUS_BENCH_DF_GPUS=4
