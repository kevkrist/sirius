#!/bin/bash
# vx_build.sh: compile the verifier's test file out of tree with the prototype's compile flags and link it with the prototype's
# (unchanged) test objects into $X/vxbin/sirius_unittest_vx. Reads wt-xfer-d2d, writes nothing there. Run under flock.
. /opt/dlami/nvme/dfperf/runs/vxfer_d2d/bin/env.sh
mkdir -p $X/vxbin
cd $W/build/release
python3 - <<'PY' > $X/vxbin/compile.sh
import json, os
X = os.environ['X']
cc = json.load(open('compile_commands.json'))
e = [e for e in cc if e['file'].endswith('bench_dynamic_filter_lifecycle.cpp')][0]
cmd = e['command']
obj = 'CMakeFiles/sirius_unittest.dir/test/cpp/operator/bench_dynamic_filter_lifecycle.cpp.o'
assert obj in cmd and e['file'] in cmd
cmd = cmd.replace('-o ' + obj, '-o ' + X + '/vxbin/vx_tests.o').replace('-c ' + e['file'], '-c ' + X + '/src/vx_tests.cpp')
print('cd ' + e['directory'] + ' && ' + cmd)
link = open(X + '/src/link_cmd.txt').read().strip()
assert '-o test/cpp/sirius_unittest ' in link
link = link.replace('-o test/cpp/sirius_unittest ', '-o ' + X + '/vxbin/sirius_unittest_vx ' + X + '/vxbin/vx_tests.o ')
print('cd ' + e['directory'] + ' && ' + link)
PY
cd $W && pixi run --frozen bash $X/vxbin/compile.sh > $X/vxbin/build.log 2>&1
echo "rc=$? $(date +%T)" >> $X/vxbin/build.log
tail -3 $X/vxbin/build.log | cut -c1-300
