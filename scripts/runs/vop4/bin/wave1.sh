#!/bin/bash
# Wave 1: every job takes the machine lock for its own duration only.
L=/opt/dlami/nvme/dfperf/machine.lock; D=/opt/dlami/nvme/dfperf/runs/vop4/bin
K="SIRIUS_PROTO_BLOOM_RANGE=100000:3000000"; P="SIRIUS_PROTO_BLOOM_BPK=8"
for q in 17 8 11 21 18 6; do B_ENV=$K flock $L $D/abba.sh kind 6 $q "A B B A" & done
for q in 21 3 7 17 6; do B_ENV=$P flock $L $D/abba.sh bpk8 6 $q "A B B A" & done
B_ENV= flock $L $D/abba.sh binctl 6 3 "A B B A" &
for q in 17 8 11 21 3 7; do flock $L $D/logpass.sh $q & done
wait; echo WAVE1_DONE $(date +%T)
