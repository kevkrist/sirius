#!/bin/bash
# fault.sh: CUDA fault-injection binary, multi-GPU DF cases, switch off and pipe. Under the machine lock.
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
export CUDA_VISIBLE_DEVICES=0,1,2,3
T=/home/ubuntu/sirius/results/telemetry
marker=$(mktemp -p $TMPDIR xferd2d.marker.XXXX)
cd $R/wt-xfer-d2d
for m in off pipe; do
  SIRIUS_PROTO_XFER_D2D=$m timeout 600 pixi run build/release/test/cpp/sirius_cuda_fault_test "[cuda_status_fault][multi_gpu]~[.]" > $R/runs/xfer_d2d/mb/fault_$m.log 2>&1
  echo "rc=$? label=fault_$m $(date +%T)" >> $R/runs/xfer_d2d/mb/status.txt
done
if [ -d $T ]; then
  new=$(find $T -mindepth 1 -maxdepth 1 -type d -newer $marker)
  if [ -n "$new" ]; then mkdir -p $R/telemetry/xfer_d2d_mb; mv $new $R/telemetry/xfer_d2d_mb/; fi
fi
rm -f $marker
