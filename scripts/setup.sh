#!/bin/bash
set -euxo pipefail
R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
[ -d $SCCACHE_DIR ] || cp -r ~/.cache/sccache $SCCACHE_DIR
cd $R
[ -d repo ] || git clone --reference /home/ubuntu/sirius --dissociate https://github.com/sirius-db/sirius.git repo
cd repo
git fetch origin pull/2569/head:pr2569 pull/2570/head:pr2570
git rev-parse pr2569 pr2570
git checkout pr2570
git submodule update --init --recursive
( pixi run bash test/tpch_performance/generate_tpch_data.sh 1000 --format parquet --output $R/data/tpch_parquet_sf1000 > $R/logs/gen1000.log 2>&1 ) &
pixi run make > $R/logs/build_tip.log 2>&1
wait
echo SETUP_DONE
