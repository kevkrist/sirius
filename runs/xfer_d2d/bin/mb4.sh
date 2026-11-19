#!/bin/bash
# MB4 finish microbench (run under the machine lock): G=4, masks x sizes x modes.
# mask bit d = GPU d contributed; root = GPU 0. 1 = root only, 3 = pair 0-1, 5 = GPUs 0+2 (one per pair), 15 = all, 4 = only GPU 2.
B=/opt/dlami/nvme/dfperf/runs/xfer_d2d/bin
$B/mb.sh mb4_g4${1:-} 0,1,2,3 "[mb4]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_CHUNK_MIB=0 SIRIUS_BENCH_DF_MB=57,174,281,1434 \
  SIRIUS_BENCH_DF_MASKS=1,3,5,15,4 SIRIUS_BENCH_DF_XFER=off,star,exchange,pipe SIRIUS_BENCH_DF_REPS=5
