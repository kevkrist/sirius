#!/bin/bash
# Usage: chain_ut.sh <worktree-name>  -- build, then the DF unit tests. Each step its own lock hold; stops at the first failure.
R=/opt/dlami/nvme/dfperf; X=$R/runs/shadow_fix; L=$R/machine.lock; B=$R/runs/shadow/bin; wt=$1
log=$X/chain_ut_$wt.log
step() { echo "queued $* $(date +%T)" >> $log; flock $L "$@"; rc=$?; echo "done rc=$rc $* $(date +%T)" >> $log; return $rc; }
step $B/build.sh $wt || exit 1
step $X/bin/shadow_test.sh $wt || exit 2
step $B/unit.sh $wt single || exit 3
step $B/unit.sh $wt mgpu || exit 4
echo "ALL_OK $(date +%T) $(git -C $R/$wt rev-parse --short HEAD)" >> $log
