#!/bin/bash
# usage: logsec.sh <log> <label> [regex]  -> lines of that labeled query section matching regex
L=$1; q=$2; re=${3:-.}
s=$(grep -n "label('$q')" "$L" | head -1 | cut -d: -f1)
awk -v s="$s" 'NR>s && /sirius_set_query_label/ {exit} NR>s' "$L" | grep -E "$re" | grep -v "column count" | cut -c1-300
