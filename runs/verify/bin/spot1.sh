#!/bin/bash
# Spot A/B blocks, one lock acquisition per run (ABBA order).
R=/opt/dlami/nvme/dfperf; B=$R/runs/verify/bin; L="flock $R/machine.lock"
IT=6
# q16: base vs tip
REPO=$R/wt-base $L $B/vq.sh base1 16 $IT
REPO=$R/repo    $L $B/vq.sh tip1 16 $IT
REPO=$R/repo    $L $B/vq.sh tip2 16 $IT
REPO=$R/wt-base $L $B/vq.sh base2 16 $IT
# q2: tip DF on vs off, both with SIRIUS_ENABLE_TEST_OPTIONS=1 (like for like)
export SIRIUS_ENABLE_TEST_OPTIONS=1
SIRIUS_PRE_SQL="SET enable_dynamic_filter = true;"  $L $B/vq.sh on1 2 $IT
SIRIUS_PRE_SQL="SET enable_dynamic_filter = false;" $L $B/vq.sh off1 2 $IT
SIRIUS_PRE_SQL="SET enable_dynamic_filter = false;" $L $B/vq.sh off2 2 $IT
SIRIUS_PRE_SQL="SET enable_dynamic_filter = true;"  $L $B/vq.sh on2 2 $IT
# q16 tip DF on vs off, like for like
SIRIUS_PRE_SQL="SET enable_dynamic_filter = true;"  $L $B/vq.sh on1 16 $IT
SIRIUS_PRE_SQL="SET enable_dynamic_filter = false;" $L $B/vq.sh off1 16 $IT
SIRIUS_PRE_SQL="SET enable_dynamic_filter = false;" $L $B/vq.sh off2 16 $IT
SIRIUS_PRE_SQL="SET enable_dynamic_filter = true;"  $L $B/vq.sh on2 16 $IT
echo SPOT1_DONE
