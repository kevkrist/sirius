#!/bin/bash
R=/opt/dlami/nvme/dfperf
for cfg in "g1 0 1 bloom" "g1 0 1 auto" "g2 0,1 2 bloom" "g2 0,1 2 auto"; do
  set -- $cfg
  for B in ${BLIST:-1 512}; do for reps in 1 5; do
    NSYS=1 $R/mblife/run.sh ns_$1_$4_b${B}_r${reps} $2 "[mb2]" SIRIUS_BENCH_DF_GPUS=$3 SIRIUS_BENCH_DF_KINDS=$4 \
      SIRIUS_BENCH_DF_KEYS=1 SIRIUS_BENCH_DF_ROWS=32 SIRIUS_BENCH_DF_BATCHES=$B SIRIUS_BENCH_DF_REPS=$reps
  done; done
done
