#!/bin/bash
# build wt-xfer-host (run under flock). Keeps going past failing translation units so a retry is short.
. /opt/dlami/nvme/dfperf/runs/xfer_host/bin/env.sh
L=$R/logs/build_xfer_host.log
cd $W
echo "=== build start $(date +%T)" >> $L
[ -f build/release/build.ninja ] || pixi run make build/release/build.ninja >> $L 2>&1 || { echo "rc=configure-failed $(date +%T)" >> $L; exit 1; }
pixi run cmake --build --preset release --target sirius_library sirius_unittest -- -k 0 >> $L 2>&1
rc=$?
if [ $rc = 0 ]; then pixi run make >> $L 2>&1; rc=$?; fi
echo "rc=$rc $(date +%T)" >> $L
exit $rc
