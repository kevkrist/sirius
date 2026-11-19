#!/bin/bash
# rg.sh <tag>: incremental rebuild, then the identity gates (Bloom MBID on 4 GPUs + two 3-GPU topologies, replica relay).
# Creates GATE_OK only when the build is ok and every gate passes. Run under the machine lock.
R=/opt/dlami/nvme/dfperf; X=$R/runs/xfer_d2d; B=$X/bin; tag=${1:-re}
rm -f $X/GATE_OK
$B/rebuild.sh $tag
grep -q "^rc=0" $R/logs/build_xfer_d2d_$tag.log || { echo "rg: build failed"; grep -n "error" $R/logs/build_xfer_d2d_$tag.log | head -20; exit 1; }
$B/mb.sh gate_g4 0,1,2,3 "[mbid]" SIRIUS_BENCH_DF_GPUS=4
$B/mb.sh gate_g012 0,1,2 "[mbid]" SIRIUS_BENCH_DF_GPUS=3 SIRIUS_BENCH_DF_MB=3,20
$B/mb.sh gate_g023 0,2,3 "[mbid]" SIRIUS_BENCH_DF_GPUS=3 SIRIUS_BENCH_DF_MB=3,20
$B/mb.sh gate_relay 0,1,2,3 "[xfer_relay]"
n=$(grep -h "ROW,MBID_SUMMARY" $X/mb/gate_g4.log $X/mb/gate_g012.log $X/mb/gate_g023.log | grep -c "bad=0$")
r=$(grep -c "All tests passed" $X/mb/gate_relay.log)
[ "$n" = 3 ] && [ "$r" = 1 ] && touch $X/GATE_OK
echo "gate summaries ok: $n/3 relay: $r/1"
