#!/bin/bash
B=/opt/dlami/nvme/dfperf/runs/op1a/bin
$B/build_ut.sh
$B/mb.sh mb8_g2 0,1 "[mb8]" SIRIUS_BENCH_DF_GPUS=2 SIRIUS_BENCH_DF_REPS=7
