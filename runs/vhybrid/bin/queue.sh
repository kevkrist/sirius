#!/bin/bash
# Verification queue: waits for the wt-vhybrid build, then ABBA blocks (lock taken per half block inside abba.sh).
R=/opt/dlami/nvme/dfperf; V=$R/runs/vhybrid
until [ -f $V/build.status ]; do sleep 15; done
grep -q "rc=0" $V/build.status || { echo BUILDFAIL >> $V/v1.progress; exit 1; }
$V/bin/abba.sh v1 7 "T H E EH X" 5 2 16 8 3 9 21 12 22
$V/bin/abba.sh v2 7 "T H E EH" 7 10 11 18 19 20
$V/bin/abba.sh v3 7 "EH EHV" 5 2
