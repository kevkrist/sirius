#!/bin/bash
L=/opt/dlami/nvme/dfperf/machine.lock; B=/opt/dlami/nvme/dfperf/runs/vxfer_host/bin; X=/opt/dlami/nvme/dfperf/runs/vxfer_host
run() { echo "WAIT $(date +%T) $*" >> $X/chainH.log; flock $L bash -c "echo START \$(date +%T) >> $X/chainH.log; $*"; echo "END $(date +%T) rc=$?" >> $X/chainH.log; }
run "$B/c5_stress.sh 2 3 stress2"
echo "chainH done $(date +%T)" >> $X/chainH.log
