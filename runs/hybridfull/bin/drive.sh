#!/bin/bash
# drive.sh : block m1 (7 iterations = 1 warm + 6 hot, first hot dropped -> 5 kept x 2 halves = 10 hot/arm).
# Anchor queries get T H E EH as well. Resumable; stops starting new queries after 42 min (background limit 60 min).
R=/opt/dlami/nvme/dfperf; B=$R/runs/hybridfull/bin; start=$(date +%s)
MAIN="S X HX HXF EX EHX EHXF"; ANCH="T S H X HX HXF E EH EX EHX EHXF"
for q in 3 5 8 9 16 21 2 12 22 4 7 10 11 17 18 19 20; do
  [ $(( $(date +%s) - start )) -gt 2520 ] && { echo BUDGET; exit 0; }
  case $q in 3|5|8|9|16|21) arms=$ANCH;; *) arms=$MAIN;; esac
  $B/chunk.sh m1 7 "$arms" $q
done
echo ALLDONE
