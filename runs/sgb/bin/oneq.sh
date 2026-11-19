#!/bin/bash
# Usage: oneq.sh <side> <label> <q> <iters>   side = tip | base | sgb | sgboff      (run under flock machine.lock)
# Fresh process, host pin + pin compression. Output $R/runs/sgb/<label>/, Quent -> $R/telemetry/sgb/<label>/q<N>/.
set -u
side=$1; label=$2; q=$3; iters=$4
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
export SIRIUS_CONFIG_FILE=/home/ubuntu/sirius/specific_config.yaml
case $side in
  tip) repo=$R/repo;; base) repo=$R/wt-base;;
  sgb) repo=$R/wt-sgb; export SIRIUS_PROTO_SGB=1;;
  sgboff) repo=$R/wt-sgb;;
  *) echo bad side; exit 2;;
esac
DATA=$R/data/tpch_parquet_sf1000
TEL=/home/ubuntu/sirius/results/telemetry
out=$R/runs/sgb/$label; mkdir -p $out $TMPDIR
before=$(mktemp); ls -1 $TEL | sort > $before
cd $repo
${WRAP:-} pixi run python test/tpch_performance/performance_test.py \
  --input $DATA --scale-factor 1000 --data-source parquet --engine gpu \
  --config $SIRIUS_CONFIG_FILE --iterations $iters --queries $q \
  --pin host --pin-compression \
  --compression-plan-dir /home/ubuntu/sirius/src/compression/simpatico_codegen/plans/tpch_sf1000 \
  ${EXTRA:-} --output $out --name q$q > $out/q$q.stdout 2>&1
rc=$?
mkdir -p $R/telemetry/sgb/$label/q$q
ls -1 $TEL | sort | comm -13 $before - | while read d; do mv $TEL/$d $R/telemetry/sgb/$label/q$q/; done
rm -f $before
echo "side=$side label=$label q=$q rc=$rc $(date +%T)" >> $out/status.txt
exit $rc
