#!/bin/bash
# Run under the machine lock. Builds the worktree if needed, then smoke for the eager prototype: q3 (long scan), q12 and
# q16 (probe scan shorter than the build CONCAT: probe pipeline can finish before the build lands), DEBUG log, 3 iterations
# each, SIRIUS_EAGER_PROBE=1. Then smoke_check.py decides GO / NOGO.
R=/opt/dlami/nvme/dfperf; B=$R/runs/eager/bin
[ -f $R/logs/eager_build.log ] || $B/build.sh
grep -q "BUILD_DONE rc=0" $R/logs/eager_build.log 2>/dev/null || { echo "build failed"; exit 5; }
for q in 3 12 16; do
  SIRIUS_LOG_LEVEL=debug SIRIUS_EAGER_PROBE=1 REPO=$R/wt-eager timeout 900 $B/one_query.sh smoke_dbg $q 3
done
python3 $B/smoke_check.py > $R/runs/eager/smoke_check.txt 2>&1 && touch $R/runs/eager/GO || touch $R/runs/eager/NOGO
