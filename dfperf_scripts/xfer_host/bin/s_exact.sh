#!/bin/bash
# incremental build + exact-kind identity gate at G=4 (run under flock)
B=/opt/dlami/nvme/dfperf/runs/xfer_host/bin; X=/opt/dlami/nvme/dfperf/runs/xfer_host
md5sum /opt/dlami/nvme/dfperf/wt-xfer-host/sirius-duckdb/build/release/extension/sirius/sirius.duckdb_extension >> $X/out/ext_md5.txt
$B/build_only.sh || exit 1
md5sum /opt/dlami/nvme/dfperf/wt-xfer-host/sirius-duckdb/build/release/extension/sirius/sirius.duckdb_extension >> $X/out/ext_md5.txt
UT_TIMEOUT=400 $B/ut_run.sh exact_g4 0,1,2,3 "[xfer_proto_exact]" SIRIUS_BENCH_DF_GPUS=4
