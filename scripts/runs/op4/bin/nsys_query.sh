#!/bin/bash
# Usage: one_query.sh <label> <qnum> <iterations>   env: REPO (default $R/repo), SIRIUS_PRE_SQL, SIRIUS_LOG_LEVEL, PROTO_* env
# MUST be run under flock /opt/dlami/nvme/dfperf/machine.lock
set -u
label=$1; q=$2; iters=$3
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
export SIRIUS_CONFIG_FILE=/home/ubuntu/sirius/specific_config.yaml
REPO=${REPO:-$R/repo}
DATA=$R/data/tpch_parquet_sf1000
TEL=/home/ubuntu/sirius/results/telemetry
out=$R/runs/op4/$label; mkdir -p $out $TMPDIR
before=$(mktemp); ls -1 $TEL | sort > $before
cd $REPO
nvidia-smi --query-gpu=index,clocks.sm,temperature.gpu,memory.used --format=csv,noheader > $out/q$q.gpu.txt 2>&1
pixi run python test/tpch_performance/performance_test.py \
  --input $DATA --scale-factor 1000 --data-source parquet --engine gpu \
  --config $SIRIUS_CONFIG_FILE --iterations $iters --queries $q \
  --pin host --pin-compression \
  --compression-plan-dir /home/ubuntu/sirius/src/compression/simpatico_codegen/plans/tpch_sf1000 \
  --precmd nsys --query-timeout 3600 --output $out --name q$q > $out/q$q.stdout 2>&1
rc=$?
mkdir -p $R/telemetry/op4/$label/q$q
ls -1 $TEL | sort | comm -13 $before - | while read d; do mv $TEL/$d $R/telemetry/op4/$label/q$q/; done
rm -f $before
echo "label=$label q=$q rc=$rc repo=$REPO pre='${SIRIUS_PRE_SQL:-}' $(date +%T)" >> $out/status.txt
exit $rc
