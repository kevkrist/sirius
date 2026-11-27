#!/bin/bash
# 2026-11-27 close-out: wait for the wt-eager2 build, TRACE tip log pass (input of quent_df.py), then plan r7.
R=/opt/dlami/nvme/dfperf
until grep -q BUILD_DONE $R/logs/eager2_build_r7.log 2>/dev/null; do sleep 20; done
grep -q "BUILD_DONE rc=0" $R/logs/eager2_build_r7.log || { echo BUILD_FAILED; exit 1; }
mkdir -p $R/runs/eager/logs
SIRIUS_LOG_LEVEL=trace $R/runs/baseline/bin/block.sh logpass 2 $(seq 1 22) > $R/runs/eager/logs/r7_logpass.out 2>&1
echo "LOGPASS_DONE $(date +%T)"
$R/runs/eager/bin/plan.sh $R/runs/eager/plans/r7.plan
