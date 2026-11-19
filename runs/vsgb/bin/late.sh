#!/bin/bash
# Flexible verifier ticket: runs $R/runs/vsgb/bin/late_body.sh if it exists when the lock is acquired; otherwise exits at once.
B=/opt/dlami/nvme/dfperf/runs/vsgb/bin
echo "late ticket $1 acquired $(date +%F_%T)"
if [ -x $B/late_body_$1.sh ]; then $B/late_body_$1.sh; else echo "nothing to do"; fi
echo "late ticket $1 done $(date +%F_%T)"
