#!/bin/bash
# md5.sh <tag>...: result.txt md5 per query across arms (must be one distinct value per query).
cd /opt/dlami/nvme/dfperf/runs/xfer_compose
for q in 21 3 7 8 10 9 6 2; do
  n=$(for t in "$@"; do md5sum ${t}_*/tpch_*_q$q/sirius/q$q/result.txt 2>/dev/null; done | awk '{print $1}' | sort | uniq -c | wc -l)
  c=$(for t in "$@"; do ls ${t}_*/tpch_*_q$q/sirius/q$q/result.txt 2>/dev/null; done | wc -l)
  echo "q$q files=$c distinct_md5=$n"
done
