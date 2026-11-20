#!/bin/bash
# lk.sh <chain name> <command...>: one lock hold for the command, with LOCKED/END lines in $X/<chain>.log
name=$1; shift
X=/opt/dlami/nvme/dfperf/runs/vxfer_d2d; t0=$(date +%T)
flock /opt/dlami/nvme/dfperf/machine.lock bash -c "echo LOCKED \$(date +%T) queued $t0: $* >> $X/$name.log; $*; echo END rc=\$? \$(date +%T) >> $X/$name.log"
