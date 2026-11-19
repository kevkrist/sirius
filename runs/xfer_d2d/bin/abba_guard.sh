#!/bin/bash
# abba_guard.sh <tag> <iters> <q...>: ABBA block only when the identity gate passed.
X=/opt/dlami/nvme/dfperf/runs/xfer_d2d
[ -e $X/GATE_OK ] || { echo "abba: gate not passed, skipped $*"; exit 1; }
exec $X/bin/abba_q.sh "$@"
