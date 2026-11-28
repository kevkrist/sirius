#!/bin/bash
# TRACE pass of the shapes (no interrupts) for per-operator row comparison: arms tip, f1, s2g0
cd /opt/dlami/nvme/dfperf/runs/veager
for a in tip f1 s2g0; do
  SUFFIX=_tr SIRIUS_LOG_LEVEL=trace flock /opt/dlami/nvme/dfperf/machine.lock bin/shapes.sh $a nointerrupt; echo "$a rc=$? $(date +%T)"
done
