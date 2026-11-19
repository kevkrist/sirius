#!/bin/bash
# Run under flock $R/machine.lock: pops the first line of $X/queue.txt and runs it. One hold = one queued command.
# A line starting with "GATE " runs only once $X/gate.ok exists (stage 1 passed).
# Exit 75 = nothing runnable (queue empty or gate closed): the hold is released at once.
X=/opt/dlami/nvme/dfperf/runs/xfer_host
cmd=$( (flock 9
        line=$(head -n 1 $X/queue.txt 2>/dev/null)
        case "$line" in
          "GATE "*) [ -f $X/gate.ok ] || exit 0; line=${line#GATE };;
        esac
        [ -n "$line" ] && sed -i '1d' $X/queue.txt
        echo "$line") 9>$X/queue.lock )
[ -n "$cmd" ] || exit 75
echo "START $(date +%T) $cmd" >> $X/queue.log
bash -c "$cmd"; rc=$?
echo "END   $(date +%T) rc=$rc $cmd" >> $X/queue.log
exit 0
