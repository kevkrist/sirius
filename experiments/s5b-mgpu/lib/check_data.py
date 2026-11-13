#!/usr/bin/env python3
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
"""Checks the TPC-H Parquet datasets the stages read, from Parquet footers only.

A dataset passes when all eight tables are present and lineitem holds about 6,000,000 x SF rows
(TPC-H's lineitem count varies slightly around that). It also sums the column chunks' uncompressed
sizes, which bound what the P stages pin on the host tier. Writes `results/S0/data.parquet` and
prints one line per scale factor; exits nonzero when any requested dataset is missing or wrong.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import sys

import pyarrow.parquet as pq

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import common  # noqa: E402
import schemas  # noqa: E402

TABLES = common.TPCH_TABLES


def table_files(root: pathlib.Path, table: str) -> list[pathlib.Path]:
    if (root / f"{table}.parquet").is_file():
        return [root / f"{table}.parquet"]
    return sorted((root / table).glob("*.parquet")) if (root / table).is_dir() else []


def check(sf: int) -> dict:
    override = os.environ.get(f"S5B_TPCH_SF{sf}")
    root = (
        pathlib.Path(override)
        if override
        else common.env_path("S5B_DATA") / f"tpch_parquet_sf{sf}"
    )
    row = {
        "sf": sf,
        "path": str(root),
        "tables": 0,
        "files": 0,
        "bytes": 0,
        "uncompressed_bytes": 0,
        "lineitem_rows": None,
        "ok": False,
        "problem": None,
    }
    if not root.is_dir():
        row["problem"] = "missing"
        return row
    missing = []
    for table in TABLES:
        files = table_files(root, table)
        if not files:
            missing.append(table)
            continue
        row["tables"] += 1
        row["files"] += len(files)
        row["bytes"] += sum(f.stat().st_size for f in files)
        metadata = [pq.ParquetFile(f).metadata for f in files]
        row["uncompressed_bytes"] += sum(
            m.row_group(g).total_byte_size
            for m in metadata
            for g in range(m.num_row_groups)
        )
        if table == "lineitem":
            row["lineitem_rows"] = sum(m.num_rows for m in metadata)
    expected = 6_000_000 * sf
    if missing:
        row["problem"] = f"tables missing: {missing}"
    elif (
        row["lineitem_rows"] is None
        or abs(row["lineitem_rows"] - expected) > 0.01 * expected
    ):
        row["problem"] = (
            f"lineitem has {row['lineitem_rows']} rows, expected about {expected}"
        )
    else:
        row["ok"] = True
    return row


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--sf", default="1,10,100,1000")
    args = parser.parse_args()
    rows = [check(int(sf)) for sf in args.sf.split(",")]
    for row in rows:
        state = "ok" if row["ok"] else f"NOT READY ({row['problem']})"
        print(
            f"SF{row['sf']}: {state}: {row['path']} {row['bytes'] / 2**30:.1f} GiB "
            f"({row['uncompressed_bytes'] / 2**30:.1f} GiB uncompressed), {row['files']} files, "
            f"lineitem rows {row['lineitem_rows']}"
        )
    schemas.write_table("S0.data", rows, common.results_dir("S0") / "data.parquet")
    return 0 if all(row["ok"] for row in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
