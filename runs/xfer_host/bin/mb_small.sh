#!/bin/bash
# MB4P G=4 small arrays (fixed cost of the pipelines) + 3 contributors. Run under flock.
B=/opt/dlami/nvme/dfperf/runs/xfer_host/bin
UT_TIMEOUT=400 $B/ut_run.sh mb4p_g4_small 0,1,2,3 "[mb4p]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_REPS=9 \
  SIRIUS_BENCH_DF_MB=1,4,16 SIRIUS_BENCH_DF_SOURCES=1,2,4
UT_TIMEOUT=400 $B/ut_run.sh mb4p_g4_s3 0,1,2,3 "[mb4p]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_REPS=5 \
  SIRIUS_BENCH_DF_MB=57,281,1434 SIRIUS_BENCH_DF_SOURCES=3
