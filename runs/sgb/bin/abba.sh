#!/bin/bash
# abba.sh <block> <A side> <B side> q...   : per query A B B A, fresh locked process each, 1 warm + 5 hot
B=/opt/dlami/nvme/dfperf/runs/sgb/bin; L=/opt/dlami/nvme/dfperf/machine.lock
blk=$1; a=$2; b=$3; shift 3
for q in "$@"; do
  flock $L $B/oneq.sh $a ${blk}_${a}_1 $q 6
  flock $L $B/oneq.sh $b ${blk}_${b}_1 $q 6
  flock $L $B/oneq.sh $b ${blk}_${b}_2 $q 6
  flock $L $B/oneq.sh $a ${blk}_${a}_2 $q 6
done
