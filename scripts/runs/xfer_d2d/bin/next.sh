#!/bin/bash
# Runs under the machine lock: pops the first line of todo.txt and runs it. rc 3 = nothing to do.
R=/opt/dlami/nvme/dfperf; X=$R/runs/xfer_d2d
line=$(flock $X/todo.lock bash -c "head -n1 $X/todo.txt 2>/dev/null; sed -i '1d' $X/todo.txt 2>/dev/null")
[ -z "$line" ] && exit 3
echo "$(date +%T) START $line" >> $X/done.txt
bash -c "$line" >> $X/next.log 2>&1
echo "$(date +%T) END rc=$? $line" >> $X/done.txt
exit 0
