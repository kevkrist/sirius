#!/bin/bash
# Usage: flock $R/machine.lock logpass.sh <q>   -- untimed TRACE log pass, tip (A1) then proto async (B1), 2 iterations each
q=$1
R=/opt/dlami/nvme/dfperf
SIRIUS_LOG_LEVEL=trace $R/runs/vop1a/bin/pair.sh trace $q 2 A1 B1
