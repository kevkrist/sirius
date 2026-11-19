#!/bin/bash
# Wave 2: repeat blocks in reversed order (B A A B) + queries the prototype did not time.
L=/opt/dlami/nvme/dfperf/machine.lock; D=/opt/dlami/nvme/dfperf/runs/vop4/bin
K="SIRIUS_PROTO_BLOOM_RANGE=100000:3000000"; P="SIRIUS_PROTO_BLOOM_BPK=8"
# kind: repeats on claim carriers + one control
for q in 11 17 8 21 18; do OFFSET=2 B_ENV=$K flock $L $D/abba.sh kind 6 $q "B A A B" & done
# kind: affected, never timed (sets of 0.35-2M keys behind a prior mask)
for q in 2 7 19 20; do B_ENV=$K flock $L $D/abba.sh kind 6 $q "A B B A" & done
# bpk8: repeats on claim carriers
for q in 21 3 7; do OFFSET=2 B_ENV=$P flock $L $D/abba.sh bpk8 6 $q "B A A B" & done
# bpk8: Bloom queries the suite extrapolation rests on
for q in 10 4 9 8 5; do B_ENV=$P flock $L $D/abba.sh bpk8 6 $q "A B B A" & done
wait; echo WAVE2_DONE $(date +%T)
