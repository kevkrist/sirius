#!/bin/bash
# timing chain 3: second half of the ABBA for the queries that have one A/U pair (reverse order) -> n=8 on every query. Starts after chain T1.
L=/opt/dlami/nvme/dfperf/machine.lock; B=/opt/dlami/nvme/dfperf/runs/vxfer_host/bin; X=/opt/dlami/nvme/dfperf/runs/vxfer_host
run() { echo "WAIT $(date +%T) $*" >> $X/chainT3.log; flock $L bash -c "echo START \$(date +%T) >> $X/chainT3.log; $*"; echo "END $(date +%T) rc=$?" >> $X/chainT3.log; }
until grep -q "chainT1 done" $X/chainT1.log 2>/dev/null; do sleep 10; done
run "$B/abba_block.sh v1 'U2 A2' 4 9 13"
run "$B/abba_block.sh v1 'A2 U2' 5 12 18"
run "$B/abba_block.sh v1 'U2 A2' 1 16 22 11"
run "$B/abba_block.sh v1 'A2 U2' 14 15 17"
run "$B/abba_block.sh v1 'U2 A2' 19 20"
echo "chainT3 done $(date +%T)" >> $X/chainT3.log
