#!/usr/bin/env bash
C=/home/ubuntu/sirius-scaling-20261105
for n in g4_a g1_a g2s_a g2x_a g4_b g1_b; do
  $C/bin/gpu_lock_run.sh $C/bin/run_session.sh $C/configs/$n.yaml $C/00-baseline/$n 4 time
  echo "done $n $(date -Is) $(tail -1 $C/00-baseline/$n/meta.txt)"
done
