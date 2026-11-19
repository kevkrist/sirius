#!/bin/bash
# MB4P G=4 with larger Bloom chunks (SIRIUS_PROTO_BLOOM_CHUNK_MIB). Run under flock.
B=/opt/dlami/nvme/dfperf/runs/xfer_host/bin
for c in 16 32; do
  UT_TIMEOUT=400 $B/ut_run.sh mb4p_g4_chunk$c 0,1,2,3 "[mb4p]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_REPS=5 \
    SIRIUS_BENCH_DF_MB=57,281,1434 SIRIUS_BENCH_DF_SOURCES=1,4 SIRIUS_BENCH_DF_WINDOWS=2,4 SIRIUS_PROTO_BLOOM_CHUNK_MIB=$c
done
