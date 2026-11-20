#!/bin/bash
# Usage: shadow_test.sh <worktree-name>  -- the Consumer agreement tests with every assertion printed (-s), to read the
# trial counts of the randomized case. Run under the machine lock.
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
export SIMPATICO_JIT_CACHE_DIR=$R/cache/simpatico_jit TMPDIR=$R/tmp
wt=$1; out=$R/runs/shadow_fix/unit/$wt; mkdir -p $out
cd $R/$wt
/usr/bin/time -f "elapsed=%es" -o $out/shadow_s.time env CUDA_VISIBLE_DEVICES=0 pixi run build/release/test/cpp/sirius_unittest "[scan_merge][shadow]" -s -d yes > $out/shadow_s.txt 2>&1
rc=$?
echo "$wt shadow_s rc=$rc $(date +%T) $(git rev-parse --short HEAD) $(tail -2 $out/shadow_s.txt | tr '\n' ' ') $(cat $out/shadow_s.time)" >> $R/runs/shadow_fix/unit/status.txt
exit $rc
