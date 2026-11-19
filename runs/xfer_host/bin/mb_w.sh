#!/bin/bash
# MB4P G=4, 4 contributors: window / OR-thread sweep. Run under flock.
B=/opt/dlami/nvme/dfperf/runs/xfer_host/bin
UT_TIMEOUT=500 $B/ut_run.sh mb4p_g4_w 0,1,2,3 "[mb4p]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_REPS=5 \
  SIRIUS_BENCH_DF_MB=57,281,1434 SIRIUS_BENCH_DF_SOURCES=4 SIRIUS_BENCH_DF_WINDOWS=4,1,2,8 SIRIUS_BENCH_DF_OR_THREADS=16,4,8,32
