#!/bin/bash
# G=2 variants: GPUs 0,2 (no P2P, one uplink each); GPUs 0,1 forced HOST vs PEER. Run under flock.
B=/opt/dlami/nvme/dfperf/runs/xfer_host/bin
C="SIRIUS_BENCH_DF_GPUS=2 SIRIUS_BENCH_DF_REPS=5 SIRIUS_BENCH_DF_MB=57,281,1434 SIRIUS_BENCH_DF_SOURCES=1,2"
$B/ut_run.sh mb4p_g2_02 0,2 "[mb4p]" $C
$B/ut_run.sh mb4p_g2_01_host 0,1 "[mb4p]" $C SIRIUS_BENCH_DF_FORCE_HOST=1
$B/ut_run.sh mb4p_g2_01_peer 0,1 "[mb4p]" $C SIRIUS_BENCH_DF_MODES=off
$B/ut_run.sh check_g2_02 0,2 "[xfer_proto]" SIRIUS_BENCH_DF_GPUS=2
