#!/usr/bin/env bash
# run_session.sh <config.yaml> <outdir> <iters> <mode: time|nsys> [queries...]
# One DuckDB session: views -> host pin-all (compression per config) -> per query, <iters> labeled iterations.
# mode=nsys: iteration 1 uncaptured (warm-up), iterations 2..N wrapped in profiler_start/stop -> one nsys report per query.
set -uo pipefail
CFG=$1; OUT=$2; ITERS=$3; MODE=$4; shift 4
QUERIES=("$@"); [ ${#QUERIES[@]} -eq 0 ] && QUERIES=($(seq 1 22))
REPO=/home/ubuntu/sirius; TP=$REPO/test/tpch_performance; PQ=/opt/dlami/nvme/tpch_parquet_sf1000
DUCKDB=$REPO/sirius-duckdb/build/release/duckdb; EXT=$REPO/sirius-duckdb/build/release/extension/sirius/sirius.duckdb_extension
mkdir -p "$OUT"; cp "$CFG" "$OUT/config.yaml"; SQL="$OUT/session.sql"
{
  for T in customer lineitem nation orders part partsupp region supplier; do
    F=$(ls "$PQ/$T/"*.parquet | sed "s/.*/'&'/" | paste -sd,); echo "CREATE VIEW $T AS SELECT * FROM read_parquet([$F]);"; done
  echo ".timer on"
  echo ".print __PIN_BEGIN__"
  SIRIUS_PIN_TIER=${PIN_TIER:-host} python3 "$TP/tpch_pin_columns.py" pin-all "$PQ"
  echo ".print __PIN_END__"
  for q in "${QUERIES[@]}"; do
    for ((i=1;i<=ITERS;i++)); do
      echo ".print __Q${q}_ITER${i}__"
      [ "$MODE" = nsys ] && [ $i -eq 2 ] && echo "CALL profiler_start();"
      echo "CALL sirius_set_query_label('tpch_q${q}_iter${i}');"
      cat "$TP/tpch_queries/orig/q${q}.sql"; echo
    done
    [ "$MODE" = nsys ] && [ $ITERS -ge 2 ] && echo "CALL profiler_stop();" && echo ".shell sleep ${NSYS_GAP:-0}"
  done
} > "$SQL"
export SIRIUS_DISABLE=0 SIRIUS_CONFIG_FILE="$CFG" SIRIUS_LOG_DIR="$OUT/log"
echo "start $(date -Is) commit=$(git -C $REPO rev-parse HEAD)" > "$OUT/meta.txt"
if [ "$MODE" = nsys ]; then
  mkdir -p "$OUT/nsys"
  mkdir -p "$OUT/nsys"
  nsys profile --trace=cuda,nvtx,osrt --sample=none --cudabacktrace=none --cuda-memory-usage=false ${NSYS_EXTRA:-} \
    --capture-range=cudaProfilerApi --capture-range-end=repeat \
    --output="$OUT/nsys/rep" --force-overwrite=true --stats=false \
    "$DUCKDB" -no-agent -unsigned -bail -cmd "LOAD '$EXT';" -f "$SQL" > "$OUT/output.txt" 2>&1
else
  "$DUCKDB" -no-agent -unsigned -bail -cmd "LOAD '$EXT';" -f "$SQL" > "$OUT/output.txt" 2>&1
fi
echo "rc=$? end $(date -Is)" >> "$OUT/meta.txt"
# timings: the Run Time line after each query (skip label CALL lines -> take the last Run Time before next marker)
python3 - "$OUT" <<'PY'
import re,sys
out=sys.argv[1]; cur=None; last=None; rows=[]
for line in open(f"{out}/output.txt", errors="replace"):
    m=re.match(r"__(Q\d+_ITER\d+|PIN_BEGIN|PIN_END)__", line.strip())
    if m:
        if cur and last is not None: rows.append((cur,last))
        cur=m.group(1); last=None; continue
    m=re.search(r"Run Time \(s\): real ([0-9.]+)", line)
    if m: v=float(m.group(1)); last=(max(last or 0,v) if cur and not cur.startswith("PIN") else (last or 0)+v)
if cur and last is not None: rows.append((cur,last))
with open(f"{out}/timings.csv","w") as f:
    f.write("query,iter,seconds\n")
    for k,v in rows:
        if k.startswith("Q"):
            q,i=re.match(r"Q(\d+)_ITER(\d+)",k).groups(); f.write(f"{q},{i},{v}\n")
        else: f.write(f"{k},0,{v}\n")
PY
