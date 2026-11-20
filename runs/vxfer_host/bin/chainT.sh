#!/bin/bash
# timing chain: one lock hold per line
L=/opt/dlami/nvme/dfperf/machine.lock; B=/opt/dlami/nvme/dfperf/runs/vxfer_host/bin; X=/opt/dlami/nvme/dfperf/runs/vxfer_host
run() { echo "WAIT $(date +%T) $*" >> $X/chainT.log; flock $L bash -c "echo START \$(date +%T) >> $X/chainT.log; $*"; echo "END $(date +%T) rc=$?" >> $X/chainT.log; }
run "$B/abba_block.sh v1 'A1 U1 O1 O2 U2 A2' 21"
run "$B/abba_block.sh v1 'U1 A1 O1 O2 A2 U2' 3"
run "$B/abba_block.sh v1 'A1 U1 U2 A2' 7; $B/abba_block.sh v1 'U1 A1' 10"
run "$B/abba_block.sh v1 'A2 U2' 10; $B/abba_block.sh v1 'U1 A1 A2 U2' 8"
run "$B/abba_block.sh v1 'A1 U1 U2 A2' 6; $B/abba_block.sh v1 'U1 A1 A2 U2' 2"
run "$B/abba_block.sh v1 'A1 U1' 4 9 13"
run "$B/abba_block.sh v1 'U1 A1' 5 12 18"
run "$B/abba_block.sh v1 'A1 U1' 1 16 22 11"
run "$B/abba_block.sh v1 'U1 A1' 14 15 17"
run "$B/abba_block.sh v1 'A1 U1' 19 20"
echo "chainT done $(date +%T)" >> $X/chainT.log
