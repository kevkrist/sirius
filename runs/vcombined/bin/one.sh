#!/bin/bash
# Verifier's runner. Usage (under machine lock): one.sh <arm> <half> <q> <iters>
# arm: T = tip binary $R/repo; O/S/X/E/SX/SE/XE/SXE = $R/wt-combined binary + env switches. Output $R/runs/vcombined/<arm>_<half>/
set -u
arm=$1; half=$2; q=$3; iters=$4
export R=/opt/dlami/nvme/dfperf
unset SIRIUS_PROTO_XFER_D2D SIRIUS_EAGER_PROBE SIRIUS_PROTO_SHADOW_OFF SIRIUS_EAGER_CONCAT_FIRST SIRIUS_PRE_SQL SIRIUS_LOG_LEVEL SIRIUS_ENABLE_TEST_OPTIONS SIRIUS_BUILD_PATH
if [ $arm = T ]; then REPO=$R/repo; else
  REPO=$R/wt-combined
  case $arm in *S*) ;; *) export SIRIUS_PROTO_SHADOW_OFF=1;; esac
  case $arm in *X*) export SIRIUS_PROTO_XFER_D2D=pipe;; esac
  case $arm in *E*) export SIRIUS_EAGER_PROBE=1;; esac
fi
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
export SIRIUS_CONFIG_FILE=/home/ubuntu/sirius/specific_config.yaml
TEL=/home/ubuntu/sirius/results/telemetry
out=$R/runs/vcombined/${arm}_$half; mkdir -p $out $TMPDIR
before=$(mktemp); ls -1 $TEL | sort > $before
cd $REPO
{ echo "repo=$REPO head=$(git rev-parse --short HEAD)"; env | grep -E '^SIRIUS_' | sort; } > $out/q$q.env.txt
nvidia-smi --query-gpu=index,clocks.sm,temperature.gpu,memory.used --format=csv,noheader > $out/q$q.gpu.txt 2>&1
pixi run python test/tpch_performance/performance_test.py \
  --input $R/data/tpch_parquet_sf1000 --scale-factor 1000 --data-source parquet --engine gpu \
  --config $SIRIUS_CONFIG_FILE --iterations $iters --queries $q \
  --pin host --pin-compression \
  --compression-plan-dir /home/ubuntu/sirius/src/compression/simpatico_codegen/plans/tpch_sf1000 \
  --output $out --name q$q > $out/q$q.stdout 2>&1
rc=$?
mkdir -p $R/telemetry/vcombined_${arm}_$half/q$q
ls -1 $TEL | sort | comm -13 $before - | while read d; do mv $TEL/$d $R/telemetry/vcombined_${arm}_$half/q$q/; done
rm -f $before
echo "arm=$arm half=$half q=$q rc=$rc $(date +%T)" >> $R/runs/vcombined/status.txt
exit $rc
