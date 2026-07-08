#!/usr/bin/env python3
"""Run one process of the Track A dynamic-filter priority acceptance matrix."""

import argparse
import csv
import hashlib
import json
import os
import time
from datetime import datetime, timezone
from pathlib import Path

from performance_test import QUERIES, open_connection


CONFIG_SQL = {
    "no_filter": ("SET enable_dynamic_filter_pushdown=false",),
    "legacy": (
        "SET enable_dynamic_filter_pushdown=true",
        "SET dynamic_filter_build_priority='legacy'",
    ),
    "off": (
        "SET enable_dynamic_filter_pushdown=true",
        "SET dynamic_filter_build_priority='off'",
    ),
}

SYNTHETIC_MANY_JOIN = """
SELECT count(*) AS row_count,
       sum(l.l_extendedprice * (1 - l.l_discount)) AS revenue
FROM lineitem l
JOIN orders o ON o.o_orderkey = l.l_orderkey
JOIN customer c ON c.c_custkey = o.o_custkey
JOIN nation customer_nation ON customer_nation.n_nationkey = c.c_nationkey
JOIN region r ON r.r_regionkey = customer_nation.n_regionkey
JOIN nation supplier_nation
  ON supplier_nation.n_nationkey = customer_nation.n_nationkey
 AND supplier_nation.n_regionkey = r.r_regionkey
JOIN supplier s
  ON s.s_suppkey = l.l_suppkey
 AND s.s_nationkey = supplier_nation.n_nationkey
JOIN partsupp ps
  ON ps.ps_suppkey = s.s_suppkey
 AND ps.ps_partkey = l.l_partkey
JOIN part p ON p.p_partkey = ps.ps_partkey
WHERE r.r_name = 'ASIA'
  AND o.o_orderdate >= DATE '1995-01-01'
  AND o.o_orderdate < DATE '1996-01-01'
  AND p.p_size BETWEEN 1 AND 50
"""

WORKLOAD = {
    "q5": QUERIES["q5"],
    "q7": QUERIES["q7"],
    "q8": QUERIES["q8"],
    "q9": QUERIES["q9"],
    "q21": QUERIES["q21"],
    "many_join": SYNTHETIC_MANY_JOIN,
}


def canonical_result(rows):
    return "".join(f"{row!r}\n" for row in sorted(rows, key=repr))


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--config", required=True, choices=CONFIG_SQL)
    parser.add_argument("--pass-level", required=True, choices=("info", "debug"))
    parser.add_argument("--iterations", required=True, type=int)
    return parser.parse_args()


def main():
    args = parse_args()
    if args.iterations < 1:
        raise SystemExit("--iterations must be positive")

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    log_dir = output / "logs"
    log_dir.mkdir(exist_ok=True)
    os.environ["SIRIUS_LOG_LEVEL"] = args.pass_level
    os.environ["SIRIUS_LOG_DIR"] = str(log_dir)

    manifest = {
        "config": args.config,
        "config_sql": CONFIG_SQL[args.config],
        "dataset": str(args.input.resolve()),
        "pass_level": args.pass_level,
        "iterations": args.iterations,
        "discard_timing_iteration": 0 if args.pass_level == "info" else None,
        "workload": list(WORKLOAD),
        "started_at": datetime.now(timezone.utc).isoformat(),
        "pid": os.getpid(),
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    connection = open_connection(str(args.input), gpu_execution=True)
    try:
        for statement in CONFIG_SQL[args.config]:
            connection.execute(statement)
        connection.execute("SET gpu_execution=true")

        with (output / "runtimes.csv").open("w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(("config", "pass_level", "query", "iteration", "runtime_s"))
            for name, query in WORKLOAD.items():
                result_dir = output / "results" / name
                result_dir.mkdir(parents=True, exist_ok=True)
                for iteration in range(args.iterations):
                    print(
                        f"[{datetime.now().isoformat(timespec='seconds')}] "
                        f"{args.config}/{args.pass_level} {name} iteration {iteration}",
                        flush=True,
                    )
                    started = time.perf_counter()
                    rows = connection.execute(query).fetchall()
                    runtime = time.perf_counter() - started
                    canonical = canonical_result(rows)
                    digest = hashlib.sha256(canonical.encode()).hexdigest()
                    (result_dir / f"iteration-{iteration}.txt").write_text(canonical)
                    (result_dir / f"iteration-{iteration}.sha256").write_text(
                        digest + "\n"
                    )
                    writer.writerow(
                        (
                            args.config,
                            args.pass_level,
                            name,
                            iteration,
                            f"{runtime:.9f}",
                        )
                    )
                    stream.flush()
                    print(
                        f"  runtime={runtime:.3f}s rows={len(rows)} sha256={digest}",
                        flush=True,
                    )
    finally:
        connection.close()


if __name__ == "__main__":
    main()
