#!/bin/bash
# Reviewer probe harness: compiles one scratch TU with wt-prod-70's flags and links a scratch unittest binary.
# Reads wt-prod-70's build tree, writes only under $P. Run under the machine lock.
set -e
export R=/opt/dlami/nvme/dfperf
export PIXI_CACHE_DIR=$R/cache/pixi RATTLER_CACHE_DIR=$R/cache/rattler SCCACHE_DIR=$R/cache/sccache TMPDIR=$R/tmp
P=$R/runs/review_shadow/probe; WT=$R/wt-prod-70; cd $WT/build/release
OBJ=CMakeFiles/sirius_unittest.dir/test/cpp/scan/test_dynamic_filter_merge.cpp.o
python3 - <<PY > $P/cc.sh
import json
for e in json.load(open('compile_commands.json')):
    if e['file'].endswith('test/cpp/scan/test_dynamic_filter_merge.cpp'):
        c = e['command'].replace('-o $OBJ', '-o $P/rv_probe_merge.o').replace('-c ' + e['file'], '-I$WT/test/cpp/scan -c $P/rv_probe_merge.cpp')
        print(c.replace('sccache ', ''))
PY
pixi run --manifest-path $WT ninja -C $WT/build/release -t commands test/cpp/sirius_unittest | tail -1 \
  | sed "s#$OBJ#$P/rv_probe_merge.o#; s#-o test/cpp/sirius_unittest#-o $P/rv_unittest#" > $P/link.sh
grep -c rv_probe_merge.o $P/cc.sh $P/link.sh; grep -o -- "-o [^ ]*rv_unittest" $P/link.sh
( cd $WT/build/release && pixi run --manifest-path $WT bash $P/cc.sh ) 2>&1 | tail -15
( cd $WT/build/release && pixi run --manifest-path $WT bash $P/link.sh ) 2>&1 | tail -15
ls -la $P/rv_unittest
