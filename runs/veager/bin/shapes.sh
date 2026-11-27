#!/bin/bash
# Usage (under the machine lock): shapes.sh <arm> [extra shapes.py args]   arm = tip | f1 | fk1 | s2g1 | o1 | cpu
set -u
arm=$1; shift
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
export SIRIUS_CONFIG_FILE=/home/ubuntu/sirius/specific_config.yaml
export DATA=$R/data/tpch_parquet_sf1000
V=$R/runs/veager; out=$V/shapes/$arm; mkdir -p $out/log
export SIRIUS_LOG_DIR=$out/log SIRIUS_LOG_LEVEL=${SIRIUS_LOG_LEVEL:-debug}
TEL=/home/ubuntu/sirius/results/telemetry
before=$(mktemp); ls -1 $TEL | sort > $before
REPO=$R/wt-eager2; extra=
case $arm in
  tip) REPO=$R/repo ;;
  cpu) REPO=$R/repo; extra=cpu ;;
  o1)  export SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=1 ;;
  f1)  export SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=2 SIRIUS_EAGER_GATE=2 SIRIUS_EAGER_ADMIT_GIB=16 ;;
  fk1) export SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=2 SIRIUS_EAGER_GATE=2 SIRIUS_EAGER_ADMIT_GIB=16 SIRIUS_EAGER_SCAN_THROTTLE=1 ;;
  s2g1) export SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=2 SIRIUS_EAGER_GATE=1 ;;
  s2g0) export SIRIUS_DF_DEFER_PUBLISH=0 SIRIUS_EAGER_PROBE=2 SIRIUS_EAGER_GATE=0 ;;
  *) echo "unknown arm"; exit 9 ;;
esac
cd $REPO
pixi run python $V/bin/shapes.py $out/results.jsonl ${TIMEOUT:-240} $extra "$@" > $out/stdout.txt 2>&1
rc=$?
mkdir -p $R/telemetry/veager_shapes_$arm
ls -1 $TEL | sort | comm -13 $before - | while read d; do mv $TEL/$d $R/telemetry/veager_shapes_$arm/; done
rm -f $before
echo "arm=$arm rc=$rc $(date +%T)" >> $out/status.txt
exit $rc
