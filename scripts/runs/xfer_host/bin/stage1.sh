#!/bin/bash
# build + go/no-go: correctness of every schedule, then a short MB4P pass. Run under flock.
# A failed build puts this script back at the head of the queue (at most 3 times): fix the code before the next hold.
B=/opt/dlami/nvme/dfperf/runs/xfer_host/bin; X=/opt/dlami/nvme/dfperf/runs/xfer_host
if ! $B/build_only.sh; then
  n=$(cat $X/stage1.tries 2>/dev/null || echo 0); n=$((n+1)); echo $n > $X/stage1.tries
  if [ $n -lt 4 ]; then (flock 9; sed -i "1i $B/stage1.sh" $X/queue.txt) 9>$X/queue.lock; fi
  exit 1
fi
UT_TIMEOUT=400 $B/ut_run.sh check_g4 0,1,2,3 "[xfer_proto]" SIRIUS_BENCH_DF_GPUS=4
grep -q 'All tests passed' $X/out/check_g4.log && touch $X/gate.ok
UT_TIMEOUT=500 $B/ut_run.sh mb4p_g4_a 0,1,2,3 "[mb4p]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_REPS=5 SIRIUS_BENCH_DF_MB=57,281,1434
