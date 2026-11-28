#!/bin/bash
# drive2.sh : block m2 (after the 2026-11-28 wipe; ONE fresh build of 905b9ade). 8 arms on all 17 DF queries,
# 7 iterations = 1 warm + 6 hot, first hot dropped -> 5 kept x 2 halves = 10 hot/arm. Resumable (m2.progress);
# stops starting new queries after 36 min (background limit 60 min).
R=/opt/dlami/nvme/dfperf; B=$R/runs/hybridfull/bin; start=$(date +%s)
ARMS="T S X HX HXF EX EHX EHXF"
for q in 3 5 8 9 16 21 2 12 22 4 7 10 11 17 18 19 20; do
  [ $(( $(date +%s) - start )) -gt 2160 ] && { echo BUDGET; exit 0; }
  $B/chunk.sh m2 7 "$ARMS" $q
done
echo ALLDONE
