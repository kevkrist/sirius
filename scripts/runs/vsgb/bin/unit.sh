#!/bin/bash
# Usage: unit.sh  -- run INSIDE one lock hold. DF unit tests on tip binary, prototype binary env off, prototype env on.
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
O=$R/runs/vsgb/unit; mkdir -p $O
TEL=/home/ubuntu/sirius/results/telemetry
before=$(mktemp -p $TMPDIR vsgb.before.XXXX); ls -1 $TEL | sort > $before
run() { # name repo envflag filter
  name=$1; repo=$2; flag=$3; shift 3
  export SIRIUS_TEST_LOG_DIR=$O/logs_$name; mkdir -p $SIRIUS_TEST_LOG_DIR
  unset SIRIUS_PROTO_SGB; [ "$flag" = 1 ] && export SIRIUS_PROTO_SGB=1
  ( cd $repo && pixi run build/release/test/cpp/sirius_unittest "$@" > $O/$name.out 2>&1 )
  echo "UNIT name=$name rc=$? $(date +%T)" | tee -a $O/$name.out
}
run tip_df    $R/repo   0 "[dynamic_filter]~[!benchmark]"
run sgboff_df $R/wt-sgb 0 "[dynamic_filter]~[!benchmark]"
run sgbon_df  $R/wt-sgb 1 "[dynamic_filter]~[!benchmark]"
run sgbon_op  $R/wt-sgb 1 "[partition],[operator],[routing],[exchange]~[!benchmark]~[dynamic_filter]"
run tip_op    $R/repo   0 "[partition],[operator],[routing],[exchange]~[!benchmark]~[dynamic_filter]"
mkdir -p $R/telemetry/vsgb/unit
ls -1 $TEL | sort | comm -13 $before - | while read d; do mv $TEL/$d $R/telemetry/vsgb/unit/; done
rm -f $before
