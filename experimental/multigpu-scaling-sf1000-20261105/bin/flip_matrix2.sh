#!/usr/bin/env bash
# Follow-up flips after batch4g finding: does larger scan batch improve scaling or just speed? + combined best config.
C=/home/ubuntu/sirius-scaling-20261105
cd $C
mk() { n=$1; g=$2; shift 2; python3 bin/make_config.py configs/$n.yaml $C/20-experiments/$n/telemetry $g "$@"; }
mk x_g1_batch4g     0       operator_params.scan_task_batch_size=4GB
mk x_g4_batch8g     0,1,2,3 operator_params.scan_task_batch_size=8GB
mk x_g4_best        0,1,2,3 operator_params.scan_task_batch_size=4GB operator_params.max_build_hash_table_bytes=__DEL__ operator_params.max_broadcast_join_size=8GB
mk x_g1_best        0       operator_params.scan_task_batch_size=4GB operator_params.max_build_hash_table_bytes=__DEL__ operator_params.max_broadcast_join_size=8GB
for n in x_g1_batch4g x_g4_batch8g x_g4_best x_g1_best; do
  $C/bin/gpu_lock_run.sh $C/bin/run_session.sh $C/configs/$n.yaml $C/20-experiments/$n 4 time
  echo "done $n $(date -Is) $(tail -1 $C/20-experiments/$n/meta.txt)"
done
