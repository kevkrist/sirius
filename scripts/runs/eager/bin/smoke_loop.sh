#!/bin/bash
# Queue the smoke on the machine lock; re-queue while the build is not done (smoke.sh exits 77 at once in that case).
R=/opt/dlami/nvme/dfperf; B=$R/runs/eager/bin
for t in $(seq 1 200); do flock $R/machine.lock $B/smoke.sh; rc=$?; [ $rc -ne 77 ] && break; sleep 20; done
echo "smoke_rc=$rc"; cat $R/runs/eager/smoke_check.txt 2>/dev/null
