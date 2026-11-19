#!/bin/bash
# Usage: logpass.sh <arm> <label> <q> [<q> ...] -- TRACE pass, 2 iterations per query; run INSIDE one lock hold.
arm=$1; label=$2; shift 2
B=/opt/dlami/nvme/dfperf/runs/vsgb/bin
echo "START logpass $arm $label $* $(date +%F_%T)"
for q in "$@"; do SIRIUS_LOG_LEVEL=trace $B/oneq.sh $arm $label $q 2; echo "  q=$q rc=$? $(date +%T)"; done
echo "END $(date +%F_%T)"
