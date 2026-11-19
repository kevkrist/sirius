#!/bin/bash
# One queue presence on the machine lock; an idle acquisition releases at once.
R=/opt/dlami/nvme/dfperf; X=$R/runs/xfer_d2d
while [ ! -e $X/STOP ]; do
  flock $R/machine.lock $X/bin/next.sh
  [ $? = 3 ] && sleep 30
done
