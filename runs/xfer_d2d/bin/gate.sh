#!/bin/bash
# Bit-identity gate (MBID) on 4 GPUs and two 3-GPU topologies; creates GATE_OK when every case matches.
R=/opt/dlami/nvme/dfperf; X=$R/runs/xfer_d2d; B=$X/bin
grep -q "^rc=0" $R/logs/build_xfer_d2d.log || { echo "gate: build not ok"; exit 1; }
$B/mb.sh gate_g4 0,1,2,3 "[mbid]" SIRIUS_BENCH_DF_GPUS=4
$B/mb.sh gate_g012 0,1,2 "[mbid]" SIRIUS_BENCH_DF_GPUS=3 SIRIUS_BENCH_DF_MB=3,20
$B/mb.sh gate_g023 0,2,3 "[mbid]" SIRIUS_BENCH_DF_GPUS=3 SIRIUS_BENCH_DF_MB=3,20
n=$(grep -h "ROW,MBID_SUMMARY" $X/mb/gate_g4.log $X/mb/gate_g012.log $X/mb/gate_g023.log | grep -c "bad=0$")
[ "$n" = 3 ] && touch $X/GATE_OK
echo "gate summaries ok: $n/3"
