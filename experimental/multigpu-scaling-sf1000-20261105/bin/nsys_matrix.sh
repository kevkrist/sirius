#!/usr/bin/env bash
# nsys g4 + g1 sessions (iter1 warm-up uncaptured, iter2 captured per query), then Quent-off g4 control with dmon sampling.
C=/home/ubuntu/sirius-scaling-20261105
cd $C
python3 bin/make_config.py configs/nsys_g4.yaml $C/01-nsys/nsys_g4/telemetry 0,1,2,3
python3 bin/make_config.py configs/nsys_g1.yaml $C/01-nsys/nsys_g1/telemetry 0
python3 bin/make_config.py configs/g4_noquent.yaml $C/00-baseline/g4_noquent/telemetry 0,1,2,3 telemetry.enable_quent=false
for n in nsys_g4 nsys_g1; do
  $C/bin/gpu_lock_run.sh $C/bin/run_session.sh $C/configs/$n.yaml $C/01-nsys/$n 2 nsys
  echo "done $n $(date -Is) $(tail -1 $C/01-nsys/$n/meta.txt)"
done
$C/bin/gpu_lock_run.sh $C/bin/run_with_dmon.sh $C/configs/g4_noquent.yaml $C/00-baseline/g4_noquent
echo "done g4_noquent $(date -Is)"
