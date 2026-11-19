#!/bin/bash
cd /opt/dlami/nvme/dfperf
( time flock /opt/dlami/nvme/dfperf/machine.lock runs/mb_bloom/build.sh "$@" ) > runs/mb_bloom/build_bench.log 2>&1
echo BUILD_EXIT=$? >> runs/mb_bloom/build_bench.log
