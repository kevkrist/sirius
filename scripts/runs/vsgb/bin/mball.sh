#!/bin/bash
# MB8 on: PEER pair 0,1; PEER pair 2,3; cross pair 0,2 (no P2P -> HOST), all 4. Run INSIDE one lock hold.
B=/opt/dlami/nvme/dfperf/runs/vsgb/bin
$B/mb.sh g2_peer01 0,1 "[mb8]" SIRIUS_BENCH_DF_GPUS=2 SIRIUS_BENCH_DF_REPS=5
$B/mb.sh g2_peer23 2,3 "[mb8]" SIRIUS_BENCH_DF_GPUS=2 SIRIUS_BENCH_DF_REPS=5
$B/mb.sh g2_host02 0,2 "[mb8]" SIRIUS_BENCH_DF_GPUS=2 SIRIUS_BENCH_DF_REPS=5
$B/mb.sh g4 0,1,2,3 "[mb8]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_REPS=5
