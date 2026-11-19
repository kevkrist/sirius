#!/bin/bash
# gate_summary.sh <log>: collapse [df_gate] lines of the last (hot) window
L=$1; s=$(grep -n "\[window\] begin" $L | tail -1 | cut -d: -f1)
tail -n +$s $L | grep "df_gate" | sed -E 's/^.*\[df_gate\]//' | awk '{k=$0; gsub(/rows=[0-9]+(->[0-9]+)?/,"rows=N",k); gsub(/gate=0x[0-9a-f]+/,"gate",k); gsub(/keep=0\.[0-9]+/,"keep=K",k); gsub(/survival=[0-9.e-]+/,"survival=S",k); c[k]++} END{for(k in c) print c[k], k}' | sort -k2 | cut -c1-230
tail -n +$s $L | grep "df_gate.*decode-probe ch" | sed -E 's/^.*filter=\[([^]]+)\].*rows=([0-9]+)->([0-9]+).*/\1 \2 \3/' | awk '{a[$1" "$2]+=$(NF-1); b[$1" "$2]+=$NF; n[$1" "$2]++} END{for(k in a) printf "DECODE-PROBE-TOTAL %s chunks=%d rows %d -> %d keep=%.4f\n", k, n[k], a[k], b[k], b[k]/a[k]}'
tail -n +$s $L | grep "dynamic_filter_key\]" | sed -E 's/^.*\[dynamic_filter_key\]/KEY/' | cut -c1-230
