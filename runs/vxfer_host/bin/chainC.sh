#!/bin/bash
L=/opt/dlami/nvme/dfperf/machine.lock; B=/opt/dlami/nvme/dfperf/runs/vxfer_host/bin; X=/opt/dlami/nvme/dfperf/runs/vxfer_host
run() { echo "WAIT $(date +%T) $*" >> $X/chainC.log; flock $L bash -c "echo START \$(date +%T) >> $X/chainC.log; $*"; echo "END $(date +%T) rc=$?" >> $X/chainC.log; }
run "$B/c1_identity.sh; $B/c2_fault.sh 'off auto'"
run "$B/c6_log.sh log 21 3"
run "$B/c2_fault.sh 'hostor gpuor'; $B/c3_ut.sh hostor gpuor"
run "$B/c7_nsys.sh 'A1 U1' 21 7"
run "$B/c4_perm.sh 2,0,3,1:4 1,2,3:3 0,1,2:3 3,1:2; $B/c3_ut.sh auto"
run "$B/c5_stress.sh 3 2"
run "$B/c6_log.sh log 7 10 8"
echo "chainC done $(date +%T)" >> $X/chainC.log
