#!/bin/bash
# Chain 1: flip build, then the forced-flip measurement on q21. Each step its own lock hold.
R=/opt/dlami/nvme/dfperf; X=$R/runs/shadow_fix; L=$R/machine.lock
echo "queued flip_build $(date +%T)" >> $X/chain1.log
flock $L $X/bin/flip_build.sh; echo "flip_build rc=$? $(date +%T)" >> $X/chain1.log
echo "queued flip_measure $(date +%T)" >> $X/chain1.log
flock $L $X/bin/flip_measure.sh f1 6 21; echo "flip_measure rc=$? $(date +%T)" >> $X/chain1.log
