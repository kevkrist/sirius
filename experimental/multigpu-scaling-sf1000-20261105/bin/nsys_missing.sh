#!/usr/bin/env bash
# Re-capture queries whose capture range was skipped, with a 20 s gap after each profiler_stop so nsys finishes flushing.
# usage: nsys_missing.sh <name> <gpu_ids> <queries...>
C=/home/ubuntu/sirius-scaling-20261105
n=$1; g=$2; shift 2
cd $C
python3 bin/make_config.py configs/$n.yaml $C/01-nsys/$n/telemetry $g
NSYS_GAP=20 $C/bin/gpu_lock_run.sh $C/bin/run_session.sh $C/configs/$n.yaml $C/01-nsys/$n 2 nsys "$@"
echo "done $n $(date -Is) $(tail -1 $C/01-nsys/$n/meta.txt)"
