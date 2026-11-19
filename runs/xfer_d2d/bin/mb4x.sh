#!/bin/bash
# Exact-kind session benches (MB4x exact slices, MB5 small builds) at G=4, switch off vs on; G=2 pair sanity for MB4.
B=/opt/dlami/nvme/dfperf/runs/xfer_d2d/bin
for m in off exchange; do
  $B/mb.sh mb4x_$m 0,1,2,3 "[mb4x]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_KEYS=1 SIRIUS_PROTO_XFER_D2D=$m SIRIUS_BENCH_DF_REPS=7
  $B/mb.sh mb5_$m 0,1,2,3 "[mb5]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_KEYS=1 SIRIUS_BENCH_DF_KINDS=auto SIRIUS_PROTO_XFER_D2D=$m SIRIUS_BENCH_DF_REPS=7
done
$B/mb.sh mb4_g2 0,1 "[mb4]" SIRIUS_BENCH_DF_GPUS=2 SIRIUS_BENCH_DF_CHUNK_MIB=0 SIRIUS_BENCH_DF_MB=281,1434 \
  SIRIUS_BENCH_DF_MASKS=1,3 SIRIUS_BENCH_DF_XFER=off,pipe SIRIUS_BENCH_DF_REPS=5
