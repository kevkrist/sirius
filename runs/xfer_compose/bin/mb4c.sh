#!/bin/bash
# MB4 finish microbench (under the machine lock): G=4, masks x sizes x modes off / pipe (GPU OR) / compose (CPU OR).
B=/opt/dlami/nvme/dfperf/runs/xfer_compose/bin
$B/mb.sh mb4_g4${1:-} 0,1,2,3 "[mb4]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_CHUNK_MIB=0 SIRIUS_BENCH_DF_MB=${MB:-57,174,281,1434} \
  SIRIUS_BENCH_DF_MASKS=${MASKS:-1,3,5,15,4} SIRIUS_BENCH_DF_XFER=${XFER:-off,pipe,compose} SIRIUS_BENCH_DF_REPS=5
