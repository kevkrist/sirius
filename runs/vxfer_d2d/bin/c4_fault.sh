#!/bin/bash
# fault-injection binary, multi-GPU DF cases, per mode; XML report for per-case assertion counts
. /opt/dlami/nvme/dfperf/runs/vxfer_d2d/bin/env.sh
export CUDA_VISIBLE_DEVICES=0,1,2,3
T=/home/ubuntu/sirius/results/telemetry
marker=$(mktemp -p $TMPDIR vxferd2d.marker.XXXX)
cd $W
for m in "$@"; do
  SIRIUS_PROTO_XFER_D2D=$m timeout 300 pixi run --frozen build/release/sirius_cuda_fault_test "[cuda_status_fault][multi_gpu]~[.]" -r xml > $X/out/fault_$m.xml 2> $X/out/fault_$m.err
  echo "rc=$? label=fault_$m $(date +%T)" | tee -a $X/out/status.txt
done
new=$(find $T -mindepth 1 -maxdepth 1 -type d -newer $marker); [ -n "$new" ] && { mkdir -p $R/telemetry/vxfer-d2d-ut; mv $new $R/telemetry/vxfer-d2d-ut/; }
rm -f $marker
