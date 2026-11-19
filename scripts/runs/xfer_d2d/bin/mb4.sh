#!/bin/bash
# MB4 finish microbench: G=4, masks x sizes x modes; plus G=2 pair and exact-kind session cases.
B=/opt/dlami/nvme/dfperf/runs/xfer_d2d/bin
$B/mb.sh mb4_g4 0,1,2,3 "[mb4]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_CHUNK_MIB=0 SIRIUS_BENCH_DF_MB=57,174,281,1434 \
  SIRIUS_BENCH_DF_MASKS=1,3,5,15 SIRIUS_BENCH_DF_XFER=off,star,exchange SIRIUS_BENCH_DF_REPS=5
$B/mb.sh mb4_g2 0,1 "[mb4]" SIRIUS_BENCH_DF_GPUS=2 SIRIUS_BENCH_DF_CHUNK_MIB=0 SIRIUS_BENCH_DF_MB=281,1434 \
  SIRIUS_BENCH_DF_MASKS=1,3 SIRIUS_BENCH_DF_XFER=off,exchange SIRIUS_BENCH_DF_REPS=5
for m in off exchange; do
  $B/mb.sh mb4x_$m 0,1,2,3 "[mb4x]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_PROTO_XFER_D2D=$m SIRIUS_BENCH_DF_REPS=7
  $B/mb.sh mb5_$m 0,1,2,3 "[mb5]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_PROTO_XFER_D2D=$m SIRIUS_BENCH_DF_REPS=7
done
