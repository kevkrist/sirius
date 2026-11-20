#!/bin/bash
# Final evidence for prod/shadow-2570 in wt-prod-70: build, DF unit tests, q21/q8 ABBA, 22-query DEBUG pass.
# Each step its own lock hold; stops at the first failed build or unit step.
R=/opt/dlami/nvme/dfperf; X=$R/runs/shadow_fix; L=$R/machine.lock; B=$R/runs/shadow/bin; wt=wt-prod-70
log=$X/chain_final.log
step() { echo "queued $* $(date +%T)" >> $log; flock $L "$@"; rc=$?; echo "done rc=$rc $* $(date +%T)" >> $log; return $rc; }
step $B/build.sh $wt || exit 1
step $X/bin/shadow_test.sh $wt || exit 2
step $B/unit.sh $wt single || exit 3
step $B/unit.sh $wt mgpu || exit 4
step $B/abba1.sh r1 6 21
step $B/abba1.sh r1 6 8
step $B/dbgn.sh dbg_fix prod 21 8 2 20 1 3 4
step $B/dbgn.sh dbg_fix prod 5 6 7 9 10 11 12
step $B/dbgn.sh dbg_fix prod 13 14 15 16 17 18 19 22
echo "ALL_DONE $(date +%T) $(git -C $R/$wt rev-parse --short HEAD)" >> $log
