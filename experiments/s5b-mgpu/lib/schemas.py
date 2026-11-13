#!/usr/bin/env python3
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
"""The one definition of every Parquet table the S5b kit writes under `results/`.

Every writer (`bench.py`, `parity_stage.py`, `probe.py`, `check_data.py`, `cpu_reference.py`)
names a table here and passes its rows to `write_table`, which refuses a column the table does not
declare. Rows are assembled with `merge_row`, which refuses a column that two parts both supply: a
cell's `filters` setting and a publication's pushed-filter count can never overwrite each other.
Event tables take their event columns from the `fields` of the log formats in `formats.py`.

RUNBOOK.md Appendices A (tables) and C (log formats) are generated from these definitions:

    python -I lib/schemas.py --write-runbook RUNBOOK.md   # rewrite the generated blocks
    python -I lib/schemas.py --check-runbook RUNBOOK.md   # exit 1 when they are stale

`selftest.py` runs the check, so a stale appendix fails S0.
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import re
import sys
from typing import Any, Iterable

import pyarrow as pa
import pyarrow.parquet as pq

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import formats  # noqa: E402
from formats import Field  # noqa: E402

TYPES = {
    "string": pa.string(),
    "int32": pa.int32(),
    "int64": pa.int64(),
    "float64": pa.float64(),
    "bool": pa.bool_(),
}


@dataclasses.dataclass(frozen=True)
class Table:
    """One Parquet file of a stage's results."""

    name: str
    file: str
    stages: str
    fields: tuple[Field, ...]
    doc: str

    def schema(self) -> pa.Schema:
        return pa.schema([(f.name, TYPES[f.type]) for f in self.fields])

    @property
    def names(self) -> tuple[str, ...]:
        return tuple(f.name for f in self.fields)


def F(
    name: str, type_: str, doc: str = ""
) -> Field:  # noqa: N802 - a table-definition shorthand
    return Field(name, type_, doc)


def _check_unique(fields: Iterable[Field]) -> tuple[Field, ...]:
    fields = tuple(fields)
    seen: set[str] = set()
    for field in fields:
        if field.name in seen:
            raise ValueError(f"column {field.name!r} declared twice")
        seen.add(field.name)
    return fields


# ---------------------------------------------------------------------------------------------
# Context columns
# ---------------------------------------------------------------------------------------------

C_CONTEXT = (
    F("stage", "string"),
    F("label", "string", "run label, for example `tip_4gpu`"),
    F("build", "string", "tip, key or base"),
    F("commit", "string"),
    F("gpus", "int32"),
    F("sf", "int32"),
    F("settings", "string", "the SET statements of the label"),
    F("query", "string", "q1..q22"),
)
C_EVENT_CONTEXT = (F("stage", "string"), F("label", "string"), F("query", "string"))

SESSION_CONTEXT = (
    F("stage", "string"),
    F("session", "string", "one DuckDB process, for example `tip_4gpu`"),
    F(
        "role",
        "string",
        "base_a, tip, base_b, base_confirm, tip_confirm, matrix or confirm",
    ),
    F("build", "string"),
    F("commit", "string"),
    F("gpus", "int32"),
    F("sf", "int32"),
)
CELL_CONTEXT = (
    F("label", "string", "settings cell"),
    F("experiment", "string", "E1 or E2 (NULL in P1)"),
    F("concat_divisor", "int32", "H divisor; 0 means 0 bytes (NULL in P1)"),
    F("policy", "string", "build_fold_policy (NULL in P1)"),
    F("filters", "string", "Dynamic Filters on or off (NULL in P1)"),
    F("settings", "string", "the SET statements of the cell"),
)
P_CONTEXT = (*SESSION_CONTEXT, *CELL_CONTEXT, F("query", "string"))
RUN_KEY = (F("pass", "int32", "0 is the warm pass"), F("kind", "string", "warm or hot"))
OPERATOR_FIELDS = (
    F("operator", "string"),
    F("operator_id", "int32"),
    F("tasks", "int32"),
    F("total_ms", "float64"),
    F("first_output_s", "float64", "first output after the query window opened"),
)
MEMORY_FIELDS = (
    F("oom_retries", "int32"),
    F("reschedules", "int32"),
    F("downgrade_bytes", "int64"),
    F("reservation_shortfalls", "int32"),
    F("reservation_failures", "int32"),
)


def _table(
    name: str, file: str, stages: str, doc: str, *groups: Iterable[Field]
) -> Table:
    return Table(name, file, stages, _check_unique(f for g in groups for f in g), doc)


TABLES = {
    t.name: t
    for t in (
        # S0
        _table(
            "S0.settings",
            "settings.parquet",
            "S0",
            "settings a loaded Sirius resolved, per build and GPU count (`probe`)",
            (F("build", "string"), F("commit", "string"), F("gpus", "int32")),
            (F("name", "string"), F("value", "string")),
        ),
        _table(
            "S0.data",
            "data.parquet",
            "S0",
            "the TPC-H Parquet datasets, from their footers",
            (
                F("sf", "int32"),
                F("path", "string"),
                F("tables", "int32"),
                F("files", "int32"),
                F("bytes", "int64", "Parquet bytes on disk"),
                F(
                    "uncompressed_bytes",
                    "int64",
                    "column-chunk bytes before Parquet compression",
                ),
                F("lineitem_rows", "int64"),
                F("ok", "bool"),
                F("problem", "string"),
            ),
        ),
        # C stages
        _table(
            "C.queries",
            "queries.parquet",
            "C2, C4, C5",
            "one row per label and query (each query is its own process)",
            C_CONTEXT,
            (
                F("returncode", "int32"),
                F("result_match", "bool", "GPU result equals the CPU result"),
                F(
                    "result_order",
                    "string",
                    "ordered, or multiset when only the row order differs",
                ),
                F("gpu_fallback", "bool"),
                F("plan_fallback", "bool"),
                F("gpu_error", "string"),
                F("key_lines", "int32"),
                F("constructions_started", "int32"),
                F("publications", "int32"),
                F("bloom_transfers_peer", "int32"),
                F("bloom_transfers_host", "int32"),
                F("multi_partition_joins", "int32"),
            ),
            MEMORY_FIELDS,
            (
                F("replica_problems", "int32"),
                F("severe_lines", "int32"),
                F(
                    "settings_problems",
                    "string",
                    "logged settings that disagree with the label",
                ),
                F("gpu_peak_bytes", "int64", "peak GPU pool bytes of the process"),
            ),
        ),
        _table(
            "C.keys",
            "keys.parquet",
            "C2, C4, C5",
            "one row per Key Outcome",
            (
                F("stage", "string"),
                F("label", "string"),
                F("build", "string"),
                F("gpus", "int32"),
                F("sf", "int32"),
                F("query", "string"),
            ),
            formats.KEY_LINE_FIELDS[1:],
        ),
        _table(
            "C.key_diff",
            "key_diff.parquet",
            "C2",
            "one row per Key Outcome compared between the baseline and the tip",
            (
                F("stage", "string"),
                F("base_label", "string"),
                F("candidate_label", "string"),
                F("query", "string"),
                F("sig", "string"),
                F("occ", "int32"),
                F("condition", "int32"),
                F(
                    "status",
                    "string",
                    "equal, stronger, weaker, extra, missing or duplicate",
                ),
                F("zone_map", "string", "equal, lost or gained"),
                F("base_kind", "string"),
                F("candidate_kind", "string"),
                F("base_size", "int64"),
                F("candidate_size", "int64"),
                F("base_reason", "string"),
                F("candidate_reason", "string"),
                F("declared_delta", "bool", "the candidate reason is a declared delta"),
                F("failed", "bool", "fails the parity script"),
                F(
                    "strict_failed",
                    "bool",
                    "fails the C2 bar (declared deltas included)",
                ),
            ),
        ),
        _table(
            "C.bloom_transfers",
            "bloom_transfers.parquet",
            "C2, C4, C5",
            "one row per Bloom Filter reduction",
            C_EVENT_CONTEXT,
            formats.BLOOM_FINISH.fields,
        ),
        _table(
            "C.publications",
            "publications.parquet",
            "C2, C4, C5",
            "one row per Publication Session that constructed filters",
            C_EVENT_CONTEXT,
            formats.PUBLICATION.fields,
        ),
        _table(
            "C.join_strategies",
            "join_strategies.parquet",
            "C2, C4, C5",
            "one row per hash join partition strategy",
            C_EVENT_CONTEXT,
            formats.JOIN_STRATEGY.fields,
        ),
        _table(
            "C.build_folds",
            "build_folds.parquet",
            "C2, C4, C5",
            "one row per build fold decision",
            C_EVENT_CONTEXT,
            formats.BUILD_FOLD.fields,
        ),
        # P stages
        _table(
            "P.sessions",
            "sessions.parquet",
            "P1, P2, P3",
            "one row per DuckDB process",
            SESSION_CONTEXT,
            (
                F("seed", "int64", "seed of the shuffled cell order"),
                F("cells", "int32"),
                F("queries", "int32"),
                F("passes", "int32", "1 warm + hot passes"),
                F("status", "string", "ok, error, timeout or stalled"),
                F("returncode", "int32"),
                F("wall_s", "float64"),
                F("setup_s", "float64", "LOAD, views and pinning"),
                F(
                    "gpu_peak_bytes",
                    "int64",
                    "peak GPU pool bytes of the process (max over GPUs)",
                ),
                F("host_rss_peak_bytes", "int64"),
                F("mem_available_min_bytes", "int64", "lowest MemAvailable seen"),
                F(
                    "disk_peak_bytes",
                    "int64",
                    "largest size of the session directory seen",
                ),
                F("pinned_tables", "int32"),
                F("compressed_tables", "int32", "pinned tables the config compressed"),
                F("error", "string"),
            ),
        ),
        _table(
            "P.gpu_state",
            "gpu_state.parquet",
            "P1, P2, P3",
            "nvidia-smi clocks, temperature and throttle reasons before and after each process",
            (
                F("stage", "string"),
                F("session", "string"),
                F("moment", "string", "before or after"),
            ),
            (
                F("gpu", "int32"),
                F("sm_clock_mhz", "int32"),
                F("mem_clock_mhz", "int32"),
                F("max_sm_clock_mhz", "int32"),
                F("temperature_c", "int32"),
                F("power_w", "float64"),
                F("pstate", "string"),
                F("throttle_mask", "string", "clocks event reasons bit mask"),
                F(
                    "throttle_reasons",
                    "string",
                    "decoded names; `gpu_idle` is normal between processes",
                ),
            ),
        ),
        _table(
            "P.runs",
            "runs.parquet",
            "P1, P2, P3",
            "one row per query run",
            P_CONTEXT,
            RUN_KEY,
            (
                F("latency_ms", "float64", "DuckDB CLI `.timer` real time"),
                F("status", "string"),
            ),
            MEMORY_FIELDS,
            (
                F("pinned_scans", "int32", "scans served from pinned tables"),
                F("plan_fallback", "bool"),
                F("runtime_fallback", "bool"),
                F("severe_lines", "int32"),
            ),
        ),
        _table(
            "P.queries",
            "queries.parquet",
            "P1, P2, P3",
            "one row per session, cell and query",
            P_CONTEXT,
            (
                F(
                    "status",
                    "string",
                    "ok unless a run failed, fell back or read from disk",
                ),
                F("warm_ms", "float64"),
                F("median_hot_ms", "float64", "the latency metric"),
                F("min_hot_ms", "float64"),
                F("max_hot_ms", "float64"),
                F("hot_runs", "int32"),
                F("hot_spread", "float64", "(max - min) / median of the hot runs"),
                F("result_hash", "string"),
                F("result_rows", "int64"),
                F("ref_match", "bool", "warm result equals the CPU reference"),
                F(
                    "ref_order",
                    "string",
                    "ordered, or multiset when only the row order differs",
                ),
                F("ref_detail", "string"),
                F("fallback", "bool"),
            ),
            MEMORY_FIELDS,
            (
                F("pinned_scans_min", "int32", "fewest pinned scans over the runs"),
                F(
                    "settings_problems",
                    "string",
                    "logged settings that disagree with the cell",
                ),
                F("pair_tasks", "int32", "HASH_JOIN tasks (trace warm run)"),
                F("hash_join_ms", "float64"),
                F("concat_ms", "float64"),
                F("first_pair_s", "float64"),
                F("folds", "int32"),
                F("fold_decisions", "int32"),
            ),
        ),
        _table(
            "P.build_folds",
            "build_folds.parquet",
            "P1, P2, P3",
            "build fold decisions of the warm runs",
            P_CONTEXT,
            formats.BUILD_FOLD.fields,
        ),
        _table(
            "P.operators",
            "operators.parquet",
            "P2, P3",
            "per-operator task output of the trace warm runs",
            P_CONTEXT,
            OPERATOR_FIELDS,
        ),
        _table(
            "P.bloom_transfers",
            "bloom_transfers.parquet",
            "P1, P2, P3",
            "Bloom Filter reductions of the warm runs",
            P_CONTEXT,
            formats.BLOOM_FINISH.fields,
        ),
        _table(
            "P.publications",
            "publications.parquet",
            "P1, P2, P3",
            "Publication Sessions of every run (the line is info level)",
            P_CONTEXT,
            RUN_KEY,
            formats.PUBLICATION.fields,
        ),
        _table(
            "P.join_strategies",
            "join_strategies.parquet",
            "P1, P2, P3",
            "hash join partition strategies of the warm runs",
            P_CONTEXT,
            formats.JOIN_STRATEGY.fields,
        ),
        _table(
            "P1.compare",
            "compare.parquet",
            "P1",
            "base against tip per GPU count and query",
            (F("sf", "int32"), F("gpus", "int32"), F("query", "string")),
            (
                F("base_a_ms", "float64", "first base process"),
                F("tip_ms", "float64"),
                F("base_b_ms", "float64", "second base process (A/A)"),
                F("base_ms", "float64", "geometric mean of base_a_ms and base_b_ms"),
                F("ratio", "float64", "tip_ms / base_ms"),
                F(
                    "aa_ratio",
                    "float64",
                    "base_b_ms / base_a_ms: the noise of this query",
                ),
                F("base_confirm_ms", "float64"),
                F("tip_confirm_ms", "float64"),
                F("confirmed_ratio", "float64"),
                F(
                    "final_ratio",
                    "float64",
                    "confirmed_ratio when confirmed, else ratio",
                ),
                F("identical", "bool", "base and tip results equal"),
                F("base_ref_match", "bool"),
                F("tip_ref_match", "bool"),
                F("tip_ref_order", "string"),
                F("base_oom_retries", "int32"),
                F("tip_oom_retries", "int32"),
                F("base_downgrade_bytes", "int64"),
                F("tip_downgrade_bytes", "int64"),
            ),
        ),
        _table(
            "P2.compare",
            "compare.parquet",
            "P2, P3",
            "each fold policy against `never` per cell and query",
            (
                F("experiment", "string"),
                F("concat_divisor", "int32"),
                F("filters", "string"),
                F("policy", "string"),
                F("query", "string"),
            ),
            (
                F("never_ms", "float64"),
                F("median_ms", "float64"),
                F("ratio", "float64"),
                F("noise", "float64", "larger hot-run spread of the two"),
                F("same_result_as_never", "bool"),
                F("ref_match", "bool"),
                F("oom_over_never", "int64"),
                F("downgrade_over_never", "int64"),
                F("folds", "int32"),
                F("flagged", "bool", "over a bar before confirmation"),
                F("confirmed", "bool", "a confirming re-run exists"),
                F("confirmed_ratio", "float64"),
                F("confirmed_oom_over_never", "int64"),
                F("confirmed_downgrade_over_never", "int64"),
                F("final_ratio", "float64"),
                F("final_oom_over_never", "int64"),
                F("final_downgrade_over_never", "int64"),
            ),
        ),
        _table(
            "P.reference",
            "reference_sf<N>.parquet",
            "P1, P2",
            "the CPU reference answers",
            (
                F("sf", "int32"),
                F("query", "string"),
                F("rows", "int64"),
                F("ref_hash", "string"),
                F("cpu_seconds", "float64"),
                F("spill_peak_bytes", "int64", "largest DuckDB temp directory seen"),
                F("duckdb_commit", "string"),
            ),
        ),
    )
}


class SchemaError(ValueError):
    """A row that does not fit its table."""


def merge_row(*parts: dict[str, Any]) -> dict[str, Any]:
    """One row from @p parts; a column that two parts both supply is an error."""
    row: dict[str, Any] = {}
    for part in parts:
        clash = row.keys() & part.keys()
        if clash:
            raise SchemaError(f"columns supplied twice: {sorted(clash)}")
        row.update(part)
    return row


def write_table(name: str, rows: list[dict[str, Any]], path: pathlib.Path) -> None:
    """Writes @p rows as table @p name; absent columns are NULL, undeclared ones an error."""
    table = TABLES[name]
    declared = set(table.names)
    for row in rows:
        extra = row.keys() - declared
        if extra:
            raise SchemaError(f"{name}: undeclared columns {sorted(extra)}")
    columns = {n: [row.get(n) for row in rows] for n in table.names}
    data = pa.Table.from_pydict(columns, schema=table.schema())
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    pq.write_table(data, tmp, compression="zstd")
    tmp.replace(path)


# ---------------------------------------------------------------------------------------------
# Generated RUNBOOK appendices
# ---------------------------------------------------------------------------------------------


def tables_markdown() -> str:
    lines = []
    for table in TABLES.values():
        columns = ", ".join(
            f"`{f.name}` {f.type}" + (f" ({f.doc})" if f.doc else "")
            for f in table.fields
        )
        lines += [
            f"- **`{table.file}`** ({table.stages}, `{table.name}`): {table.doc}. {columns}."
        ]
    return "\n".join(lines)


BLOCKS = {"tables": tables_markdown, "formats": formats.appendix_markdown}
BLOCK = re.compile(
    r"(<!-- BEGIN GENERATED (?P<name>\w+) by lib/schemas.py; do not edit -->\n)"
    r"(?:.*?\n)?"
    r"(<!-- END GENERATED (?P=name) -->)",
    re.S,
)


def render_runbook(text: str) -> str:
    """@p text with every generated block replaced by its current content."""
    found = set()

    def replace(match: re.Match[str]) -> str:
        found.add(match["name"])
        return match.group(1) + BLOCKS[match["name"]]() + "\n" + match.group(3)

    rendered = BLOCK.sub(replace, text)
    if found != set(BLOCKS):
        raise SchemaError(
            f"RUNBOOK generated blocks found {sorted(found)}, expected {sorted(BLOCKS)}"
        )
    return rendered


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--write-runbook", type=pathlib.Path)
    group.add_argument("--check-runbook", type=pathlib.Path)
    args = parser.parse_args()
    path = args.write_runbook or args.check_runbook
    text = path.read_text()
    rendered = render_runbook(text)
    if args.write_runbook:
        path.write_text(rendered)
        return 0
    if rendered != text:
        print(
            f"{path}: generated appendices are stale; run --write-runbook",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
