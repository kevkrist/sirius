#!/bin/bash
# contention rerun: background hostor (links saturated both ways) long enough to cover the whole foreground (run under flock)
B=/opt/dlami/nvme/dfperf/runs/vxfer_host/bin
FG="SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_REPS=5 SIRIUS_BENCH_DF_MB=281,1434 SIRIUS_BENCH_DF_SOURCES=2,4 SIRIUS_BENCH_DF_MODES=off,gpuor,hostor"
for bg in hostor gpuor; do
  UT_TIMEOUT=200 $B/ut_run.sh mb4p_bg2_$bg 0,1,2,3 "[mb4p]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_REPS=4000 SIRIUS_BENCH_DF_MB=1434 SIRIUS_BENCH_DF_SOURCES=4 SIRIUS_BENCH_DF_MODES=$bg &
  bgpid=$!
  sleep 12
  date +%T.%N > /opt/dlami/nvme/dfperf/runs/vxfer_host/out/mb4p_fg2_under_$bg.t0
  UT_TIMEOUT=150 $B/ut_run.sh mb4p_fg2_under_$bg 0,1,2,3 "[mb4p]" $FG
  pgrep -f "sirius_unittest \[mb4p\]" > /opt/dlami/nvme/dfperf/runs/vxfer_host/out/mb4p_fg2_under_$bg.bgalive
  pkill -f "sirius_unittest \[mb4p\]"; wait $bgpid 2>/dev/null
  sleep 2
done
