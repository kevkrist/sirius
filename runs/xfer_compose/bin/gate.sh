#!/bin/bash
# Bit-identity gate (MBID) on 4 GPUs and two 3-GPU topologies, modes off/pipe/compose (+ forced singletons).
R=/opt/dlami/nvme/dfperf; X=$R/runs/xfer_compose; B=$X/bin
$B/mb.sh gate_g4 0,1,2,3 "[mbid]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_XFER=off,pipe,compose
$B/mb.sh gate_g012 0,1,2 "[mbid]" SIRIUS_BENCH_DF_GPUS=3 SIRIUS_BENCH_DF_MB=3,20 SIRIUS_BENCH_DF_XFER=off,pipe,compose
$B/mb.sh gate_g023 0,2,3 "[mbid]" SIRIUS_BENCH_DF_GPUS=3 SIRIUS_BENCH_DF_MB=3,20 SIRIUS_BENCH_DF_XFER=off,pipe,compose
$B/mb.sh gate_g4_single 0,1,2,3 "[mbid]" SIRIUS_BENCH_DF_GPUS=4 SIRIUS_BENCH_DF_XFER=off,compose SIRIUS_PROTO_XFER_SINGLETONS=1
grep -h "ROW,MBID_SUMMARY" $X/mb/gate_g4.log $X/mb/gate_g012.log $X/mb/gate_g023.log $X/mb/gate_g4_single.log
