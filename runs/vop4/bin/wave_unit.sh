#!/bin/bash
# DF unit tests on the prototype build: no env, 8 bpk, kind range. Each takes the lock for itself.
L=/opt/dlami/nvme/dfperf/machine.lock; D=/opt/dlami/nvme/dfperf/runs/vop4/bin
T='[dynamic_filter]~[cuda_status_fault]~[.]'
flock $L $D/unit.sh noenv "$T" &
SIRIUS_PROTO_BLOOM_BPK=8 flock $L $D/unit.sh bpk8 "$T" &
SIRIUS_PROTO_BLOOM_RANGE=100000:3000000 flock $L $D/unit.sh kind "$T" &
SIRIUS_PROTO_BLOOM_RANGE=1:3000000 flock $L $D/unit.sh kindall "$T" &
wait; echo UNIT_DONE $(date +%T)
