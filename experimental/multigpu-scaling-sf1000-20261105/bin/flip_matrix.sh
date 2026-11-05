#!/usr/bin/env bash
# Config-flip experiments (un-profiled, Quent on, 1 warm-up + 3 hot). Each waits on the GPU lock.
C=/home/ubuntu/sirius-scaling-20261105
cd $C; mkdir -p 20-experiments
mk() { n=$1; g=$2; shift 2; python3 bin/make_config.py configs/$n.yaml $C/20-experiments/$n/telemetry $g "$@"; }
mk x_g4_joinpol 0,1,2,3 operator_params.max_build_hash_table_bytes=__DEL__ operator_params.max_broadcast_join_size=8GB
mk x_g1_joinpol 0       operator_params.max_build_hash_table_bytes=__DEL__ operator_params.max_broadcast_join_size=8GB
mk x_g4_hpb256  0,1,2,3 operator_params.hash_partition_bytes=256MB
mk x_g4_hpb4g   0,1,2,3 operator_params.hash_partition_bytes=4GB operator_params.concat_batch_bytes=4GB
mk x_g4_batch4g 0,1,2,3 operator_params.scan_task_batch_size=4GB
mk x_g4_thr8    0,1,2,3 executor.pipeline.num_threads=8
mk x_g1_thr8    0       executor.pipeline.num_threads=8
mk x_g4_gpupin  0,1,2,3
for n in x_g4_joinpol x_g1_joinpol x_g4_hpb256 x_g4_hpb4g x_g4_batch4g x_g4_thr8 x_g1_thr8; do
  $C/bin/gpu_lock_run.sh $C/bin/run_session.sh $C/configs/$n.yaml $C/20-experiments/$n 4 time
  echo "done $n $(date -Is) $(tail -1 $C/20-experiments/$n/meta.txt)"
done
PIN_TIER=gpu $C/bin/gpu_lock_run.sh $C/bin/run_session.sh $C/configs/x_g4_gpupin.yaml $C/20-experiments/x_g4_gpupin 4 time
echo "done x_g4_gpupin $(date -Is) $(tail -1 $C/20-experiments/x_g4_gpupin/meta.txt)"
