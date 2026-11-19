#!/bin/bash
# Usage: flock $R/machine.lock ut.sh   -- DF unit tests: tip binary, proto sync, proto async (same Catch2 filter)
set -u
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
TEL=/home/ubuntu/sirius/results/telemetry
out=$R/runs/vop1a/ut; mkdir -p $out
F='[dynamic_filter],[publisher],[publication_claim],[physical_partition],[in_task_publication],[build_filter],[build_probe],[hash_join],[physical_concat]'
before=$(mktemp); ls -1 $TEL | sort > $before
cd $R/repo;    timeout 900 pixi run build/release/test/cpp/sirius_unittest "$F" > $out/tip.log 2>&1;   echo "tip rc=$? $(date +%T)" >> $out/status.txt
cd $R/wt-op1a; SIRIUS_DF_ASYNC_PUBLISH=0 timeout 900 pixi run build/release/test/cpp/sirius_unittest "$F" > $out/proto_sync.log 2>&1;  echo "proto_sync rc=$? $(date +%T)" >> $out/status.txt
cd $R/wt-op1a; timeout 900 pixi run build/release/test/cpp/sirius_unittest "$F" > $out/proto_async.log 2>&1; echo "proto_async rc=$? $(date +%T)" >> $out/status.txt
cd $R/wt-op1a; timeout 900 pixi run build/release/test/cpp/sirius_unittest "$F" > $out/proto_async2.log 2>&1; echo "proto_async2 rc=$? $(date +%T)" >> $out/status.txt
mkdir -p $R/telemetry/vop1a_ut
ls -1 $TEL | sort | comm -13 $before - | while read d; do mv $TEL/$d $R/telemetry/vop1a_ut/; done
rm -f $before
