#!/bin/bash
# timing chain 1 (claim queries + controls): one lock hold per line. (q21 'A1 U1 O1 O2 U2 A2' already queued as an orphan flock.)
L=/opt/dlami/nvme/dfperf/machine.lock; B=/opt/dlami/nvme/dfperf/runs/vxfer_host/bin; X=/opt/dlami/nvme/dfperf/runs/vxfer_host
run() { echo "WAIT $(date +%T) $*" >> $X/chainT1.log; flock $L bash -c "echo START \$(date +%T) >> $X/chainT1.log; $*"; echo "END $(date +%T) rc=$?" >> $X/chainT1.log; }
run "$B/abba_block.sh v1 'U1 A1 O1 O2 A2 U2' 3"
run "$B/abba_block.sh v1 'A1 U1 U2 A2' 7; $B/abba_block.sh v1 'U1 A1' 10"
run "$B/abba_block.sh v1 'A2 U2' 10; $B/abba_block.sh v1 'U1 A1 A2 U2' 8"
run "$B/abba_block.sh v1 'A1 U1 U2 A2' 6; $B/abba_block.sh v1 'U1 A1 A2 U2' 2"
echo "chainT1 done $(date +%T)" >> $X/chainT1.log
