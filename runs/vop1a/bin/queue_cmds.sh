#!/bin/bash
# Usage: queue_cmds.sh <file of script invocations> ; every line runs as its own waiter: flock $L <line>
R=/opt/dlami/nvme/dfperf; L=$R/machine.lock
n=$(grep -cv '^$' "$1")
grep -v '^$' "$1" | xargs -P "$n" -d '\n' -I{} bash -c "flock $L {}"
echo "queue_cmds $1 done $(date +%T)"
