#!/bin/bash
L=/opt/dlami/nvme/dfperf/machine.lock; B=/opt/dlami/nvme/dfperf/runs/vxfer_host/bin; X=/opt/dlami/nvme/dfperf/runs/vxfer_host
run() { echo "WAIT $(date +%T) $*" >> $X/chainE.log; flock $L bash -c "echo START \$(date +%T) >> $X/chainE.log; $*"; echo "END $(date +%T) rc=$?" >> $X/chainE.log; }
run "$B/c8_contend.sh"
run "$B/abba_block.sh w1 'U1 W1 W2 U2' 21; $B/abba_block.sh w1 'W1 U1' 3"
run "$B/c9_rep.sh rep 12 U1 21 3 7"
run "$B/c9_rep.sh rep 12 U1 10 8 4 9"
echo "chainE done $(date +%T)" >> $X/chainE.log
