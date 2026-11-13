#!/usr/bin/env python3
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
"""Computes the CPU reference answer of every TPC-H query once, with plain DuckDB, as Parquet.

Each query runs in its own DuckDB CLI process of the base worktree with `SIRIUS_DISABLE=1` and no
extension loaded, reading the Parquet views, and is written with `COPY (query) TO 'q<i>.parquet'`.
The answers stay under `S5B_WORK/reference/sf<N>/`; `results/<stage>/reference_sf<N>.parquet`
records their row counts, canonical hashes, CPU times and the peak size of DuckDB's spill directory
(`S5B_WORK/reference/duckdb_tmp`, emptied after each query). A query whose Parquet file exists is
skipped.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import sys


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import common  # noqa: E402
import duck  # noqa: E402
import schemas  # noqa: E402

reference_path = common.reference_path


def compute(
    sf: int, query: str, memory_limit: str, timeout_s: float
) -> tuple[float | None, int | None]:
    """CPU seconds and the spill directory's peak bytes; seconds are None on failure."""
    target = reference_path(sf, query)
    directory = common.work_dir("reference", f"sf{sf}", f".{query}")
    partial = target.with_name(target.name + ".partial")
    partial.unlink(missing_ok=True)
    text = (common.queries_dir(sf) / f"{query}.sql").read_text().strip().rstrip(";")
    spill = common.work_dir("reference", "duckdb_tmp")
    script = (
        "\n".join(
            [
                f"SET memory_limit = {duck.sql_string(memory_limit)};",
                f"SET temp_directory = {duck.sql_string(str(spill))};",
                *duck.view_statements(sf),
                ".timer on 6",
                f"COPY ({text}) TO {duck.sql_string(str(partial))} (FORMAT parquet);",
            ]
        )
        + "\n"
    )
    # Plain DuckDB holds no GPU context, but keep the one-engine-at-a-time rule anyway.
    common.wait_for_idle_gpus()
    run = duck.run(
        "base",
        script,
        directory,
        config=None,
        sirius_disabled=True,
        timeout_s=timeout_s,
        watch=spill,
    )
    shutil.rmtree(spill, ignore_errors=True)
    if run.returncode != 0 or not partial.is_file():
        common.log(f"SF{sf} {query}: CPU reference FAILED (see {directory})")
        return None, run.disk_peak_bytes
    partial.replace(target)
    seconds = run.timer_seconds
    return (seconds[-1] if seconds else None), run.disk_peak_bytes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--sf", type=int, required=True)
    parser.add_argument(
        "--stage", required=True, help="results/<stage>/ receives the summary"
    )
    parser.add_argument("--queries", default=",".join(common.TPCH_QUERIES))
    parser.add_argument(
        "--memory-limit",
        default=os.environ.get("S5B_CPU_MEMORY_LIMIT", "60%"),
        help="DuckDB memory_limit (default 60%% of RAM)",
    )
    parser.add_argument(
        "--timeout", type=float, default=7200.0, help="seconds per query"
    )
    args = parser.parse_args()
    failures = 0
    meta_path = (
        common.env_path("S5B_WORK") / "reference" / f"sf{args.sf}" / "cpu_seconds.json"
    )
    meta = json.loads(meta_path.read_text()) if meta_path.is_file() else {}
    for query in args.queries.split(","):
        if reference_path(args.sf, query).is_file():
            continue
        common.log(f"SF{args.sf} {query}: computing the CPU reference")
        seconds, spill = compute(args.sf, query, args.memory_limit, args.timeout)
        if not reference_path(args.sf, query).is_file():
            failures += 1
            continue
        meta[query] = {"cpu_seconds": seconds, "spill_peak_bytes": spill}
        common.write_json_atomic(meta_path, meta)
        common.log(
            f"SF{args.sf} {query}: {seconds} s, spill peak {(spill or 0) / 2**30:.1f} GiB"
        )
    rows = []
    commit = common.git_head(common.worktree("base"))
    for query in common.TPCH_QUERIES:
        path = reference_path(args.sf, query)
        if not path.is_file():
            continue
        table = common.read_reference(path)
        rows.append(
            {
                "sf": args.sf,
                "query": query,
                "rows": table.num_rows,
                "ref_hash": common.result_hash(table),
                "cpu_seconds": (meta.get(query) or {}).get("cpu_seconds"),
                "spill_peak_bytes": (meta.get(query) or {}).get("spill_peak_bytes"),
                "duckdb_commit": commit,
            }
        )
    out = common.results_dir(args.stage) / f"reference_sf{args.sf}.parquet"
    schemas.write_table("P.reference", rows, out)
    common.log(
        f"{len(rows)} reference answer(s) at SF{args.sf}; {failures} failure(s); wrote {out}"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
