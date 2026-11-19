#!/bin/bash
# One locked hold: DF unit tests (async default, then SIRIUS_DF_ASYNC_PUBLISH=0) and MB8 on GPUs 0,1.
B=/opt/dlami/nvme/dfperf/runs/op1a/bin
F='[publisher],[publication_claim],[physical_partition],[in_task_publication],[build_filter],[dynamic_filter][construction]'
$B/mb.sh ut_async 0,1,2,3 "$F"
$B/mb.sh ut_sync 0,1,2,3 "$F" SIRIUS_DF_ASYNC_PUBLISH=0
$B/mb.sh mb8_g2 0,1 "[mb8]" SIRIUS_BENCH_DF_GPUS=2 SIRIUS_BENCH_DF_REPS=7
