#!/bin/bash
# Restore: clone, branches, SF1000 data, build of the OLD tip (pr2570 = ae939824, the reference "tip" of all notes).
# Since 2026-11-28 the PR heads on sirius-db carry the shadow rule (apply/2569 08963d79, apply/2570 317e0cfc);
# the old heads live on the fork as dfperf/backup/*.
set -euxo pipefail
R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache
[ -d $SCCACHE_DIR ] || cp -r ~/.cache/sccache $SCCACHE_DIR
cd $R
[ -d repo ] || git clone --reference /home/ubuntu/sirius --dissociate https://github.com/sirius-db/sirius.git repo
cd repo
git remote add fork git@github.com:kevkrist/sirius.git 2>/dev/null || true
git fetch -q fork '+refs/heads/dfperf/proto/*:refs/heads/proto/*' '+refs/heads/dfperf/mb/*:refs/heads/mb/*' \
  '+refs/heads/dfperf/prod/*:refs/heads/prod/*' '+refs/heads/dfperf/apply/*:refs/heads/apply/*' \
  '+refs/heads/dfperf/verify/*:refs/heads/verify/*' \
  +refs/heads/dfperf/backup/pr2569-a2d6bd5a:refs/heads/pr2569 +refs/heads/dfperf/backup/pr2570-ae939824:refs/heads/pr2570
git rev-parse pr2569 pr2570
git checkout pr2570
git submodule update --init --recursive
( pixi run bash test/tpch_performance/generate_tpch_data.sh 1000 --format parquet --output $R/data/tpch_parquet_sf1000 > $R/logs/gen1000.log 2>&1 ) &
pixi run make > $R/logs/build_tip.log 2>&1
wait
echo SETUP_DONE
