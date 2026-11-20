#!/bin/bash
# Blocks up to 9.5 minutes until any chain log grows, then prints the last line of each.
X=/opt/dlami/nvme/dfperf/runs/shadow_fix
n0=$(cat $X/chain_final.log $X/chain_ut_wt-prod-69.log $X/chain_mut.log 2>/dev/null | grep -c '^done\|ALL')
for i in $(seq 1 57); do [ $(cat $X/chain_final.log $X/chain_ut_wt-prod-69.log $X/chain_mut.log 2>/dev/null | grep -c '^done\|ALL') -gt $n0 ] && break; sleep 10; done
for f in chain_final chain_ut_wt-prod-69 chain_mut; do echo "$f: $(tail -1 $X/$f.log 2>/dev/null | sed 's#/opt/dlami/nvme/dfperf/runs/##g')"; done; date +%T
