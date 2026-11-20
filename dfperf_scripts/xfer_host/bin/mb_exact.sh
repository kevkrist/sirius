#!/bin/bash
# Exact-kind builds (MB4x slices, MB2 auto kinds, MB5 one-shot replicate) with the replica switch off / on. Run under flock.
B=/opt/dlami/nvme/dfperf/runs/xfer_host/bin
for sw in 0 1; do
  $B/ut_run.sh mb4x_g4_r$sw 0,1,2,3 "[mb4x]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_REPS=5 SIRIUS_PROTO_XFER_REPLICA=$sw
  $B/ut_run.sh mb2_g4_r$sw 0,1,2,3 "[mb2]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_REPS=5 SIRIUS_BENCH_DF_KINDS=auto \
    SIRIUS_BENCH_DF_KEYS=1,2 SIRIUS_BENCH_DF_ROWS=1000,200000 SIRIUS_BENCH_DF_BATCHES=1,8,64 SIRIUS_PROTO_XFER_REPLICA=$sw
done
