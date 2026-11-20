#!/bin/bash
# MB4P replication (quiet), then the same foreground rows while a second process keeps the links busy (run under flock).
#   bg=off    : tip loop over 1434 MiB, 4 contributors (serial legs, one link at a time)
#   bg=hostor : hostor over 1434 MiB, 4 contributors (all links, both directions)
B=/opt/dlami/nvme/dfperf/runs/vxfer_host/bin
FG="SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_REPS=5 SIRIUS_BENCH_DF_MB=281,1434 SIRIUS_BENCH_DF_SOURCES=1,2,4 SIRIUS_BENCH_DF_MODES=off,hostor"
UT_TIMEOUT=200 $B/ut_run.sh mb4p_quiet 0,1,2,3 "[mb4p]" $FG SIRIUS_BENCH_DF_SOURCES=1,2,3,4 SIRIUS_BENCH_DF_MODES=off,gpuor,hostor
for bg in off hostor; do
  UT_TIMEOUT=120 $B/ut_run.sh mb4p_bg_$bg 0,1,2,3 "[mb4p]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_REPS=400 SIRIUS_BENCH_DF_MB=1434 SIRIUS_BENCH_DF_SOURCES=4 SIRIUS_BENCH_DF_MODES=$bg &
  bgpid=$!
  sleep 12
  UT_TIMEOUT=100 $B/ut_run.sh mb4p_fg_under_$bg 0,1,2,3 "[mb4p]" $FG SIRIUS_BENCH_DF_SOURCES=4
  pkill -f "sirius_unittest \[mb4p\]"; wait $bgpid 2>/dev/null
  sleep 2
done
