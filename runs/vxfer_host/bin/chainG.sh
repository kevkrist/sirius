#!/bin/bash
L=/opt/dlami/nvme/dfperf/machine.lock; B=/opt/dlami/nvme/dfperf/runs/vxfer_host/bin; X=/opt/dlami/nvme/dfperf/runs/vxfer_host
run() { echo "WAIT $(date +%T) $*" >> $X/chainG.log; flock $L bash -c "echo START \$(date +%T) >> $X/chainG.log; $*"; echo "END $(date +%T) rc=$?" >> $X/chainG.log; }
until grep -q "chainE done" $X/chainE.log 2>/dev/null; do sleep 10; done
run "$B/abba_block.sh r1 'U1 R1 R2 U2' 21; $B/abba_block.sh r1 'R1 U1' 3"
run "$B/abba_block.sh g1 'G1 H1 H2 G2' 21"
echo "chainG done $(date +%T)" >> $X/chainG.log
