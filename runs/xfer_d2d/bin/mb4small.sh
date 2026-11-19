#!/bin/bash
# Fixed cost of the island reduce: small Blooms at G=4 (builder level), and the exact-kind session benches with mode pipe.
B=/opt/dlami/nvme/dfperf/runs/xfer_d2d/bin
$B/mb.sh mb4_small 0,1,2,3 "[mb4]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_CHUNK_MIB=0 SIRIUS_BENCH_DF_MB=1,4,16,32 \
  SIRIUS_BENCH_DF_MASKS=1,15 SIRIUS_BENCH_DF_XFER=off,star,exchange,pipe SIRIUS_BENCH_DF_REPS=9
$B/mb.sh mb4x_pipe 0,1,2,3 "[mb4x]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_KEYS=1 SIRIUS_PROTO_XFER_D2D=pipe SIRIUS_BENCH_DF_REPS=7
$B/mb.sh mb5_pipe 0,1,2,3 "[mb5]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_KEYS=1 SIRIUS_BENCH_DF_KINDS=auto SIRIUS_PROTO_XFER_D2D=pipe SIRIUS_BENCH_DF_REPS=7
$B/mb.sh mb5_off2 0,1,2,3 "[mb5]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_KEYS=1 SIRIUS_BENCH_DF_KINDS=auto SIRIUS_PROTO_XFER_D2D=off SIRIUS_BENCH_DF_REPS=7
