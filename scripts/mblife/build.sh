#!/bin/bash
# configure if needed + build sirius_unittest
cd /opt/dlami/nvme/dfperf/wt-mb-life
if [ ! -f build/release/build.ninja ]; then pixi run make build/release/build.ninja || exit 1; fi
pixi run cmake --build --preset release --target sirius_unittest
