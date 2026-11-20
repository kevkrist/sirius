#!/bin/bash
L=/opt/dlami/nvme/dfperf/machine.lock; B=/opt/dlami/nvme/dfperf/runs/vxfer_host/bin; X=/opt/dlami/nvme/dfperf/runs/vxfer_host
run() { echo "WAIT $(date +%T) $*" >> $X/chainF.log; flock $L bash -c "echo START \$(date +%T) >> $X/chainF.log; $*"; echo "END $(date +%T) rc=$?" >> $X/chainF.log; }
until grep -q "chainC done" $X/chainC.log 2>/dev/null; do sleep 10; done
run "$B/c8b_contend.sh"
echo "chainF done $(date +%T)" >> $X/chainF.log
