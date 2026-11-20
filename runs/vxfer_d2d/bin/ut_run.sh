#!/bin/bash
# usage: ut_run.sh <label> <visible gpus> <catch filter> [VAR=VALUE ...]   (run under flock; NSYS=1 -> nsys; UTBIN=<binary> overrides)
. /opt/dlami/nvme/dfperf/runs/vxfer_d2d/bin/env.sh
label=$1; gpus=$2; filter=$3; shift 3
for kv in "$@"; do export "$kv"; done
[ -n "${UTBIN:-}" ] && UT=$UTBIN
export CUDA_VISIBLE_DEVICES=$gpus
T=/home/ubuntu/sirius/results/telemetry
marker=$(mktemp -p $TMPDIR vxferd2d.marker.XXXX)
cd $W
if [ "${NSYS:-0}" = 1 ]; then
  mkdir -p $X/nsys
  timeout ${UT_TIMEOUT:-900} pixi run --frozen nsys profile --trace=cuda,nvtx --sample=none --cudabacktrace=none --force-overwrite=true \
    --export=sqlite -o $X/nsys/$label $UT "$filter" > $X/out/$label.log 2>&1
else
  timeout ${UT_TIMEOUT:-900} pixi run --frozen $UT "$filter" > $X/out/$label.log 2>&1
fi
rc=$?
if [ -d $T ]; then
  new=$(find $T -mindepth 1 -maxdepth 1 -type d -newer $marker)
  if [ -n "$new" ]; then mkdir -p $R/telemetry/vxfer-d2d-ut; mv $new $R/telemetry/vxfer-d2d-ut/; fi
fi
rm -f $marker
echo "rc=$rc label=$label $(date +%T)" | tee -a $X/out/status.txt
exit $rc
