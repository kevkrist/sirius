#!/usr/bin/env python3
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
"""Writes the 22 TPC-H queries as `S5B_QUERIES/sf<N>/q<i>.sql`, one statement per file.

The text comes from the tip worktree's `test/tpch_performance/queries.py`, the benchmark
runner's source of truth, which renders Q11's FRACTION for the scale factor.
"""

from __future__ import annotations

import argparse
import importlib.util
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import common  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--sf", default="1,10,100,1000", help="comma list of scale factors"
    )
    args = parser.parse_args()
    source = common.worktree("tip") / "test" / "tpch_performance" / "queries.py"
    spec = importlib.util.spec_from_file_location("tpch_queries", source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    for sf in (int(value) for value in args.sf.split(",")):
        out = common.env_path("S5B_QUERIES") / f"sf{sf}"
        out.mkdir(parents=True, exist_ok=True)
        rendered = module.queries_for_scale_factor(sf)
        if sorted(rendered) != sorted(common.TPCH_QUERIES):
            sys.exit(f"error: unexpected query set {sorted(rendered)}")
        for name, text in rendered.items():
            common.write_text_atomic(
                out / f"{name}.sql", text.strip().rstrip(";") + ";\n"
            )
        common.log(f"wrote 22 queries to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
