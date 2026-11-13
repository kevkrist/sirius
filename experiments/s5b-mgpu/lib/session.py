# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
"""One timing session: a single long-lived DuckDB CLI process that runs a stage's query passes.

A session loads one build's extension with one config, creates the TPC-H views, pins every table
on the host tier (`CALL pin_table(<glob>, tier => 'host', name => <table>)`, compressed as the
config says), then runs `passes` passes over its cells and queries in the order `schedule` gives:

- pass 0 is the warm pass: each run logs at the session's warm level (debug or trace) and writes
  its result as CSV, which is compared with the CPU reference;
- passes 1.. are the hot passes: info-level logs, results discarded (`.mode trash`).

Each pass is a block that runs every cell once, in an order shuffled by `random.Random(seed)`, and
rotates the query order by a stride, so slow drift on the machine favors no cell and no query.
Before a cell's queries the script SETs every setting of that cell explicitly. Every run gets its
own `sirius_log_dir`, so OOM retries, downgrade bytes and Publication Sessions are counted per run.
A `.print @@S5B <step>` line precedes each timed query, so each `.timer` reading on stdout is
attributed to its step even when a run fails.

`execute` writes `record.json` (the session's only result) and then gzips the raw logs. A session
directory without `record.json` is unfinished: rerunning moves it to `.partial/` and starts over.
"""

from __future__ import annotations

import dataclasses
import json
import pathlib
import random
from typing import Any

import common
import duck
import formats


@dataclasses.dataclass(frozen=True)
class Cell:
    """One combination of SQL settings; P1 has a single cell with no settings."""

    label: str
    settings: tuple[tuple[str, str], ...] = ()
    experiment: str | None = None
    concat_divisor: int | None = None
    policy: str | None = None
    filters: str | None = None

    def settings_text(self) -> str:
        return " ".join(f"{name}={value}" for name, value in self.settings)

    def settings_strings(self) -> list[str]:
        return [f"{name}={value}" for name, value in self.settings]

    def meta(self) -> dict[str, Any]:
        return {
            "label": self.label,
            "settings": [list(pair) for pair in self.settings],
            "experiment": self.experiment,
            "concat_divisor": self.concat_divisor,
            "policy": self.policy,
            "filters": self.filters,
        }

    @staticmethod
    def from_meta(meta: dict[str, Any]) -> "Cell":
        return Cell(
            meta["label"],
            tuple(tuple(pair) for pair in meta["settings"]),
            meta["experiment"],
            meta["concat_divisor"],
            meta["policy"],
            meta["filters"],
        )


@dataclasses.dataclass(frozen=True)
class Spec:
    """Everything that defines a session; `session.json` locks it."""

    stage: str
    name: str
    role: str
    build: str
    gpus: int
    sf: int
    cells: tuple[Cell, ...]
    queries: tuple[str, ...]
    passes: int
    seed: int
    warm_log_level: str
    timeout_s: float
    stall_s: float

    def meta(self) -> dict[str, Any]:
        meta = dataclasses.asdict(self)
        meta["cells"] = [cell.meta() for cell in self.cells]
        meta["queries"] = list(self.queries)
        return meta

    @staticmethod
    def from_meta(meta: dict[str, Any]) -> "Spec":
        values = dict(meta)
        values["cells"] = tuple(Cell.from_meta(c) for c in meta["cells"])
        values["queries"] = tuple(meta["queries"])
        return Spec(**values)

    def cell(self, label: str) -> Cell:
        return next(cell for cell in self.cells if cell.label == label)


@dataclasses.dataclass(frozen=True)
class Step:
    index: int
    pass_: int
    cell: str
    query: str

    @property
    def kind(self) -> str:
        return "warm" if self.pass_ == 0 else "hot"


def schedule(
    cells: list[str], queries: list[str], passes: int, seed: int
) -> list[Step]:
    """Blocks of every cell once each, shuffled per pass; query order rotated per pass."""
    rng = random.Random(seed)
    stride = max(1, len(queries) // max(1, passes))
    steps: list[Step] = []
    for pass_ in range(passes):
        order = list(cells)
        rng.shuffle(order)
        offset = (pass_ * stride) % len(queries)
        rotated = queries[offset:] + queries[:offset]
        for cell in order:
            for query in rotated:
                steps.append(Step(len(steps), pass_, cell, query))
    return steps


def run_dir(directory: pathlib.Path, step: Step) -> pathlib.Path:
    return directory / "runs" / f"p{step.pass_}" / step.cell / step.query


def result_csv(directory: pathlib.Path, cell: str, query: str) -> pathlib.Path:
    return directory / "results" / cell / f"{query}.csv"


def script(
    spec: Spec,
    steps: list[Step],
    directory: pathlib.Path,
    query_texts: dict[str, str],
    preamble: list[str],
    pins: list[str],
) -> str:
    """The session's whole DuckDB CLI input."""
    lines = [".bail on", ".mode trash", *preamble]
    lines += [
        f"SET sirius_log_dir = {duck.sql_string(str(directory / 'setup'))};",
        *pins,
    ]
    # From here a failed query is recorded and the session goes on.
    lines.append(".bail off")
    current = None
    for step in steps:
        if step.cell != current:
            lines += [
                f"SET {name} = {value};"
                for name, value in spec.cell(step.cell).settings
            ]
            current = step.cell
        level = spec.warm_log_level if step.pass_ == 0 else "info"
        lines += [
            f"SET sirius_log_dir = {duck.sql_string(str(run_dir(directory, step)))};",
            f"SET sirius_log_level = {duck.sql_string(level)};",
        ]
        if step.pass_ == 0:
            lines += [".mode csv", ".headers on"]
        else:
            lines.append(".mode trash")
        lines.append(f".print {formats.STEP_MARKER} {step.index}")
        if step.pass_ == 0:
            lines.append(f'.once "{result_csv(directory, step.cell, step.query)}"')
        lines += [".timer on 6", query_texts[step.query], ".timer off"]
    # Switching the sink once more closes and flushes the last run's log file.
    lines.append(f"SET sirius_log_dir = {duck.sql_string(str(directory / 'end'))};")
    return "\n".join(lines) + "\n"


def step_latencies(stdout: str) -> dict[int, float]:
    """Milliseconds per step: the first `.timer` reading after each step marker."""
    latencies: dict[int, float] = {}
    current = None
    for line in stdout.splitlines():
        if match := formats.STEP_MARKER_RE.match(line.strip()):
            current = int(match["step"])
        elif current is not None and (timer := formats.TIMER.match(line)):
            latencies[current] = float(timer["real"]) * 1000.0
            current = None
    return latencies


def run_status(
    latency_ms: float | None, events: common.LogEvents, started: bool
) -> str:
    """`ok`, or why the run does not count: not_run, error, fallback or unpinned."""
    if not started:
        return "not_run"
    if latency_ms is None or events.windows_failed or not events.windows_ok:
        return "error"
    if events.fallback:
        return "fallback"
    if events.pinned_scans == 0:
        return "unpinned"
    return "ok"


def run_metrics(events: common.LogEvents) -> dict[str, Any]:
    return {
        "oom_retries": events.oom_retries,
        "reschedules": events.reschedules,
        "downgrade_bytes": events.downgrade_bytes,
        "reservation_shortfalls": events.reservation_shortfalls,
        "reservation_failures": events.reservation_failures,
        "pinned_scans": events.pinned_scans,
        "plan_fallback": bool(events.plan_fallbacks),
        "runtime_fallback": bool(events.runtime_fallbacks),
        "severe_lines": len(events.severe),
    }


def _first_timestamp(directory: pathlib.Path) -> float | None:
    for path in sorted(directory.glob("*.log")):
        with path.open(encoding="utf-8", errors="replace") as handle:
            for line in handle:
                if (stamp := common.parse_timestamp(line)) is not None:
                    return stamp
    return None


def session_dir(spec: Spec) -> pathlib.Path:
    return common.env_path("S5B_WORK") / spec.stage / spec.name


def record_path(spec: Spec) -> pathlib.Path:
    return session_dir(spec) / "record.json"


def load_record(path: pathlib.Path) -> dict[str, Any]:
    return json.loads(path.read_text())


def lock_spec(spec: Spec) -> None:
    """Refuses to reuse a session directory whose `session.json` records another spec."""
    path = session_dir(spec) / "session.json"
    if not path.is_file():
        return
    limits = ("timeout_s", "stall_s")  # limits may change between attempts
    recorded = {
        k: v for k, v in json.loads(path.read_text()).items() if k not in limits
    }
    wanted = {
        k: v for k, v in json.loads(json.dumps(spec.meta())).items() if k not in limits
    }
    if recorded != wanted:
        raise SystemExit(
            f"error: {path} records another spec; use a fresh work dir or session"
        )


def execute(spec: Spec) -> dict[str, Any]:
    """Runs the session and writes its `record.json`."""
    directory = session_dir(spec)
    if directory.exists():
        common.set_aside_partial(directory)
    directory.mkdir(parents=True)
    common.write_json_atomic(directory / "session.json", spec.meta())
    steps = schedule(
        [c.label for c in spec.cells], list(spec.queries), spec.passes, spec.seed
    )
    for step in steps:
        run_dir(directory, step).mkdir(parents=True)
    for cell in spec.cells:
        (directory / "results" / cell.label).mkdir(parents=True)
    (directory / "end").mkdir()
    texts = {
        q: (common.queries_dir(spec.sf) / f"{q}.sql").read_text().strip().rstrip(";")
        + ";"
        for q in spec.queries
    }
    table_files = duck.tables(spec.sf)
    pins = duck.pin_statements(table_files)
    text = script(
        spec, steps, directory, texts, duck.gpu_preamble(spec.build, spec.sf), pins
    )

    seen = [0]

    def progress() -> int:
        while seen[0] < len(steps) and any(
            run_dir(directory, steps[seen[0]]).iterdir()
        ):
            seen[0] += 1
        return seen[0]

    common.wait_for_idle_gpus()
    before = common.gpu_state(directory / "nvidia_smi_before.txt")
    common.log(
        f"{spec.stage}/{spec.name}: {len(steps)} query runs, timeout {spec.timeout_s:.0f} s"
    )
    cli = duck.run(
        spec.build,
        text,
        directory,
        config=common.config_file(spec.gpus),
        log_dir=directory / "setup",
        log_level="info",
        test_options=any(cell.settings for cell in spec.cells),
        timeout_s=spec.timeout_s,
        stall_s=spec.stall_s,
        progress=progress,
        progress_total=len(steps),
        watch=directory,
    )
    after = common.gpu_state(directory / "nvidia_smi_after.txt")
    record = build_record(spec, steps, directory, cli, before, after, len(pins))
    common.write_json_atomic(directory / "record.json", record)
    saved = common.compress_logs(directory)
    common.log(
        f"{spec.stage}/{spec.name}: {record['status']}; gzipped logs saved {saved / 2**20:.0f} MiB"
    )
    return record


def build_record(
    spec: Spec,
    steps: list[Step],
    directory: pathlib.Path,
    cli: duck.CliRun,
    before: list[dict[str, Any]],
    after: list[dict[str, Any]],
    pinned_tables: int,
) -> dict[str, Any]:
    latencies = step_latencies(cli.stdout)
    started_up_to = max(latencies, default=-1)
    setup = common.parse_log_dir(directory / "setup")
    peaks = dict(setup.gpu_peak_bytes)
    runs, warm, excerpt = [], [], list(setup.excerpt[:50])
    for step in steps:
        rdir = run_dir(directory, step)
        warm_run = step.pass_ == 0
        events = common.parse_log_dir(
            rdir, collect_operators=warm_run and spec.warm_log_level == "trace"
        )
        for gpu, peak in events.gpu_peak_bytes.items():
            peaks[gpu] = max(peaks.get(gpu, 0), peak)
        latency = latencies.get(step.index)
        started = step.index <= started_up_to or any(rdir.iterdir())
        status = run_status(latency, events, started)
        runs.append(
            {
                "pass": step.pass_,
                "kind": step.kind,
                "cell": step.cell,
                "query": step.query,
                "latency_ms": latency,
                "status": status,
                **run_metrics(events),
                "publications": events.publications,
            }
        )
        if warm_run:
            reference = common.reference_path(spec.sf, step.query)
            csv_path = result_csv(directory, step.cell, step.query)
            correctness = common.compare_to_reference(csv_path, reference)
            settings = common.settings_dict(spec.cell(step.cell).settings_strings())
            warm.append(
                {
                    "cell": step.cell,
                    "query": step.query,
                    "result_csv": str(csv_path),
                    **correctness,
                    "settings_problems": common.settings_problems(settings, events),
                    "build_folds": events.build_folds,
                    "bloom_transfers": events.bloom_transfers,
                    "join_strategies": events.join_strategies,
                    "operators": events.operator_summary(),
                }
            )
        excerpt += [
            f"p{step.pass_} {step.cell} {step.query}: {line}"
            for line in events.excerpt[:20]
        ]
    end = common.parse_log_dir(directory / "end")
    for gpu, peak in end.gpu_peak_bytes.items():
        peaks[gpu] = max(peaks.get(gpu, 0), peak)
    setup_start = _first_timestamp(directory / "setup")
    first_run = _first_timestamp(run_dir(directory, steps[0])) if steps else None
    failed_runs = [r for r in runs if r["status"] != "ok"]
    error = None
    if cli.status != "ok" or failed_runs:
        error = cli.stderr.strip()[-3000:] or None
    return {
        "spec": spec.meta(),
        "commit": common.git_head(common.worktree(spec.build)),
        "status": cli.status,
        "returncode": cli.returncode,
        "wall_s": cli.wall_s,
        "setup_s": first_run - setup_start if setup_start and first_run else None,
        "gpu_peak_bytes": max(peaks.values()) if peaks else None,
        "host_rss_peak_bytes": cli.host_rss_peak_bytes,
        "mem_available_min_bytes": cli.mem_available_min_bytes,
        "disk_peak_bytes": cli.disk_peak_bytes,
        "pinned_tables": pinned_tables,
        "compressed_tables": len(set(setup.compressed_pins)),
        "fallback_banner": cli.fallback_banner,
        "gpu_state": {"before": before, "after": after},
        "error": error,
        "runs": runs,
        "warm": warm,
        "excerpt": excerpt[:400],
    }
