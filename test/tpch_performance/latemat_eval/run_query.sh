#!/usr/bin/env bash
# Run one query under a given execution config against tpch_sf50.duckdb, capturing
# CSV output and the Sirius debug log (to prove whether late-mat fired).
#
# usage: run_query.sh <cpu|eager|latemat> <out_csv> <logdir> < query.sql
set -euo pipefail
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"

cfg="$1"; out="$2"; logdir="$3"
rm -rf "$logdir"; mkdir -p "$logdir"

B=build/release/duckdb
EXT=build/release/extension/sirius/sirius.duckdb_extension
DB=test_datasets/tpch_sf50.duckdb

case "$cfg" in
  cpu)     SETS="SET gpu_execution=false;";;
  eager)   SETS="SET gpu_execution=true; SET enable_gpu_duckdb_native_scan=true; SET late_materialize_native_strings=false;";;
  latemat) SETS="SET gpu_execution=true; SET enable_gpu_duckdb_native_scan=true; SET late_materialize_native_strings=true;";;
  *) echo "bad config: $cfg" >&2; exit 2;;
esac

QUERY="$(cat)"
# build/release/duckdb auto-loads the Sirius extension, so no explicit LOAD.
PRE="SET sirius_log_level='debug'; SET sirius_log_dir='$logdir'; $SETS"
pixi run "$B" "$DB" -csv -c "$PRE $QUERY" > "$out"
