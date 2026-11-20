#!/bin/bash
# identity gate at G=4 (run under flock)
B=/opt/dlami/nvme/dfperf/runs/xfer_host/bin; X=/opt/dlami/nvme/dfperf/runs/xfer_host
UT_TIMEOUT=600 $B/ut_run.sh check_g4 0,1,2,3 "[xfer_proto]" SIRIUS_BENCH_DF_GPUS=4
grep -q 'All tests passed' $X/out/check_g4.log && touch $X/gate.ok
