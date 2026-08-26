#!/usr/bin/env bash
# One kit invocation with TRACE logging for reservation evidence.
set -euo pipefail
REPO=/localhome/local-kkristensen/Code/sirius
PLANS="$REPO/bench/sf1000-repro/plans"
CUDF_SO="${CUDF_SO:-$HOME/cudf-src/cpp/build/libcudf.so}"
export LD_PRELOAD="$CUDF_SO"
export SIRIUS_PRE_SQL="SET pin_table_compression = true; \
SET pin_table_input_compression_plan_dir = '$PLANS'; \
SET expression_evaluator_strategy = 'ast_jit'; \
SET sirius_log_level = 'trace'"
for t in LINEITEM ORDERS PART CUSTOMER SUPPLIER NATION REGION PARTSUPP; do
  export "SIRIUS_PIN_TIER_$t=gpu"
done
cd "$REPO"
python3 test/tpch_performance/performance_test.py \
  --input "$DATA" \
  --mode grouped --iterations "$ITER" --engine gpu --pin host \
  --queries "$QUERIES" --config "$CFG" --name "$NAME"
