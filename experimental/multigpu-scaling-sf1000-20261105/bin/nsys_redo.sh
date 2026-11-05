#!/usr/bin/env bash
# Clean nsys re-run (output dir now created): per-query capture of warm iteration 2, g4 then g1.
C=/home/ubuntu/sirius-scaling-20261105
cd $C
for spec in nsys2_g4:0,1,2,3 nsys2_g1:0; do
  n=${spec%%:*}; g=${spec#*:}
  python3 bin/make_config.py configs/$n.yaml $C/01-nsys/$n/telemetry $g
  $C/bin/gpu_lock_run.sh $C/bin/run_session.sh $C/configs/$n.yaml $C/01-nsys/$n 2 nsys
  echo "done $n $(date -Is) $(tail -1 $C/01-nsys/$n/meta.txt)"
done
