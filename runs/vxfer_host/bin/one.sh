#!/bin/bash
# Usage: one.sh <label> <arm> <tree> <qnum> <iterations>  (env passthrough; NSYS=1 -> --precmd nsys). Run under flock.
# Output $X/<label>/<arm>/; Quent dirs this process created -> $R/telemetry/vxfer-host/<label>/<arm>/q<N>/.
set -u
label=$1; arm=$2; tree=$3; q=$4; iters=$5
. /opt/dlami/nvme/dfperf/runs/vxfer_host/bin/env.sh
export SIRIUS_CONFIG_FILE=/home/ubuntu/sirius/specific_config.yaml
DATA=$R/data/tpch_parquet_sf1000
TEL=/home/ubuntu/sirius/results/telemetry
out=$X/$label/$arm; mkdir -p $out
before=$(mktemp); ls -1 $TEL | sort > $before
cd $tree
extra=""; [ "${NSYS:-0}" = 1 ] && extra="--precmd nsys --query-timeout 3600"
nvidia-smi --query-gpu=index,clocks.sm,temperature.gpu,memory.used --format=csv,noheader > $out/q$q.gpu.txt 2>&1
pixi run python test/tpch_performance/performance_test.py \
  --input $DATA --scale-factor 1000 --data-source parquet --engine gpu \
  --config $SIRIUS_CONFIG_FILE --iterations $iters --queries $q \
  --pin host --pin-compression \
  --compression-plan-dir /home/ubuntu/sirius/src/compression/simpatico_codegen/plans/tpch_sf1000 \
  $extra --output $out --name q$q > $out/q$q.stdout 2>&1
rc=$?
mkdir -p $R/telemetry/vxfer-host/$label/$arm/q$q
ls -1 $TEL | sort | comm -13 $before - | while read d; do mv $TEL/$d $R/telemetry/vxfer-host/$label/$arm/q$q/; done
rm -f $before
echo "label=$label arm=$arm q=$q rc=$rc $(date +%T)" >> $X/$label/status.txt
exit $rc
