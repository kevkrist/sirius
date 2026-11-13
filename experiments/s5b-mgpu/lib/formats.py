# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
"""The one description of every Sirius log line and console marker the S5b kit reads.

`common.py` (log parsing for `bench.py`, `parity_stage.py` and `probe.py`), `schemas.py` (the
Parquet tables, whose event columns are the `fields` declared here) and
`tools/dynamic_filter_key_parity.py` (the `[dynamic_filter_key]` line and the fallback markers)
all read these definitions; RUNBOOK.md Appendix C is generated from them (`schemas.py
--appendix`), and `selftest.py` checks that every pattern matches its sample and that the
appendix is current. Each sample is written from the format string at the cited source line of
the tip (b65efbe3); the base (fec54719) prints the same text for every format P1 reads.

This module uses only the standard library, so the parity tool can import it on any Python 3.
"""

from __future__ import annotations

import dataclasses
import re
from typing import Any, Callable

# `[%Y-%m-%d %T.%e] [%l] [%s:%#] %v` (src/log/spdlog_owning_sink.cpp:54).
LINE_PREFIX = re.compile(
    r"^\[(?P<ts>\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\] \[(?P<level>\w+)\]"
)
TS = "[2026-11-13 10:00:00.100] [debug] [x.cpp:1] "


class FormatError(ValueError):
    """A line that carries a known marker but not the expected shape."""


@dataclasses.dataclass(frozen=True)
class Field:
    """One value a format extracts; `type` is a `schemas.TYPES` name."""

    name: str
    type: str
    doc: str = ""


@dataclasses.dataclass(frozen=True)
class LogFormat:
    """One log line or console marker: where Sirius prints it, how to match it, what it yields."""

    name: str
    source: str
    level: str
    pattern: re.Pattern[str]
    sample: str
    fields: tuple[Field, ...] = ()
    extract: Callable[[re.Match[str]], dict[str, Any]] | None = None
    doc: str = ""

    def parse(self, text: str) -> dict[str, Any] | None:
        """The extracted fields of @p text, `{}` for a format without fields, or None."""
        match = self.pattern.search(text)
        if match is None:
            return None
        return self.extract(match) if self.extract else {}


def _int_or_none(text: str) -> int | None:
    return None if text == "-" else int(text)


def _build_fold(m: re.Match[str]) -> dict[str, Any]:
    rows = _int_or_none(m["rows"])
    resident = int(m["resident_bytes"])
    return {
        "sig": m["sig"],
        "join_id": m["join"],
        "fold_policy": m["policy"],
        "mode": m["mode"],
        "rows": rows,
        "resident_bytes": resident,
        "concat_batch_bytes": int(m["concat_batch_bytes"]),
        "publishes_filters": m["publishes"] == "true",
        "fold": m["fold"] == "true",
        "bytes_per_row": resident / rows if rows else None,
    }


BUILD_FOLD = LogFormat(
    name="build_fold",
    source="src/op/partition/consumer_partition_policy.cpp:57",
    level="debug",
    pattern=re.compile(
        r"\[build_fold\] v=1 sig=(?P<sig>[0-9a-f]{16}) join=(?P<join>\S+) policy=(?P<policy>\S+) "
        r"mode=(?P<mode>\S+) rows=(?P<rows>\S+) resident_bytes=(?P<resident_bytes>\d+) "
        r"concat_batch_bytes=(?P<concat_batch_bytes>\d+) publishes_filters=(?P<publishes>true|false) "
        r"fold=(?P<fold>true|false)"
    ),
    sample=TS
    + "[build_fold] v=1 sig=00ab00cd00ef0011 join=17 policy=bounded mode=STANDARD rows=1000 "
    "resident_bytes=64000 concat_batch_bytes=1024 publishes_filters=true fold=true",
    fields=(
        Field("sig", "string", "join signature"),
        Field("join_id", "string", "operator id"),
        Field("fold_policy", "string", "the `build_fold_policy` the join read"),
        Field("mode", "string", "STANDARD or MIXED_JOIN"),
        Field("rows", "int64", "measured build rows; NULL when unmeasured"),
        Field("resident_bytes", "int64"),
        Field("concat_batch_bytes", "int64", "the value the join read"),
        Field("publishes_filters", "bool"),
        Field("fold", "bool"),
        Field("bytes_per_row", "float64", "resident_bytes / rows"),
    ),
    extract=_build_fold,
    doc="one line per build fold decision",
)

BLOOM_FINISH = LogFormat(
    name="bloom_finish",
    source="src/cuda/sirius_dynamic_bloom_filter.cu:1274",
    level="debug",
    pattern=re.compile(
        r"\[accumulated_bloom_builder::finish\] root GPU (?P<root>-?\d+) reduced and replicated "
        r"(?P<keys>\d+) key\(s\), (?P<bytes>\d+) bytes in all, through "
        r"(?P<transport>peer copies|HOST staging): (?P<sources>\d+) contributing source\(s\), "
        r"(?P<targets>\d+) target\(s\)\."
    ),
    sample=TS
    + "[accumulated_bloom_builder::finish] root GPU 2 reduced and replicated 3 key(s), 4096 bytes "
    "in all, through HOST staging: 4 contributing source(s), 3 target(s).",
    fields=(
        Field("root_gpu", "int32"),
        Field("keys", "int32"),
        Field("bytes", "int64"),
        Field("transport", "string", "peer or host_staging"),
        Field("sources", "int32", "contributing GPUs"),
        Field("replica_gpus", "int32", "GPUs that received a Replica"),
    ),
    extract=lambda m: {
        "root_gpu": int(m["root"]),
        "keys": int(m["keys"]),
        "bytes": int(m["bytes"]),
        "transport": "peer" if m["transport"] == "peer copies" else "host_staging",
        "sources": int(m["sources"]),
        "replica_gpus": int(m["targets"]),
    },
    doc="one Bloom Filter reduction and its Replicas, with the transfer path",
)

PUBLICATION = LogFormat(
    name="publication",
    source="src/op/dynamic_filter/dynamic_filter_publisher.cpp:642",
    level="info",
    pattern=re.compile(
        r"\[dynamic_filter_publication_session\] constructed filters published"
        r"(?P<host> \(Bloom through HOST staging\))?: (?P<filters>\d+) filter\(s\) pushed to "
        r"(?P<targets>\d+) target\(s\), (?P<ms>[0-9.]+) ms after the final contribution\."
    ),
    sample="[2026-11-13 10:00:00.200] [info] [p.cpp:642] [dynamic_filter_publication_session] "
    "constructed filters published (Bloom through HOST staging): 5 filter(s) pushed to 7 "
    "target(s), 1.5 ms after the final contribution.",
    fields=(
        Field("host_staged", "bool"),
        Field(
            "filters_pushed",
            "int32",
            "Dynamic Filters pushed; 0 when nothing was published",
        ),
        Field("pushed_to", "int32", "the count the line reports after `pushed to`"),
        Field(
            "latency_ms",
            "float64",
            "publication latency after the final Contribution; 0.0 when unmeasured",
        ),
    ),
    extract=lambda m: {
        "host_staged": m["host"] is not None,
        "filters_pushed": int(m["filters"]),
        "pushed_to": int(m["targets"]),
        "latency_ms": float(m["ms"]),
    },
    doc="one per Publication Session that constructed filters",
)

CONSTRUCTION_STARTED = LogFormat(
    name="construction_started",
    source="src/op/dynamic_filter/dynamic_filter_publisher.cpp:951",
    level="info",
    pattern=re.compile(
        r"\[dynamic_filter_publication_session\] construction started: (?P<keys>\d+) key\(s\), "
        r"(?P<batches>\d+) batch\(es\) on (?P<gpus>\d+) GPU\(s\)\."
    ),
    sample="[2026-11-13 10:00:00.050] [info] [p.cpp:951] [dynamic_filter_publication_session] "
    "construction started: 2 key(s), 9 batch(es) on 4 GPU(s).",
    doc="one per Construction; counted",
)

REPLICA_QUEUED = LogFormat(
    name="replica_queued",
    source="src/cuda/sirius_dynamic_bloom_filter.cu:560 (and the IN-list filters)",
    level="debug",
    pattern=re.compile(
        r"\[(?P<filter>sirius_dynamic_\w+)\] queued (?P<bytes>\d+)-byte replica GPU (?P<src>\d+) "
        r"-> GPU (?P<dst>\d+)\."
    ),
    sample=TS
    + "[sirius_dynamic_bloom_filter] queued 4096-byte replica GPU 0 -> GPU 3.",
    doc="one Replica copy; counted",
)

ZONE_REPLICATED = LogFormat(
    name="zone_replicated",
    source="src/op/dynamic_filter/sirius_dynamic_filter.cpp:427",
    level="debug",
    pattern=re.compile(
        r"\[sirius_dynamic_zone_map_filter\] replicated \d+ zone\(s\) GPU (?P<src>\d+) -> "
        r"GPU (?P<dst>\d+)\."
    ),
    sample=TS + "[sirius_dynamic_zone_map_filter] replicated 2 zone(s) GPU 1 -> GPU 0.",
    doc="one Zone Map Replica copy; counted",
)

REPLICA_PROBLEM = LogFormat(
    name="replica_problem",
    source="src/cuda/dynamic_filter_replica_transfer.cu:79 (and the filters' skip lines)",
    level="warning",
    pattern=re.compile(r"replica GPU \d+ -> GPU \d+ (?:skipped|unavailable)"),
    sample="[2026-11-13 10:00:00.300] [warning] [r.cu:79] bloom replica GPU 0 -> GPU 2 "
    "unavailable: out of memory. That GPU will skip this optional filter.",
    doc="a Replica that was not made",
)


def _join_strategy(m: re.Match[str]) -> dict[str, Any]:
    return {
        "join_id": int(m["join"]),
        "partitions": int(m["partitions"]),
        "join_gpus": int(m["gpus"]),
        "build_bytes": int(m["build_bytes"]),
        "build_rows": None if m["unmeasured"] else int(m["rows"]),
        "table_bytes": int(m["table_bytes"]),
        "join_type": m["join_type"],
        "mode": m["mode"],
        "broadcast": m["broadcast"] is not None,
    }


JOIN_STRATEGY = LogFormat(
    name="join_strategy",
    source="src/op/sirius_physical_hash_join.cpp:995",
    level="debug",
    pattern=re.compile(
        r"sirius_physical_hash_join id (?P<join>\d+) partition strategy: (?P<partitions>\d+) "
        r"partitions \((?P<gpus>\d+) GPUs\), build side (?P<build_bytes>\d+) bytes, (?P<rows>\d+) "
        r"rows(?P<unmeasured> \(rows-unmeasured\))?, estimated hash table (?P<table_bytes>\d+) "
        r"bytes\. Join Type: (?P<join_type>[^.]+)\. Join Mode: (?P<mode>\w+) *"
        r"(?P<broadcast>\[broadcast\])?\."
    ),
    sample=TS
    + "sirius_physical_hash_join id 13 partition strategy: 1 partitions (4 GPUs), build side 10 "
    "bytes, 0 rows (rows-unmeasured), estimated hash table 18446744073709551615 bytes. Join Type: "
    "RIGHT_SEMI. Join Mode: MIXED_JOIN  [broadcast]. build_card_est 1 probe_card_est 2 placement x",
    fields=(
        Field("join_id", "int32"),
        Field("partitions", "int32"),
        Field("join_gpus", "int32", "GPUs the join saw"),
        Field("build_bytes", "int64"),
        Field("build_rows", "int64", "NULL when unmeasured"),
        Field("table_bytes", "int64"),
        Field("join_type", "string"),
        Field("mode", "string", "STANDARD, MIXED_JOIN or BUILD_PROBE"),
        Field("broadcast", "bool"),
    ),
    extract=_join_strategy,
    doc="one per hash join partition strategy",
)

RESCHEDULE = LogFormat(
    name="reschedule",
    source="src/pipeline/gpu_pipeline_executor.cpp:497",
    level="warning",
    pattern=re.compile(
        r"GPU Pipeline Executor: reschedule \(retry \d+/\d+\) for task .*?: (?P<reason>.*)$"
    ),
    sample="[2026-11-13 10:00:01.000] [warning] [e.cpp:497] GPU Pipeline Executor: reschedule "
    "(retry 1/100) for task 5 (original task 3), resuming from operator index 2: OOM at operator "
    "HASH_JOIN",
    extract=lambda m: {"oom": m["reason"].startswith("OOM")},
    doc="one task retry; an OOM retry when the reason starts with OOM",
)

RETRIES_EXCEEDED = LogFormat(
    name="retries_exceeded",
    source="src/pipeline/gpu_pipeline_executor.cpp:477",
    level="error",
    pattern=re.compile(r"GPU Pipeline Executor: task .* exceeded \d+ retries"),
    sample="[2026-11-13 10:00:01.000] [error] [e.cpp:477] GPU Pipeline Executor: task 5 (original "
    "task 3) exceeded 100 retries at operator HASH_JOIN",
    doc="a task that gave up retrying",
)

DOWNGRADE_DEBUG = LogFormat(
    name="downgrade_request",
    source="src/downgrade/downgrade_executor.cpp:542",
    level="debug",
    pattern=re.compile(
        r"\[downgrade\] \[[^\]]+\] request (?:monitor )?done: (?P<batches>\d+) batches, "
        r"(?P<bytes>\d+) bytes"
    ),
    sample=TS
    + "[downgrade] [GPU:0] request done: 3 batches, 2048 bytes (target 1000, planned 2048) in "
    "1.00 ms",
    extract=lambda m: {"bytes": int(m["bytes"])},
    doc="every downgrade request (debug and trace logs)",
)

DOWNGRADE_MONITOR = LogFormat(
    name="downgrade_monitor",
    source="src/downgrade/downgrade_executor.cpp:526",
    level="info",
    pattern=re.compile(
        r"\[downgrade\]\[monitor\] \[[^\]]+\] request done: (?P<batches>\d+) batches, "
        r"(?P<bytes>\d+) bytes"
    ),
    sample="[2026-11-13 10:00:01.000] [info] [d.cpp:526] [downgrade][monitor] [GPU:0] request "
    "done: 2 batches, 1000 bytes in 1.00 ms (1.0 MB/s), satisfied=true | to_host: 2/1000 "
    "batches/bytes, to_disk: 0/0 batches/bytes",
    extract=lambda m: {"bytes": int(m["bytes"])},
    doc="downgrades by the memory monitor (info logs)",
)

DOWNGRADE_RESERVE = LogFormat(
    name="downgrade_reserve",
    source="src/pipeline/gpu_pipeline_executor.cpp:341",
    level="info",
    pattern=re.compile(
        r"\[downgrade\]\[reserve\] pipeline \S+ task \S+: needed (?P<needed>\d+) bytes \(shortfall "
        r"(?P<shortfall>\d+)\), downgrade freed (?P<freed>\d+) bytes, reservation (?P<outcome>\w+)"
    ),
    sample="[2026-11-13 10:00:01.000] [info] [e.cpp:341] [downgrade][reserve] pipeline 3 task 9: "
    "needed 500 bytes (shortfall 200), downgrade freed 300 bytes, reservation partial",
    extract=lambda m: {
        "freed": int(m["freed"]),
        "satisfied": m["outcome"] == "satisfied",
    },
    doc="one reservation that had to downgrade first (a reservation shortfall)",
)

RESERVATION_FAILED = LogFormat(
    name="reservation_failed",
    source="src/pipeline/gpu_pipeline_executor.cpp:251,353",
    level="error",
    pattern=re.compile(
        r"Failed to acquire memory reservation|reservation still partial"
    ),
    sample="[2026-11-13 10:00:01.000] [error] [e.cpp:251] GPU Pipeline Executor: Failed to "
    "acquire memory reservation for task 9",
    doc="a reservation failure",
)

GPU_POOL = LogFormat(
    name="gpu_pool",
    source="src/sirius_context.cpp:276",
    level="info",
    pattern=re.compile(
        r"\[gpu_pool\] GPU:(?P<gpu>\d+) (?P<tag>\w+)\b.*? peak=(?P<peak>\d+) bytes"
    ),
    sample="[2026-11-13 10:00:00.000] [info] [c.cpp:276] [gpu_pool] GPU:1 QueryEnd instance=0x1 "
    "connection=1 window=2 query=3 allocated=10 bytes peak=999 bytes reserved=0 bytes",
    extract=lambda m: {"gpu": int(m["gpu"]), "peak": int(m["peak"])},
    doc="the process-lifetime peak of one GPU's pool, logged at each query's start and end",
)

WINDOW = LogFormat(
    name="window",
    source="src/sirius_context.cpp:742",
    level="info",
    pattern=re.compile(r"\[window\] (?P<event>begin|end) .* outcome=(?P<outcome>\S+)"),
    sample="[2026-11-13 10:00:02.000] [info] [c.cpp:742] [window] end instance=0x1 connection=1 "
    "window=2 query=3 outcome=ok",
    extract=lambda m: {"event": m["event"], "outcome": m["outcome"]},
    doc="an execution window opening or closing; a query run is ok only with an `end ... ok`",
)

GPU_COMPLETED = LogFormat(
    name="gpu_completed",
    source="src/transparent/physical_sirius_execution.cpp:527",
    level="info",
    pattern=re.compile(re.escape("Transparent GPU execution: query completed")),
    sample="[2026-11-13 10:00:02.000] [info] [s.cpp:527] Transparent GPU execution: query "
    "completed",
    doc="a transparently executed query finished (also after a runtime fallback)",
)

PLAN_FALLBACK = LogFormat(
    name="plan_fallback",
    source="src/sirius_context.cpp:1869-2051",
    level="info",
    pattern=re.compile(re.escape("Transparent execution fallback")),
    sample="[2026-11-13 10:00:00.000] [info] [c.cpp:2041] Transparent execution fallback "
    "(unsupported): window function",
    doc="the planner kept the query on the CPU",
)

# The text after the marker is the GPU error.
RUNTIME_FALLBACK_MARKERS = (
    "falling back to DuckDB CPU within the same transaction. GPU error: ",
    "SiriusExecuteQuery error: ",
)
RUNTIME_FALLBACK = LogFormat(
    name="runtime_fallback",
    source="src/transparent/physical_sirius_execution.cpp:514, src/sirius_extension.cpp:692",
    level="warning",
    pattern=re.compile(
        "(?:"
        + "|".join(re.escape(m) for m in RUNTIME_FALLBACK_MARKERS)
        + r")(?P<error>.*)$"
    ),
    sample="[2026-11-13 10:00:01.000] [warning] [t.cpp:514] Transparent execution: GPU execution "
    "failed at runtime; falling back to DuckDB CPU within the same transaction. GPU error: "
    "validity row_offset (3) not byte-aligned",
    extract=lambda m: {"error": m["error"].strip()},
    doc="the GPU failed mid-query and DuckDB re-ran it on the CPU",
)

# Printed to stdout (not the log) by both runtime fallback paths: print_cpu_fallback_banner().
FALLBACK_BANNER = "Error in Sirius GPU execution, fallback to DuckDB"

OPERATOR_PRODUCED = LogFormat(
    name="operator_produced",
    source="src/pipeline/gpu_pipeline_task.cpp:161",
    level="trace",
    pattern=re.compile(
        r"\[GPU:(?P<gpu>-?\d+)\] Pipeline (?P<pipeline>\S+): operator (?P<op>\S+) \(id=(?P<id>\d+)\) "
        r"task=(?P<task>\S+) produced (?P<batches>\d+) batches, .*execution time: (?P<ms>[0-9.]+) ms"
    ),
    sample="[2026-11-13 10:00:00.500] [trace] [t.cpp:161] [GPU:0] Pipeline 4: operator HASH_JOIN "
    "(id=12) task=77 produced 1 batches, num rows: 10  , size: 80 bytes (0.00 MB). execution "
    "time: 2.50 ms, peak allocated: 100 bytes (0.00 MB)",
    extract=lambda m: {"op": m["op"], "id": int(m["id"]), "ms": float(m["ms"])},
    doc="one operator task's output; the pair-task, hash-join and CONCAT measures",
)

PINNED_SCAN = LogFormat(
    name="pinned_scan",
    source="src/scan_manager/sirius_scan_manager.cpp:4062",
    level="info",
    pattern=re.compile(
        r"\[sirius_scan_manager\] assigned pinned entry '(?P<entry>[^']*)' to operator"
    ),
    sample="[2026-11-13 10:00:00.010] [info] [m.cpp:4062] [sirius_scan_manager] assigned pinned "
    "entry 'lineitem' to operator '3'",
    extract=lambda m: {"entry": m["entry"]},
    doc="a scan served from a pinned table; a P-stage query run without one read from disk",
)

PIN_COMPRESSING = LogFormat(
    name="pin_compressing",
    source="src/sirius_extension.cpp:1368",
    level="info",
    pattern=re.compile(
        r"\[pin_table\] '(?P<entry>[^']*)' tier=(?P<tier>\w+): compressing with plan"
    ),
    sample="[2026-11-13 10:00:00.000] [info] [e.cpp:1368] [pin_table] 'lineitem' tier=host: "
    "compressing with plan for 16 column(s)",
    extract=lambda m: {"entry": m["entry"]},
    doc="a pinned table the machine config compresses",
)

SEVERE = LogFormat(
    name="severe",
    source="any",
    level="error",
    pattern=re.compile(r"\] \[(?:error|critical)\] "),
    sample="[2026-11-13 10:00:00.000] [error] [x.cpp:1] something failed",
    doc="any error or critical line; counted and excerpted",
)

# Formats tried in this order; the first match wins (SEVERE and the excerpt test run on every line).
EVENT_FORMATS = (
    BUILD_FOLD,
    BLOOM_FINISH,
    PUBLICATION,
    CONSTRUCTION_STARTED,
    REPLICA_QUEUED,
    ZONE_REPLICATED,
    JOIN_STRATEGY,
    RESCHEDULE,
    RETRIES_EXCEEDED,
    DOWNGRADE_DEBUG,
    DOWNGRADE_MONITOR,
    DOWNGRADE_RESERVE,
    RESERVATION_FAILED,
    GPU_POOL,
    WINDOW,
    GPU_COMPLETED,
    PLAN_FALLBACK,
    RUNTIME_FALLBACK,
    PINNED_SCAN,
    PIN_COMPRESSING,
)
EXCERPT_FORMATS = (
    BLOOM_FINISH,
    PUBLICATION,
    REPLICA_PROBLEM,
    RESCHEDULE,
    RETRIES_EXCEEDED,
    DOWNGRADE_RESERVE,
    RESERVATION_FAILED,
    PLAN_FALLBACK,
    RUNTIME_FALLBACK,
    SEVERE,
)
ALL_FORMATS = (*EVENT_FORMATS, REPLICA_PROBLEM, OPERATOR_PRODUCED, SEVERE)


# ---------------------------------------------------------------------------------------------
# The Key Outcome line
# ---------------------------------------------------------------------------------------------

KEY_LINE_MARKER = "[dynamic_filter_key] "
KEY_LINE_VERSION = "1"
KEY_LINE_SOURCE = "src/op/dynamic_filter/dynamic_filter_key_outcome.cpp (debug)"
KEY_LINE_FIELDS = (
    Field("v", "string", "line version"),
    Field("sig", "string", "join signature"),
    Field("occ", "int32", "occurrence of the signature in the plan"),
    Field("join", "string", "operator id"),
    Field("condition", "int32"),
    Field("build_ordinal", "int32"),
    Field("rows", "int64"),
    Field("valid", "string"),
    Field("kind", "string", "none, bloom, small_in_list or hash_in_list"),
    Field("size", "int64"),
    Field("zone_map", "bool", "logged as 0 or 1"),
    Field("pushed", "int32"),
    Field("reason", "string"),
)
KEY_KINDS = ("none", "bloom", "small_in_list", "hash_in_list")
KEY_LINE_SAMPLE = (
    TS + "[dynamic_filter_key] v=1 sig=6b85214e8d4b623a occ=0 join=17 condition=0 "
    "build_ordinal=0 rows=100 valid=90 kind=bloom size=4096 zone_map=1 pushed=2 reason=-"
)


def parse_key_tokens(text: str) -> dict[str, str] | None:
    """The `name=value` tokens of the Key Outcome line in @p text, or None when it holds none.

    Raises FormatError when the line carries the marker but not exactly the version-1 fields.
    """
    start = text.find(KEY_LINE_MARKER)
    if start < 0:
        return None
    values: dict[str, str] = {}
    for token in text[start + len(KEY_LINE_MARKER) :].split():
        name, sep, value = token.partition("=")
        if not sep:
            raise FormatError(f"malformed token {token!r} in {text!r}")
        values[name] = value
    if tuple(values) != tuple(f.name for f in KEY_LINE_FIELDS):
        raise FormatError(f"unexpected fields {list(values)} in {text!r}")
    if values["v"] != KEY_LINE_VERSION:
        raise FormatError(f"unsupported line version {values['v']!r} in {text!r}")
    if values["kind"] not in KEY_KINDS:
        raise FormatError(f"unknown kind {values['kind']!r} in {text!r}")
    return values


def typed_key_row(tokens: dict[str, str]) -> dict[str, Any]:
    """The Key Outcome tokens converted to their declared types (the `v` field dropped)."""
    row: dict[str, Any] = {}
    for field in KEY_LINE_FIELDS[1:]:
        value = tokens[field.name]
        if field.type in ("int32", "int64"):
            row[field.name] = int(value)
        elif field.type == "bool":
            row[field.name] = value == "1"
        else:
            row[field.name] = value
    return row


# How a result matches its reference: the same rows in the same order, or only as multisets
# (a query with ORDER BY may legitimately reorder tied rows, or may have lost its ordering).
RESULT_ORDERED = "ordered"
RESULT_MULTISET = "multiset"


def runtime_fallback_error(text: str) -> str | None:
    """The GPU error when @p text is a runtime-fallback log line (`''` when it names none)."""
    parsed = RUNTIME_FALLBACK.parse(text)
    return None if parsed is None else parsed["error"]


# ---------------------------------------------------------------------------------------------
# The DuckDB CLI's stdout markers (duckdb d54cbbe3, tools/shell)
# ---------------------------------------------------------------------------------------------

# `.timer on 6` prints this through printf after every SQL statement, failed or not (shell.cpp:225).
TIMER = re.compile(r"^Run Time \(s\): real (?P<real>[0-9.]+)")
TIMER_SAMPLE = "Run Time (s): real 1.250000 user 0.100000 sys 0.000000"
# `.print @@S5B <step>` precedes each timed query of a session script (bench sessions).
STEP_MARKER = "@@S5B"
STEP_MARKER_RE = re.compile(r"^@@S5B (?P<step>\d+)$")


def appendix_markdown() -> str:
    """RUNBOOK.md Appendix C: every format with its source, level and a sample line."""
    lines = [
        "| name | level | source | what it is |",
        "|---|---|---|---|",
    ]
    for fmt in ALL_FORMATS:
        lines.append(f"| `{fmt.name}` | {fmt.level} | `{fmt.source}` | {fmt.doc} |")
    lines.append(
        f"| `dynamic_filter_key` | debug | `{KEY_LINE_SOURCE}` | the Key Outcome line: "
        + ", ".join(f"`{f.name}`" for f in KEY_LINE_FIELDS)
        + " |"
    )
    lines.append(
        "| `fallback_banner` | stdout | `print_cpu_fallback_banner()` | "
        f"`{FALLBACK_BANNER}` |"
    )
    lines.append(
        "| `timer` | stdout | `duckdb tools/shell/shell.cpp:225` | `.timer on 6` output, one per SQL "
        "statement |"
    )
    lines += ["", "Samples (one per format, as Sirius prints it):", "", "```"]
    lines += [fmt.sample for fmt in ALL_FORMATS]
    lines += [KEY_LINE_SAMPLE, TIMER_SAMPLE, "```"]
    return "\n".join(lines)
