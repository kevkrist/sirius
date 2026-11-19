#!/bin/bash
# usage: run_cases.sh <cases-file> <out-file>   (run under flock machine.lock)
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache TMPDIR=$R/tmp
WT=$R/wt-op4; BIN=$WT/build/release/test/bench/df_bloom_bench
TEL=/home/ubuntu/sirius/results/telemetry
marker=$(mktemp -p $TMPDIR telmark.XXXX)
cd $WT
export CUDA_VISIBLE_DEVICES=0
echo "# $(date -Is) $(nvidia-smi --query-gpu=index,clocks.sm,temperature.gpu,memory.used --format=csv,noheader -i 0)" >> $2
while IFS= read -r line; do
  [ -z "$line" ] && continue
  case "$line" in \#*) continue;; esac
  pixi run $BIN $line >> $2 2>> $2.err || echo "FAIL,$line" >> $2
done < $1
new=$(find $TEL -mindepth 1 -maxdepth 1 -type d -newer $marker 2>/dev/null)
if [ -n "$new" ]; then echo "# telemetry created during run (not moved; binary loads no config): $new" >> $2; fi
rm -f $marker
