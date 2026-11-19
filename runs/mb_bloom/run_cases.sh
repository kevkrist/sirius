#!/bin/bash
# usage: run_cases.sh <cases-file> <out-file>   (run under flock machine.lock)
# Each line of the cases file is the argument list of one df_bloom_bench process.
source /opt/dlami/nvme/dfperf/runs/mb_bloom/env.sh
BIN=$WT/build/release/test/bench/df_bloom_bench
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
# move telemetry sessions this run created (none expected: the binary loads no Sirius config)
if [ -d $TEL ]; then
  new=$(find $TEL -mindepth 1 -maxdepth 1 -type d -newer $marker)
  if [ -n "$new" ]; then mkdir -p $R/telemetry/mb_bloom; mv $new $R/telemetry/mb_bloom/; echo "# moved telemetry: $new" >> $2; fi
fi
rm -f $marker
