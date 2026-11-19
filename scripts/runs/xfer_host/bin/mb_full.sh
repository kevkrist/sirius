#!/bin/bash
# Full MB4P pass at G=4 (windows / threads sweep). Run under flock.
B=/opt/dlami/nvme/dfperf/runs/xfer_host/bin
$B/ut_run.sh mb4p_g4_full 0,1,2,3 "[mb4p]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_REPS=5 \
  SIRIUS_BENCH_DF_MB=57,174,281,1434 SIRIUS_BENCH_DF_WINDOWS=4,2,8 SIRIUS_BENCH_DF_OR_THREADS=16,8,32
