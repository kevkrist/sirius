#!/bin/bash
# identity gate on permuted / subset GPU sets: roots and contributors land in either pair (run under flock)
B=/opt/dlami/nvme/dfperf/runs/vxfer_host/bin
for spec in "$@"; do   # spec = <visible list>:<G>
  vis=${spec%%:*}; g=${spec##*:}
  UT_TIMEOUT=300 $B/ut_run.sh check_perm_${vis//,/}_g$g $vis "[xfer_proto]" SIRIUS_BENCH_DF_GPUS=$g
done
