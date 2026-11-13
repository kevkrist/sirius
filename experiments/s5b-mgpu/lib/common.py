# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
"""Shared pieces of the S5b multi-GPU experiment scripts.

Paths come from the environment that `experiments/s5b-mgpu/env.sh` exports. Log lines are matched
with the formats of `formats.py`; a format change shows up as a zero count, never a crash. Raw
output (full logs, CSV results) stays under ``S5B_WORK``; only Parquet tables (`schemas.py`),
``summary.md`` files and short excerpts go under ``S5B_RESULTS``.
"""

from __future__ import annotations

import csv
import dataclasses
import datetime
import gzip
import hashlib
import importlib.util
import json
import math
import os
import pathlib
import shutil
import subprocess
import sys
import time
from typing import Any, Iterable

import pyarrow as pa
import pyarrow.csv as pacsv
import pyarrow.parquet as pq

LIB_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(LIB_DIR))
import formats  # noqa: E402

S5B_DIR = LIB_DIR.parent
PARITY_SCRIPT = S5B_DIR / "tools" / "dynamic_filter_key_parity.py"
BUILDS = ("tip", "key", "base")
TPCH_QUERIES = tuple(f"q{i}" for i in range(1, 23))
TPCH_TABLES = (
    "customer",
    "lineitem",
    "nation",
    "orders",
    "part",
    "partsupp",
    "region",
    "supplier",
)
FLOAT_REL_TOL = 1e-9
FLOAT_ABS_TOL = 1e-12
# Significant digits kept for floating-point cells when hashing a result, so that GPU reduction
# order rarely changes the hash. Values on a rounding boundary still can; every comparison that
# decides a verdict falls back to the tolerant `tables_match`.
HASH_FLOAT_DIGITS = 10
# Exit status of a summary whose verdict cannot be given because runs are missing.
EXIT_INCOMPLETE = 3


# --------------------------------------------------------------------------------------------
# Environment and paths
# --------------------------------------------------------------------------------------------


def env_path(name: str) -> pathlib.Path:
    value = os.environ.get(name)
    if not value:
        sys.exit(
            f"error: {name} is not set; run through the experiments/s5b-mgpu shell scripts"
        )
    return pathlib.Path(value)


def worktree(build: str) -> pathlib.Path:
    if build not in BUILDS:
        sys.exit(f"error: unknown build {build!r}; expected one of {BUILDS}")
    return env_path(f"S5B_WT_{build}")


def duckdb_cli(build: str) -> pathlib.Path:
    return worktree(build) / "sirius-duckdb" / "build" / "release" / "duckdb"


def extension(build: str) -> pathlib.Path:
    release = worktree(build) / "sirius-duckdb" / "build" / "release"
    return release / "extension" / "sirius" / "sirius.duckdb_extension"


def config_file(gpus: int) -> pathlib.Path:
    path = env_path("S5B_CONFIGS") / f"sirius_{gpus}gpu.yaml"
    if not path.is_file():
        sys.exit(f"error: {path} is missing; run S0 'configs' first")
    return path


def tpch_dir(sf: int) -> pathlib.Path:
    override = os.environ.get(f"S5B_TPCH_SF{sf}")
    path = (
        pathlib.Path(override)
        if override
        else env_path("S5B_DATA") / f"tpch_parquet_sf{sf}"
    )
    if not path.is_dir():
        sys.exit(
            f"error: TPC-H SF{sf} Parquet directory {path} is missing; see S0 'data'"
        )
    return path


def queries_dir(sf: int) -> pathlib.Path:
    path = env_path("S5B_QUERIES") / f"sf{sf}"
    if not path.is_dir():
        sys.exit(f"error: {path} is missing; run S0 'queries' first")
    return path


def reference_path(sf: int, query: str) -> pathlib.Path:
    return env_path("S5B_WORK") / "reference" / f"sf{sf}" / f"{query}.parquet"


def results_dir(stage: str) -> pathlib.Path:
    path = env_path("S5B_RESULTS") / stage
    path.mkdir(parents=True, exist_ok=True)
    return path


def work_dir(*parts: str) -> pathlib.Path:
    path = env_path("S5B_WORK").joinpath(*parts)
    path.mkdir(parents=True, exist_ok=True)
    return path


def git_head(path: pathlib.Path) -> str:
    return subprocess.run(
        ["git", "-C", str(path), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


def resolved_settings(build: str, gpus: int) -> dict[str, str]:
    """The settings S0 'probe' read back from a loaded Sirius (hash_partition_bytes and others)."""
    path = env_path("S5B_WORK") / "settings" / f"{build}_{gpus}gpu.json"
    if not path.is_file():
        sys.exit(f"error: {path} is missing; run S0 'probe' first")
    return json.loads(path.read_text())


def load_parity_module():
    """The key-parity script, imported by path (this directory is not a package)."""
    spec = importlib.util.spec_from_file_location(
        "dynamic_filter_key_parity", PARITY_SCRIPT
    )
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def clean_env(**intended: str) -> dict[str, str]:
    """The current environment without any `SIRIUS_*` or `CUDA_VISIBLE_DEVICES`, plus @p intended.

    Every Sirius process the kit starts sees only the variables the kit means it to see.
    """
    env = {
        name: value
        for name, value in os.environ.items()
        if not name.startswith("SIRIUS_") and name != "CUDA_VISIBLE_DEVICES"
    }
    env.update(intended)
    return env


# --------------------------------------------------------------------------------------------
# Small file helpers
# --------------------------------------------------------------------------------------------


def now_iso() -> str:
    return datetime.datetime.now().astimezone().isoformat(timespec="seconds")


def log(message: str) -> None:
    print(f"[{datetime.datetime.now():%H:%M:%S}] {message}", flush=True)


def write_text_atomic(path: pathlib.Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(text)
    tmp.replace(path)


def write_json_atomic(path: pathlib.Path, value: Any) -> None:
    write_text_atomic(
        path, json.dumps(value, indent=2, sort_keys=True, default=str) + "\n"
    )


def set_aside_partial(directory: pathlib.Path) -> None:
    """Moves an unfinished `<stage>/<parent>/<name>` directory to `<stage>/.partial/`.

    The graveyard sits beside the parent directories, never inside one, so a parent directory
    only ever holds finished or running children.
    """
    if not directory.exists():
        return
    graveyard = directory.parent.parent / ".partial"
    graveyard.mkdir(exist_ok=True)
    stamp = f"{datetime.datetime.now():%Y%m%d-%H%M%S}"
    target = graveyard / f"{directory.parent.name}--{directory.name}-{stamp}"
    shutil.move(str(directory), str(target))
    log(f"moved unfinished {directory} to {target}")


def write_excerpt(path: pathlib.Path, lines: Iterable[str], limit: int = 400) -> None:
    """Writes at most @p limit lines, each cut to 600 characters, to keep results/ small."""
    kept = []
    total = 0
    for line in lines:
        total += 1
        if len(kept) < limit:
            kept.append(line[:600])
    if total > limit:
        kept.append(f"... {total - limit} more matching line(s) omitted")
    write_text_atomic(path, "\n".join(kept) + ("\n" if kept else ""))


def geomean(values: Iterable[float | None]) -> float | None:
    logs = [math.log(v) for v in values if v and v > 0 and math.isfinite(v)]
    return math.exp(sum(logs) / len(logs)) if logs else None


def median(values: Iterable[float | None]) -> float | None:
    clean = sorted(v for v in values if v is not None and math.isfinite(v))
    if not clean:
        return None
    middle = len(clean) // 2
    return clean[middle] if len(clean) % 2 else (clean[middle - 1] + clean[middle]) / 2


def fmt(value: float | None, digits: int = 1) -> str:
    return "-" if value is None else f"{value:.{digits}f}"


def pct(value: float | None) -> str:
    """A ratio as a signed percentage: 1.034 -> +3.4%."""
    return "-" if value is None else f"{(value - 1.0) * 100:+.1f}%"


def compress_logs(directory: pathlib.Path) -> int:
    """Gzips every `*.log` under @p directory in place; returns the bytes saved."""
    saved = 0
    for path in directory.rglob("*.log"):
        packed = path.with_name(path.name + ".gz")
        with path.open("rb") as source, gzip.open(
            packed, "wb", compresslevel=6
        ) as target:
            shutil.copyfileobj(source, target)
        saved += path.stat().st_size - packed.stat().st_size
        path.unlink()
    return saved


# --------------------------------------------------------------------------------------------
# GPU guard and GPU state
# --------------------------------------------------------------------------------------------


def gpu_compute_pids() -> list[str]:
    completed = subprocess.run(
        ["nvidia-smi", "--query-compute-apps=pid", "--format=csv,noheader"],
        capture_output=True,
        text=True,
    )
    return [line.strip() for line in completed.stdout.splitlines() if line.strip()]


def wait_for_idle_gpus(timeout_s: float = 180.0) -> None:
    """Blocks until no process holds a GPU context, so two engines never overlap.

    The step wrapper (`env.sh` `s5b_as_step`) refuses to start next to another engine; this wait
    only covers the driver releasing the previous process's contexts.
    """
    deadline = time.monotonic() + timeout_s
    while True:
        pids = gpu_compute_pids()
        if not pids:
            return
        if time.monotonic() > deadline:
            sys.exit(
                f"error: GPU processes {pids} still running after {timeout_s:.0f} s; another "
                "engine is using the GPUs. Stop and ask the user."
            )
        time.sleep(2.0)


# nvmlClocksEventReasons bits (nvml.h).
THROTTLE_BITS = (
    (0x1, "gpu_idle"),
    (0x2, "applications_clocks_setting"),
    (0x4, "sw_power_cap"),
    (0x8, "hw_slowdown"),
    (0x10, "sync_boost"),
    (0x20, "sw_thermal_slowdown"),
    (0x40, "hw_thermal_slowdown"),
    (0x80, "hw_power_brake_slowdown"),
    (0x100, "display_clock_setting"),
)
GPU_STATE_QUERY = (
    "index,clocks.sm,clocks.mem,clocks.max.sm,temperature.gpu,power.draw,pstate"
)


def decode_throttle(mask: str) -> str:
    """Names of the bits set in an nvidia-smi clocks event reasons mask such as `0x...04`."""
    try:
        value = int(mask, 16)
    except ValueError:
        return "unknown"
    names = [name for bit, name in THROTTLE_BITS if value & bit]
    known = sum(bit for bit, _ in THROTTLE_BITS)
    if value & ~known:
        names.append(f"other_0x{value & ~known:x}")
    return ",".join(names) or "none"


def _int_or_none(text: str) -> int | None:
    try:
        return int(float(text))
    except ValueError:
        return None


def parse_gpu_state(csv_text: str) -> list[dict[str, Any]]:
    """Rows of `nvidia-smi --query-gpu=<GPU_STATE_QUERY>,<reasons> --format=csv,noheader,nounits`."""
    rows = []
    for line in csv_text.splitlines():
        cells = [c.strip() for c in line.split(",")]
        if len(cells) != 8:
            continue
        index, sm, mem, max_sm, temp, power, pstate, mask = cells
        try:
            power_w = float(power)
        except ValueError:
            power_w = None
        rows.append(
            {
                "gpu": _int_or_none(index),
                "sm_clock_mhz": _int_or_none(sm),
                "mem_clock_mhz": _int_or_none(mem),
                "max_sm_clock_mhz": _int_or_none(max_sm),
                "temperature_c": _int_or_none(temp),
                "power_w": power_w,
                "pstate": pstate,
                "throttle_mask": mask,
                "throttle_reasons": decode_throttle(mask),
            }
        )
    return rows


def gpu_state(raw_out: pathlib.Path | None = None) -> list[dict[str, Any]]:
    """Clocks, temperature and throttle reasons of every GPU; also saves `nvidia-smi -q` text."""
    rows: list[dict[str, Any]] = []
    for reasons in ("clocks_event_reasons.active", "clocks_throttle_reasons.active"):
        completed = subprocess.run(
            [
                "nvidia-smi",
                f"--query-gpu={GPU_STATE_QUERY},{reasons}",
                "--format=csv,noheader,nounits",
            ],
            capture_output=True,
            text=True,
        )
        if completed.returncode == 0:
            rows = parse_gpu_state(completed.stdout)
            break
    if raw_out is not None:
        detail = subprocess.run(
            ["nvidia-smi", "-q", "-d", "CLOCK,PERFORMANCE,TEMPERATURE,POWER"],
            capture_output=True,
            text=True,
        )
        write_text_atomic(raw_out, detail.stdout + detail.stderr)
    return rows


# --------------------------------------------------------------------------------------------
# Sirius log parsing
# --------------------------------------------------------------------------------------------


def parse_timestamp(text: str) -> float | None:
    match = formats.LINE_PREFIX.match(text)
    if not match:
        return None
    return datetime.datetime.strptime(match["ts"], "%Y-%m-%d %H:%M:%S.%f").timestamp()


@dataclasses.dataclass
class LogEvents:
    """Everything the stages extract from one directory of Sirius logs."""

    build_folds: list[dict[str, Any]] = dataclasses.field(default_factory=list)
    bloom_transfers: list[dict[str, Any]] = dataclasses.field(default_factory=list)
    publications: list[dict[str, Any]] = dataclasses.field(default_factory=list)
    join_strategies: list[dict[str, Any]] = dataclasses.field(default_factory=list)
    operators: list[dict[str, Any]] = dataclasses.field(default_factory=list)
    constructions_started: int = 0
    replica_copies: int = 0
    replica_problems: list[str] = dataclasses.field(default_factory=list)
    reschedules: int = 0
    oom_retries: int = 0
    retries_exceeded: int = 0
    downgrade_bytes_debug: int = 0
    downgrade_requests_debug: int = 0
    downgrade_bytes_monitor: int = 0
    downgrade_bytes_reserve: int = 0
    reservation_shortfalls: int = 0
    reservation_failures: int = 0
    gpu_peak_bytes: dict[int, int] = dataclasses.field(default_factory=dict)
    windows_ok: int = 0
    windows_failed: int = 0
    gpu_completed: int = 0
    pinned_scans: int = 0
    compressed_pins: list[str] = dataclasses.field(default_factory=list)
    runtime_fallbacks: list[str] = dataclasses.field(default_factory=list)
    plan_fallbacks: list[str] = dataclasses.field(default_factory=list)
    first_window_begin: float | None = None
    severe: list[str] = dataclasses.field(default_factory=list)
    excerpt: list[str] = dataclasses.field(default_factory=list)
    key_lines: list[str] = dataclasses.field(default_factory=list)

    @property
    def downgrade_bytes(self) -> int:
        """All downgraded bytes: the per-request total when logged (debug), else the info lines."""
        if self.downgrade_requests_debug:
            return self.downgrade_bytes_debug
        return self.downgrade_bytes_monitor + self.downgrade_bytes_reserve

    @property
    def max_gpu_peak_bytes(self) -> int | None:
        return max(self.gpu_peak_bytes.values()) if self.gpu_peak_bytes else None

    @property
    def fallback(self) -> bool:
        return bool(self.plan_fallbacks or self.runtime_fallbacks)

    def operator_summary(self) -> list[dict[str, Any]]:
        """Per (operator name, id): task count, summed time, first output after window begin."""
        summary: dict[tuple[str, int], dict[str, Any]] = {}
        for event in self.operators:
            entry = summary.setdefault(
                (event["op"], event["id"]),
                {
                    "operator": event["op"],
                    "operator_id": event["id"],
                    "tasks": 0,
                    "total_ms": 0.0,
                    "first_output_s": None,
                },
            )
            entry["tasks"] += 1
            entry["total_ms"] += event["ms"]
            if event["ts"] is not None and self.first_window_begin is not None:
                offset = event["ts"] - self.first_window_begin
                if entry["first_output_s"] is None or offset < entry["first_output_s"]:
                    entry["first_output_s"] = offset
        return list(summary.values())


def _log_files(directory: pathlib.Path) -> list[pathlib.Path]:
    return sorted([*directory.glob("*.log"), *directory.glob("*.log.gz")])


def parse_log_dir(
    directory: pathlib.Path, collect_operators: bool = False
) -> LogEvents:
    """Parses every `*.log` (or gzipped `*.log.gz`) file directly in @p directory."""
    events = LogEvents()
    for path in _log_files(directory):
        opener = gzip.open if path.suffix == ".gz" else open
        with opener(path, "rt", encoding="utf-8", errors="replace") as handle:
            for raw in handle:
                parse_line(raw.rstrip("\n"), events, collect_operators)
    return events


def parse_line(text: str, events: LogEvents, collect_operators: bool) -> None:
    if formats.KEY_LINE_MARKER in text:
        events.key_lines.append(text)
        return
    if "produced" in text and "[trace]" in text:
        if collect_operators and (parsed := formats.OPERATOR_PRODUCED.parse(text)):
            events.operators.append({**parsed, "ts": parse_timestamp(text)})
        return
    if any(fmt.pattern.search(text) for fmt in formats.EXCERPT_FORMATS):
        events.excerpt.append(text)
    if formats.REPLICA_PROBLEM.pattern.search(text):
        events.replica_problems.append(text)
    if formats.SEVERE.pattern.search(text):
        events.severe.append(text)
    for fmt in formats.EVENT_FORMATS:
        parsed = fmt.parse(text)
        if parsed is not None:
            _record(fmt, parsed, text, events)
            return


def _record(
    fmt: formats.LogFormat, parsed: dict[str, Any], text: str, events: LogEvents
) -> None:
    if fmt is formats.BUILD_FOLD:
        events.build_folds.append(parsed)
    elif fmt is formats.BLOOM_FINISH:
        events.bloom_transfers.append(parsed)
    elif fmt is formats.PUBLICATION:
        events.publications.append(parsed)
    elif fmt is formats.JOIN_STRATEGY:
        events.join_strategies.append(parsed)
    elif fmt is formats.CONSTRUCTION_STARTED:
        events.constructions_started += 1
    elif fmt in (formats.REPLICA_QUEUED, formats.ZONE_REPLICATED):
        events.replica_copies += 1
    elif fmt is formats.RESCHEDULE:
        events.reschedules += 1
        events.oom_retries += int(parsed["oom"])
    elif fmt is formats.RETRIES_EXCEEDED:
        events.retries_exceeded += 1
    elif fmt is formats.DOWNGRADE_DEBUG:
        events.downgrade_requests_debug += 1
        events.downgrade_bytes_debug += parsed["bytes"]
    elif fmt is formats.DOWNGRADE_MONITOR:
        events.downgrade_bytes_monitor += parsed["bytes"]
    elif fmt is formats.DOWNGRADE_RESERVE:
        events.reservation_shortfalls += 1
        events.downgrade_bytes_reserve += parsed["freed"]
        events.reservation_failures += int(not parsed["satisfied"])
    elif fmt is formats.RESERVATION_FAILED:
        events.reservation_failures += 1
    elif fmt is formats.GPU_POOL:
        gpu = parsed["gpu"]
        events.gpu_peak_bytes[gpu] = max(
            events.gpu_peak_bytes.get(gpu, 0), parsed["peak"]
        )
    elif fmt is formats.WINDOW:
        if parsed["event"] == "begin":
            if events.first_window_begin is None:
                events.first_window_begin = parse_timestamp(text)
        elif parsed["outcome"] == "ok":
            events.windows_ok += 1
        else:
            events.windows_failed += 1
    elif fmt is formats.GPU_COMPLETED:
        events.gpu_completed += 1
    elif fmt is formats.PLAN_FALLBACK:
        events.plan_fallbacks.append(text)
    elif fmt is formats.RUNTIME_FALLBACK:
        events.runtime_fallbacks.append(text)
    elif fmt is formats.PINNED_SCAN:
        events.pinned_scans += 1
    elif fmt is formats.PIN_COMPRESSING:
        events.compressed_pins.append(parsed["entry"])


def settings_dict(settings: Iterable[str]) -> dict[str, str]:
    """`name=value` SET strings as a dict, with SQL quotes stripped from the values."""
    result = {}
    for setting in settings:
        name, _, value = setting.partition("=")
        result[name.strip()] = value.strip().strip("'")
    return result


def settings_problems(settings: dict[str, str], events: LogEvents) -> list[str]:
    """Logged facts that contradict the settings a run SET (finding L1).

    Only debug and trace logs carry the `[build_fold]` and join strategy lines, so run this on
    warm or parity runs.
    """
    problems = []
    policy = settings.get("build_fold_policy")
    concat = settings.get("concat_batch_bytes")
    for fold in events.build_folds:
        if policy is not None and fold["fold_policy"] != policy:
            problems.append(
                f"join {fold['join_id']} logged policy={fold['fold_policy']}, SET {policy}"
            )
        if concat is not None and str(fold["concat_batch_bytes"]) != concat:
            problems.append(
                f"join {fold['join_id']} logged concat_batch_bytes={fold['concat_batch_bytes']}, "
                f"SET {concat}"
            )
    if settings.get("max_build_hash_table_bytes") == "0":
        problems += [
            f"join {j['join_id']} ran BUILD_PROBE with max_build_hash_table_bytes=0"
            for j in events.join_strategies
            if j["mode"] == "BUILD_PROBE"
        ]
    if settings.get("enable_dynamic_filter") == "false":
        if events.constructions_started or events.publications or events.key_lines:
            problems.append(
                "Dynamic Filters were constructed with enable_dynamic_filter=false"
            )
    return problems


# --------------------------------------------------------------------------------------------
# Query results: canonical hashing and comparison against the CPU reference
# --------------------------------------------------------------------------------------------


def read_reference(path: pathlib.Path) -> pa.Table:
    return pq.read_table(path)


def read_result_csv(path: pathlib.Path, schema: pa.Schema | None) -> pa.Table:
    """Reads a DuckDB CLI CSV result, typed like the reference when one is given."""
    convert = pacsv.ConvertOptions(
        column_types=schema,
        null_values=[""],
        strings_can_be_null=True,
        quoted_strings_can_be_null=False,
    )
    return pacsv.read_csv(path, convert_options=convert)


def canonical_rows(table: pa.Table) -> list[tuple[str, ...]]:
    """Rows as strings: floats rounded to HASH_FLOAT_DIGITS significant digits, NULL as ''."""
    columns = []
    for column in table.columns:
        values = column.to_pylist()
        if pa.types.is_floating(column.type):
            columns.append(
                [
                    "" if v is None else format(v, f".{HASH_FLOAT_DIGITS}g")
                    for v in values
                ]
            )
        else:
            columns.append(["" if v is None else str(v) for v in values])
    return list(zip(*columns)) if columns else []


def result_hash(table: pa.Table) -> str:
    """Order-insensitive SHA-256 over column names and canonical rows (informational)."""
    digest = hashlib.sha256()
    digest.update("\x1f".join(table.column_names).encode())
    for row in sorted(canonical_rows(table)):
        digest.update(b"\x1e" + "\x1f".join(row).encode())
    return digest.hexdigest()[:16]


def _cell_equal(left: Any, right: Any, floating: bool) -> bool:
    if left is None or right is None:
        return left is None and right is None
    if floating:
        if math.isnan(left) and math.isnan(right):
            return True
        return math.isclose(left, right, rel_tol=FLOAT_REL_TOL, abs_tol=FLOAT_ABS_TOL)
    return left == right


ORDERED = formats.RESULT_ORDERED
MULTISET = formats.RESULT_MULTISET


def tables_match(candidate: pa.Table, reference: pa.Table) -> tuple[str | None, str]:
    """How the tables agree (floats within tolerance): `ordered`, `multiset` or None, and why.

    `multiset` means the same rows in another order. It is reported separately because a query
    with ORDER BY may legitimately reorder tied rows, or may have lost its ordering.
    """
    if candidate.column_names != reference.column_names:
        return (
            None,
            f"columns differ: {candidate.column_names} vs {reference.column_names}",
        )
    if candidate.num_rows != reference.num_rows:
        return None, f"row counts differ: {candidate.num_rows} vs {reference.num_rows}"
    floating = [pa.types.is_floating(column.type) for column in reference.columns]
    rows_left = list(zip(*(c.to_pylist() for c in candidate.columns)))
    rows_right = list(zip(*(c.to_pylist() for c in reference.columns)))

    def equal(rows_a, rows_b) -> bool:
        return all(
            _cell_equal(a, b, f)
            for row_a, row_b in zip(rows_a, rows_b)
            for a, b, f in zip(row_a, row_b, floating)
        )

    if equal(rows_left, rows_right):
        return ORDERED, "equal"

    def sort_key(row):
        return tuple("" if v is None else str(v) for v in row)

    if equal(sorted(rows_left, key=sort_key), sorted(rows_right, key=sort_key)):
        return MULTISET, "equal as multisets (row order differs)"
    return None, "values differ"


def compare_to_reference(
    result_csv: pathlib.Path, reference: pathlib.Path | None
) -> dict[str, Any]:
    """Hashes a CLI result and checks it against the CPU reference Parquet, when present."""
    outcome: dict[str, Any] = {
        "result_hash": None,
        "ref_match": None,
        "ref_order": None,
        "ref_detail": None,
        "result_rows": None,
    }
    if not result_csv.is_file():
        outcome["ref_detail"] = "no result file"
        outcome["ref_match"] = (
            False if reference is not None and reference.is_file() else None
        )
        return outcome
    schema = None
    ref_table = None
    if reference is not None and reference.is_file():
        ref_table = read_reference(reference)
        schema = ref_table.schema
    try:
        table = read_result_csv(result_csv, schema)
    except (pa.ArrowInvalid, pa.ArrowTypeError, KeyError) as error:
        outcome["ref_match"] = False if ref_table is not None else None
        outcome["ref_detail"] = (
            f"result CSV does not parse with the reference types: {error}"
        )
        return outcome
    outcome["result_hash"] = result_hash(table)
    outcome["result_rows"] = table.num_rows
    if ref_table is None:
        outcome["ref_detail"] = "no reference"
        return outcome
    order, outcome["ref_detail"] = tables_match(table, ref_table)
    outcome["ref_match"] = order is not None
    outcome["ref_order"] = order
    return outcome


def csv_results_match(
    left: pathlib.Path, right: pathlib.Path, schema: pa.Schema | None
) -> bool:
    """Tolerant comparison of two CLI results, in either row order."""
    try:
        return (
            tables_match(read_result_csv(left, schema), read_result_csv(right, schema))[
                0
            ]
            is not None
        )
    except (pa.ArrowInvalid, pa.ArrowTypeError, OSError):
        return False


def read_csv_dicts(path: pathlib.Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))
