#!/bin/bash
# Usage: one.sh <label> <arm> <tree> <qnum> <iterations>  (env passthrough). MUST run under flock $R/machine.lock.
# Output $R/runs/vop1a/<label>/<arm>/; Quent dirs this process created -> $R/telemetry/vop1a_<label>/<arm>/q<N>/.
set -u
label=$1; arm=$2; tree=$3; q=$4; iters=$5
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
export SIRIUS_CONFIG_FILE=/home/ubuntu/sirius/specific_config.yaml
DATA=$R/data/tpch_parquet_sf1000
TEL=/home/ubuntu/sirius/results/telemetry
out=$R/runs/vop1a/$label/$arm; mkdir -p $out $TMPDIR
before=$(mktemp); ls -1 $TEL | sort > $before
cd $tree
nvidia-smi --query-gpu=index,clocks.sm,temperature.gpu,memory.used --format=csv,noheader > $out/q$q.gpu.txt 2>&1
pixi run python test/tpch_performance/performance_test.py \
  --input $DATA --scale-factor 1000 --data-source parquet --engine gpu \
  --config $SIRIUS_CONFIG_FILE --iterations $iters --queries $q \
  --pin host --pin-compression \
  --compression-plan-dir /home/ubuntu/sirius/src/compression/simpatico_codegen/plans/tpch_sf1000 \
  --output $out --name q$q > $out/q$q.stdout 2>&1
rc=$?
mkdir -p $R/telemetry/vop1a_$label/$arm/q$q
ls -1 $TEL | sort | comm -13 $before - | while read d; do mv $TEL/$d $R/telemetry/vop1a_$label/$arm/q$q/; done
rm -f $before
echo "label=$label arm=$arm q=$q rc=$rc $(date +%T)" >> $R/runs/vop1a/$label/status.txt
exit $rc
