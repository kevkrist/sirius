#!/usr/bin/env bash
# run_with_dmon.sh <config> <outdir> [iters=4]: un-profiled session with 1 s nvidia-smi dmon sampling (power/util/clocks/PCIe rx,tx).
CFG=$1; OUT=$2; IT=${3:-4}
mkdir -p "$OUT"
nvidia-smi dmon -s putc -d 1 -o T > "$OUT/dmon.txt" &
D=$!
/home/ubuntu/sirius-scaling-20261105/bin/run_session.sh "$CFG" "$OUT" "$IT" time
kill $D
