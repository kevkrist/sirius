#!/bin/bash
# One lock waiter: keeps taking the machine lock for the next queued command until $X/stop exists.
X=/opt/dlami/nvme/dfperf/runs/xfer_host
while [ ! -f $X/stop ]; do
  flock /opt/dlami/nvme/dfperf/machine.lock $X/bin/next.sh
  [ $? = 75 ] && sleep 30
done
