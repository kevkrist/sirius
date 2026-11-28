#!/bin/bash
# Usage: one_query.sh <label> <qnum> <iterations>   (env: REPO, NSYS=1, SIRIUS_LOG_LEVEL)
# MUST run under flock /opt/dlami/nvme/dfperf/machine.lock (directly or through a wrapping script).
set -u
label=$1; q=$2; iters=$3
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
export SIRIUS_CONFIG_FILE=/home/ubuntu/sirius/specific_config.yaml
REPO=${REPO:-$R/wt-prod-xfer}
DATA=$R/data/tpch_parquet_sf1000; SF=1000
TEL=/home/ubuntu/sirius/results/telemetry
out=$R/runs/xfer_prod/$label; mkdir -p $out $TMPDIR
before=$(mktemp); ls -1 $TEL | sort > $before
cd $REPO
nvidia-smi --query-gpu=index,clocks.sm,temperature.gpu,memory.used --format=csv,noheader > $out/q$q.gpu.txt 2>&1
extra=""; [ "${NSYS:-0}" = 1 ] && extra="--precmd nsys --query-timeout 3600"
pixi run python test/tpch_performance/performance_test.py \
  --input $DATA --scale-factor $SF --data-source parquet --engine gpu \
  --config $SIRIUS_CONFIG_FILE --iterations $iters --queries $q \
  --pin host --pin-compression \
  --compression-plan-dir /home/ubuntu/sirius/src/compression/simpatico_codegen/plans/tpch_sf1000 \
  $extra --output $out --name q$q > $out/q$q.stdout 2>&1
rc=$?
mkdir -p $R/telemetry/xfer_prod_$label/q$q
ls -1 $TEL | sort | comm -13 $before - | while read d; do mv $TEL/$d $R/telemetry/xfer_prod_$label/q$q/; done
rm -f $before
echo "label=$label q=$q rc=$rc $(date +%T)" >> $out/status.txt
exit $rc
