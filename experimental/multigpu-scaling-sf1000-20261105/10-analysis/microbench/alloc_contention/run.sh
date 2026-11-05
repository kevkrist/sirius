#!/usr/bin/env bash
cd /home/ubuntu/sirius-scaling-20261105/10-analysis/microbench/alloc_contention
for grant in 0 1; do for args in "0 1" "0 4" "0,1 4" "0,2 4" "0,1,2,3 4" "0,1,2,3 1" "0 16" "0,1,2,3 8"; do ./alloc $args 20000 8 $grant; done; done
