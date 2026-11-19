#!/bin/bash
# waitev.sh <seconds> : block until the set of started/finished verifier jobs changes (or the deadline). No GPU use.
cd /opt/dlami/nvme/dfperf/runs/vsgb/logs
sig() { for f in *.out; do [ -s "$f" ] && echo "$f $(grep -c -E '^END|UNIT name|^rc=' "$f")"; done; }
s0=$(sig); end=$(( $(date +%s) + $1 ))
while [ $(date +%s) -lt $end ]; do
  s1=$(sig); [ "$s1" != "$s0" ] && break
  read -t 15 -r _ < <(tail -f /dev/null) || true
done
date; sig | tr '\n' ';'; echo
echo "waiters: $(pgrep -af 'flock /opt/dlami/nvme/dfperf/machine.lock' | grep -v 'bash -c' | wc -l) mine: $(pgrep -af 'flock /opt/dlami/nvme/dfperf/machine.lock' | grep -v 'bash -c' | grep -c vsgb)"
