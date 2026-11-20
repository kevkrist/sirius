#!/bin/bash
# After chain_ut.sh wt-prod-69 has finished: two mutants, then the clean rebuild. Each its own lock hold.
R=/opt/dlami/nvme/dfperf; X=$R/runs/shadow_fix; L=$R/machine.lock
until grep -q 'ALL_OK\|done rc=[1-9]' <(tail -n +9 $X/chain_ut_wt-prod-69.log); do sleep 15; done
for m in M1 M2 clean; do echo "queued $m $(date +%T)" >> $X/chain_mut.log; flock $L $X/bin/mutant.sh $m; echo "done $m $(date +%T)" >> $X/chain_mut.log; done
