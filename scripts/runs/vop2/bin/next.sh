#!/bin/bash
# next.sh: run as ONE locked command (flock machine.lock next.sh). Pops the first line of todo.txt (priority order) and runs it.
V=/opt/dlami/nvme/dfperf/runs/vop2
line=$(flock $V/todo.lock bash -c 'f='$V'/todo.txt; [ -s $f ] || exit 0; head -1 $f; tail -n +2 $f > $f.tmp && mv $f.tmp $f')
[ -z "$line" ] && exit 0
echo "$(date +%T) START $line" >> $V/done.txt
cd $V && eval "$line" >> $V/queue/next.log 2>&1
echo "$(date +%T) END rc=$? $line" >> $V/done.txt
