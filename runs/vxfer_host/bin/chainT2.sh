#!/bin/bash
# timing chain 2 (rest of the suite, one A/U pair per query, order alternating): one lock hold per line
L=/opt/dlami/nvme/dfperf/machine.lock; B=/opt/dlami/nvme/dfperf/runs/vxfer_host/bin; X=/opt/dlami/nvme/dfperf/runs/vxfer_host
run() { echo "WAIT $(date +%T) $*" >> $X/chainT2.log; flock $L bash -c "echo START \$(date +%T) >> $X/chainT2.log; $*"; echo "END $(date +%T) rc=$?" >> $X/chainT2.log; }
run "$B/abba_block.sh v1 'A1 U1' 4 9 13"
run "$B/abba_block.sh v1 'U1 A1' 5 12 18"
run "$B/abba_block.sh v1 'A1 U1' 1 16 22 11"
run "$B/abba_block.sh v1 'U1 A1' 14 15 17"
run "$B/abba_block.sh v1 'A1 U1' 19 20"
echo "chainT2 done $(date +%T)" >> $X/chainT2.log
