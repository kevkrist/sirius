#!/bin/bash
# usage: build.sh <target...>
source /opt/dlami/nvme/dfperf/runs/mb_bloom/env.sh
cd $WT
set -e
if [ ! -f build/release/build.ninja ] || [ "$RECONF" = 1 ]; then
  pixi run cmake --preset release -DSIRIUS_DUCKDB_SOURCE_DIR="$WT/duckdb" -DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache -DCMAKE_CUDA_COMPILER_LAUNCHER=sccache -DCMAKE_LINKER_TYPE=MOLD
fi
pixi run cmake --build --preset release --target "$@"
