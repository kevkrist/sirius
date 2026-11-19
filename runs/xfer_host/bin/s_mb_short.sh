#!/bin/bash
# short MB4P pass at G=4 (run under flock)
B=/opt/dlami/nvme/dfperf/runs/xfer_host/bin
UT_TIMEOUT=500 $B/ut_run.sh mb4p_g4_a 0,1,2,3 "[mb4p]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_REPS=5 SIRIUS_BENCH_DF_MB=57,174,281,1434
