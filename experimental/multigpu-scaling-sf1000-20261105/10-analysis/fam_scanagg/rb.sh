#!/bin/bash
# usage: rb.sh <g1|g4> <query> <substr>...  -> nsys_range_breakdown restricted to the query's iter-2 window
B=/home/ubuntu/sirius-scaling-20261105
cfg=$1; q=$2; shift 2
row=$(grep -E "^$q,2," $B/01-nsys/nsys2_$cfg/nsys/map.csv)
IFS=, read qq it rep sq ov qm s e <<< "$row"
echo "######## $cfg Q$q window $s,$e"
WINDOW=$s,$e python3 $B/bin/nsys_range_breakdown.py "$sq" "$@"
