#!/usr/bin/env python3
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
"""Latency stages P1 (no regression, with P4), P2 (fold matrix) and P3 (fold re-check).

Every stage runs sessions (`session.py`): one long-lived DuckDB process per build and GPU count,
with every TPC-H table pinned on the host tier, 1 warm pass and 5 hot passes. The latency metric
is the median of a query's hot runs.

- **P1**, per GPU count: sessions `base_a`, `tip`, `base_b` in that order. The two base sessions
  are an A/A pair: `base_b / base_a` per query is the noise shown next to every bar, and the base
  latency is their geometric mean. A query over the 1.10 bar gets one confirming pair of sessions
  (`base_confirm`, `tip_confirm`) over the flagged queries, and the verdict uses the confirmed
  ratio.
- **P2 and P3**: one session `matrix` whose cells are the fold settings, run with SET in
  shuffled blocks (`session.schedule`, recorded seed). A (cell, query) over a bar (10% slower
  than `never`, or more OOM retries or downgrade bytes than `never`) gets one confirming session
  `confirm` over the flagged cells, their `never` partners and the flagged queries, and the
  decision uses the confirmed values.

`run` is resumable at session granularity: a session with `record.json` is skipped, an unfinished
one is set aside and rerun, and the confirming sessions start when their inputs exist.
`summarize` writes the `schemas.py` tables and `summary.md`, and exits 0 (PASS), 1 (FAIL) or 3
(INCOMPLETE: a session, cell or query is missing, or a confirmation is pending).

Subcommands: `plan`, `run`, `summarize`.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import itertools
import json
import os
import pathlib
import secrets
import sys
from typing import Any

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import common  # noqa: E402
import schemas  # noqa: E402
import session  # noqa: E402
from schemas import merge_row  # noqa: E402

PASSES = 6  # 1 warm + 5 hot
SLOW_BAR = 1.10  # no query more than 10% slower
P1_GEOMEAN_BAR = 1.02
FOLD_GEOMEAN_BAR = 0.98  # a candidate must beat never by more than 2%
WATCH_QUERIES = ("q2", "q15", "q21")
POLICY_SHORT = {
    "never": "never",
    "legacy_filter_coupled": "legacy",
    "bounded": "bounded",
}
STAGES: dict[str, dict[str, Any]] = {
    "P1": {
        "sf": 1000,
        "gpus": "4,1",
        "warm_log_level": "debug",
        "timeout": 6 * 3600.0,
        "stall": 3600.0,
        "assume_query_s": 25.0,
        "assume_setup_s": 1200.0,
    },
    "P2": {
        "sf": 100,
        "gpus": "1",
        "experiments": "E1,E2",
        "concat": "2,8,0",
        "policies": "never,legacy_filter_coupled,bounded",
        "filters": "on,off",
        "warm_log_level": "trace",
        "timeout": 14 * 3600.0,
        "stall": 1800.0,
        "assume_query_s": 3.0,
        "assume_setup_s": 300.0,
    },
    "P3": {
        "sf": 1000,
        "gpus": "4",
        "experiments": "E1,E2",
        "concat": "8",
        "policies": None,
        "filters": "on",
        "warm_log_level": "trace",
        "timeout": 10 * 3600.0,
        "stall": 3600.0,
        "assume_query_s": 20.0,
        "assume_setup_s": 1200.0,
    },
}
P1_ROLES = (("base_a", "base"), ("tip", "tip"), ("base_b", "base"))
P1_CONFIRM_ROLES = (("base_confirm", "base"), ("tip_confirm", "tip"))
DEFAULT_CELL = "default"


# ---------------------------------------------------------------------------------------------
# Session specs
# ---------------------------------------------------------------------------------------------


def gpu_counts(args: argparse.Namespace) -> list[int]:
    return [int(value) for value in str(args.gpus).split(",")]


def queries_of(args: argparse.Namespace) -> tuple[str, ...]:
    return tuple(args.queries.split(",")) if args.queries else common.TPCH_QUERIES


def cell_label(experiment: str, divisor: int, policy: str, filters: str) -> str:
    return f"{experiment}_c{divisor}_{POLICY_SHORT[policy]}_{filters}"


def fold_cells(args: argparse.Namespace, gpus: int) -> list[session.Cell]:
    """The settings cells of P2/P3; every cell SETs all four varied settings explicitly."""
    if not args.policies:
        sys.exit("error: this stage needs --policies (P3: never,<P2 winner>)")
    resolved = common.resolved_settings("tip", gpus)
    h = int(resolved["hash_partition_bytes"])
    default_table_bytes = resolved["max_build_hash_table_bytes"]
    cells = []
    for experiment, divisor, policy, filters in itertools.product(
        args.experiments.split(","),
        (int(d) for d in args.concat.split(",")),
        args.policies.split(","),
        args.filters.split(","),
    ):
        if experiment not in ("E1", "E2"):
            sys.exit(f"error: unknown experiment {experiment!r}")
        if policy == "legacy_filter_coupled" and filters == "off":
            continue  # identical to never when no join publishes filters
        settings = (
            ("build_fold_policy", f"'{policy}'"),
            ("concat_batch_bytes", str(0 if divisor == 0 else h // divisor)),
            (
                "max_build_hash_table_bytes",
                "0" if experiment == "E2" else default_table_bytes,
            ),
            ("enable_dynamic_filter", "true" if filters == "on" else "false"),
        )
        # Optional fixed Bloom Filter budget per GPU for every cell (local.env), SET explicitly.
        bloom_bytes = os.environ.get("S5B_MAX_BLOOM_BYTES_PER_GPU")
        if bloom_bytes:
            settings += (("max_dynamic_filter_bloom_bytes_per_gpu", str(int(bloom_bytes))),)
        cells.append(
            session.Cell(
                cell_label(experiment, divisor, policy, filters),
                settings,
                experiment,
                divisor,
                policy,
                filters,
            )
        )
    return cells


def _spec(stage, args, name, role, build, gpus, cells, queries, seed) -> session.Spec:
    return session.Spec(
        stage=stage,
        name=name,
        role=role,
        build=build,
        gpus=gpus,
        sf=args.sf,
        cells=tuple(cells),
        queries=tuple(queries),
        passes=PASSES,
        seed=seed,
        warm_log_level=args.warm_log_level,
        timeout_s=args.timeout,
        stall_s=args.stall,
    )


def recorded_seed(stage: str, name: str) -> int | None:
    path = common.env_path("S5B_WORK") / stage / name / "session.json"
    return json.loads(path.read_text())["seed"] if path.is_file() else None


def main_specs(stage: str, args: argparse.Namespace, gpus: int) -> list[session.Spec]:
    if stage == "P1":
        cell = session.Cell(DEFAULT_CELL)
        return [
            _spec(
                stage,
                args,
                f"{role}_{gpus}gpu",
                role,
                build,
                gpus,
                [cell],
                queries_of(args),
                0,
            )
            for role, build in P1_ROLES
        ]
    name = f"matrix_{gpus}gpu"
    seed = recorded_seed(stage, name)
    if seed is None:
        seed = args.seed if args.seed is not None else secrets.randbits(31)
    cells = fold_cells(args, gpus)
    return [
        _spec(stage, args, name, "matrix", "tip", gpus, cells, queries_of(args), seed)
    ]


def confirm_specs(
    stage: str, args: argparse.Namespace, gpus: int, records: dict[str, dict]
) -> list[session.Spec]:
    """The confirming sessions the main sessions call for; empty when nothing is flagged."""
    if stage == "P1":
        flagged = [
            row["query"]
            for row in p1_compare(records, gpus)
            if row["ratio"] is not None and row["ratio"] > SLOW_BAR
        ]
        if not flagged:
            return []
        cell = session.Cell(DEFAULT_CELL)
        return [
            _spec(
                stage, args, f"{role}_{gpus}gpu", role, build, gpus, [cell], flagged, 0
            )
            for role, build in P1_CONFIRM_ROLES
        ]
    main = records.get(f"matrix_{gpus}gpu")
    if main is None:
        return []
    rows = fold_compare(records, gpus)
    flagged = [row for row in rows if row["flagged"]]
    if not flagged:
        return []
    spec = session.Spec.from_meta(main["spec"])
    labels = set()
    for row in flagged:
        labels.add(
            cell_label(
                row["experiment"], row["concat_divisor"], row["policy"], row["filters"]
            )
        )
        labels.add(
            cell_label(
                row["experiment"], row["concat_divisor"], "never", row["filters"]
            )
        )
    cells = [cell for cell in spec.cells if cell.label in labels]
    queries = [q for q in spec.queries if q in {row["query"] for row in flagged}]
    return [
        _spec(
            stage,
            args,
            f"confirm_{gpus}gpu",
            "confirm",
            "tip",
            gpus,
            cells,
            queries,
            spec.seed + 1,
        )
    ]


def load_records(stage: str) -> dict[str, dict]:
    """Finished sessions of @p stage by session name."""
    root = common.env_path("S5B_WORK") / stage
    return {
        path.parent.name: session.load_record(path)
        for path in sorted(root.glob("*/record.json"))
        if not path.parent.name.startswith(".")
    }


# ---------------------------------------------------------------------------------------------
# plan and run
# ---------------------------------------------------------------------------------------------


def plan(stage: str, args: argparse.Namespace) -> int:
    records = load_records(stage)
    rates = [
        (r["wall_s"] - (r["setup_s"] or 0)) / max(1, len(r["runs"]))
        for r in records.values()
        if r["status"] == "ok"
    ]
    setups = [r["setup_s"] for r in records.values() if r["setup_s"]]
    per_run = sum(rates) / len(rates) if rates else args.assume_query_s
    setup = max(setups) if setups else args.assume_setup_s
    pending_s = 0.0
    for gpus in gpu_counts(args):
        for spec in main_specs(stage, args, gpus):
            runs = len(spec.cells) * len(spec.queries) * spec.passes
            done = spec.name in records
            if not done:
                pending_s += setup + runs * per_run
            print(
                f"{spec.name:<18} {spec.build} {gpus} GPU SF{spec.sf}: {len(spec.cells)} cell(s) x "
                f"{len(spec.queries)} queries x {spec.passes} passes = {runs} runs; seed {spec.seed}; "
                f"{'done (' + records[spec.name]['status'] + ')' if done else 'pending'}"
            )
    source = "measured" if rates else "assumed"
    print(
        f"estimate for pending main sessions: {pending_s / 3600:.1f} h ({source} {per_run:.1f} s/run, "
        f"setup {setup / 60:.0f} min); confirming sessions add time only when a query is flagged"
    )
    import check_data  # noqa: PLC0415 - only plan needs the data footers

    data = check_data.check(args.sf)
    mem_total = (
        (os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES"))
        if hasattr(os, "sysconf")
        else 0
    )
    print(
        f"SF{args.sf} data: {data['bytes'] / 2**30:.0f} GiB Parquet, "
        f"{(data['uncompressed_bytes'] or 0) / 2**30:.0f} GiB uncompressed column chunks to pin on the "
        f"host tier; host RAM {mem_total / 2**30:.0f} GiB"
    )
    return 0


def _needs_run(
    spec: session.Spec, records: dict[str, dict], retry_failed: bool
) -> bool:
    record = records.get(spec.name)
    if record is None:
        return True
    return retry_failed and record["status"] != "ok"


def run(stage: str, args: argparse.Namespace) -> int:
    missing = [
        q for q in queries_of(args) if not common.reference_path(args.sf, q).is_file()
    ]
    if missing:
        sys.exit(
            f"error: no CPU reference at SF{args.sf} for {missing}; run `reference` first"
        )
    failures = 0
    for gpus in gpu_counts(args):
        records = load_records(stage)
        for spec in main_specs(stage, args, gpus):
            session.lock_spec(spec)
            if _needs_run(spec, records, args.retry_failed):
                failures += int(session.execute(spec)["status"] != "ok")
        records = load_records(stage)
        if not all(spec.name in records for spec in main_specs(stage, args, gpus)):
            continue
        for spec in confirm_specs(stage, args, gpus, records):
            session.lock_spec(spec)
            if _needs_run(spec, records, args.retry_failed):
                common.log(
                    f"{stage}: confirming {len(spec.queries)} flagged query(ies) in {spec.name}"
                )
                failures += int(session.execute(spec)["status"] != "ok")
    common.log(f"{stage}: run finished; {failures} session(s) not ok. Next: summarize")
    return 1 if failures else 0


# ---------------------------------------------------------------------------------------------
# Tables
# ---------------------------------------------------------------------------------------------


def session_context(record: dict) -> dict[str, Any]:
    spec = record["spec"]
    return {
        "stage": spec["stage"],
        "session": spec["name"],
        "role": spec["role"],
        "build": spec["build"],
        "commit": record["commit"],
        "gpus": spec["gpus"],
        "sf": spec["sf"],
    }


def cell_context(cell: session.Cell) -> dict[str, Any]:
    return {
        "label": cell.label,
        "experiment": cell.experiment,
        "concat_divisor": cell.concat_divisor,
        "policy": cell.policy,
        "filters": cell.filters,
        "settings": cell.settings_text(),
    }


def spread(values: list[float]) -> float | None:
    middle = common.median(values)
    return (max(values) - min(values)) / middle if values and middle else None


def query_row(runs: list[dict], warm: dict | None) -> dict[str, Any]:
    hot = [
        r["latency_ms"]
        for r in runs
        if r["kind"] == "hot" and r["latency_ms"] is not None
    ]
    warm_run = next((r for r in runs if r["kind"] == "warm"), None)
    bad = [r["status"] for r in runs if r["status"] != "ok"]
    operators = (warm or {}).get("operators") or []
    joins = [o for o in operators if o["operator"] == "HASH_JOIN"]
    concats = [o for o in operators if o["operator"] == "CONCAT"]
    firsts = [o["first_output_s"] for o in joins if o["first_output_s"] is not None]
    folds = (warm or {}).get("build_folds") or []
    problems = (warm or {}).get("settings_problems") or []
    return {
        "status": bad[0] if bad else "ok",
        "warm_ms": warm_run["latency_ms"] if warm_run else None,
        "median_hot_ms": common.median(hot),
        "min_hot_ms": min(hot) if hot else None,
        "max_hot_ms": max(hot) if hot else None,
        "hot_runs": len(hot),
        "hot_spread": spread(hot),
        "result_hash": (warm or {}).get("result_hash"),
        "result_rows": (warm or {}).get("result_rows"),
        "ref_match": (warm or {}).get("ref_match"),
        "ref_order": (warm or {}).get("ref_order"),
        "ref_detail": (warm or {}).get("ref_detail"),
        "fallback": any(r["plan_fallback"] or r["runtime_fallback"] for r in runs),
        "oom_retries": sum(r["oom_retries"] for r in runs),
        "reschedules": sum(r["reschedules"] for r in runs),
        "downgrade_bytes": sum(r["downgrade_bytes"] for r in runs),
        "reservation_shortfalls": sum(r["reservation_shortfalls"] for r in runs),
        "reservation_failures": sum(r["reservation_failures"] for r in runs),
        "pinned_scans_min": min((r["pinned_scans"] for r in runs), default=None),
        "settings_problems": "; ".join(problems) or None,
        "pair_tasks": sum(o["tasks"] for o in joins) if operators else None,
        "hash_join_ms": sum(o["total_ms"] for o in joins) if operators else None,
        "concat_ms": sum(o["total_ms"] for o in concats) if operators else None,
        "first_pair_s": min(firsts) if firsts else None,
        "folds": sum(f["fold"] for f in folds),
        "fold_decisions": len(folds),
    }


RUN_COLUMNS = (
    "latency_ms",
    "status",
    "oom_retries",
    "reschedules",
    "downgrade_bytes",
    "reservation_shortfalls",
    "reservation_failures",
    "pinned_scans",
    "plan_fallback",
    "runtime_fallback",
    "severe_lines",
)
SESSION_COLUMNS = (
    "status",
    "returncode",
    "wall_s",
    "setup_s",
    "gpu_peak_bytes",
    "host_rss_peak_bytes",
    "mem_available_min_bytes",
    "disk_peak_bytes",
    "pinned_tables",
    "compressed_tables",
)


def tabulate(records: dict[str, dict]) -> dict[str, list[dict[str, Any]]]:
    tables: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
    for record in records.values():
        spec = session.Spec.from_meta(record["spec"])
        context = session_context(record)
        tables["sessions"].append(
            merge_row(
                context,
                {k: record[k] for k in SESSION_COLUMNS},
                {
                    "seed": spec.seed,
                    "cells": len(spec.cells),
                    "queries": len(spec.queries),
                    "passes": spec.passes,
                    "error": (record["error"] or "")[-500:] or None,
                },
            )
        )
        for moment, rows in record["gpu_state"].items():
            for row in rows:
                tables["gpu_state"].append(
                    merge_row(
                        {"stage": spec.stage, "session": spec.name, "moment": moment},
                        row,
                    )
                )
        warm = {(w["cell"], w["query"]): w for w in record["warm"]}
        grouped: dict[tuple[str, str], list[dict]] = collections.defaultdict(list)
        for run_ in record["runs"]:
            grouped[(run_["cell"], run_["query"])].append(run_)
            where = merge_row(
                context, cell_context(spec.cell(run_["cell"])), {"query": run_["query"]}
            )
            keys = {"pass": run_["pass"], "kind": run_["kind"]}
            tables["runs"].append(
                merge_row(where, keys, {k: run_[k] for k in RUN_COLUMNS})
            )
            for publication in run_["publications"]:
                tables["publications"].append(merge_row(where, keys, publication))
        for (cell, query), runs in grouped.items():
            where = merge_row(context, cell_context(spec.cell(cell)), {"query": query})
            entry = warm.get((cell, query))
            tables["queries"].append(merge_row(where, query_row(runs, entry)))
            if entry is None:
                continue
            for fold in entry["build_folds"]:
                tables["build_folds"].append(merge_row(where, fold))
            for operator in entry["operators"]:
                tables["operators"].append(merge_row(where, operator))
            for event in entry["bloom_transfers"]:
                tables["bloom_transfers"].append(merge_row(where, event))
            for event in entry["join_strategies"]:
                tables["join_strategies"].append(merge_row(where, event))
    return tables


def query_index(records: dict[str, dict]) -> dict[tuple[str, str, str], dict]:
    """Query rows by (session, cell, query)."""
    return {
        (row["session"], row["label"], row["query"]): row
        for row in tabulate(records)["queries"]
    }


def hot_by_pass(records: dict[str, dict]) -> dict[tuple[str, str, str, int], float]:
    """Hot-run latency by (session, cell, query, pass)."""
    return {
        (name, run_["cell"], run_["query"], run_["pass"]): run_["latency_ms"]
        for name, record in records.items()
        for run_ in record["runs"]
        if run_["kind"] == "hot"
        and run_["latency_ms"] is not None
        and run_["status"] == "ok"
    }


def warm_index(records: dict[str, dict]) -> dict[tuple[str, str, str], dict]:
    return {
        (name, w["cell"], w["query"]): w
        for name, record in records.items()
        for w in record["warm"]
    }


def same_result(left: dict | None, right: dict | None, sf: int, query: str) -> bool:
    """Whether two warm results agree: equal hashes, or else equal within float tolerance."""
    if not left or not right:
        return False
    if left.get("result_hash") and left["result_hash"] == right.get("result_hash"):
        return True
    reference = common.reference_path(sf, query)
    schema = common.read_reference(reference).schema if reference.is_file() else None
    return common.csv_results_match(
        pathlib.Path(left["result_csv"]), pathlib.Path(right["result_csv"]), schema
    )


def ok_ms(row: dict | None) -> float | None:
    return row["median_hot_ms"] if row and row["status"] == "ok" else None


def ratio_of(numerator: float | None, denominator: float | None) -> float | None:
    return numerator / denominator if numerator and denominator else None


# ---------------------------------------------------------------------------------------------
# P1
# ---------------------------------------------------------------------------------------------


def p1_compare(records: dict[str, dict], gpus: int) -> list[dict[str, Any]]:
    index = query_index(records)
    warm = warm_index(records)
    rows = []
    sf = next((r["spec"]["sf"] for r in records.values()), None)
    for query in common.TPCH_QUERIES:

        def get(role: str) -> dict | None:
            return index.get((f"{role}_{gpus}gpu", DEFAULT_CELL, query))

        a, t, b = get("base_a"), get("tip"), get("base_b")
        bc, tc = get("base_confirm"), get("tip_confirm")
        if not (a or t or b):
            continue
        base_values = [v for v in (ok_ms(a), ok_ms(b)) if v]
        base_ms = common.geomean(base_values)
        ratio = ratio_of(ok_ms(t), base_ms)
        confirmed = ratio_of(ok_ms(tc), ok_ms(bc))
        tip_warm = warm.get((f"tip_{gpus}gpu", DEFAULT_CELL, query))
        base_warm = warm.get((f"base_a_{gpus}gpu", DEFAULT_CELL, query))
        rows.append(
            {
                "sf": sf,
                "gpus": gpus,
                "query": query,
                "base_a_ms": ok_ms(a),
                "tip_ms": ok_ms(t),
                "base_b_ms": ok_ms(b),
                "base_ms": base_ms,
                "ratio": ratio,
                "aa_ratio": ratio_of(ok_ms(b), ok_ms(a)),
                "base_confirm_ms": ok_ms(bc),
                "tip_confirm_ms": ok_ms(tc),
                "confirmed_ratio": confirmed,
                "final_ratio": confirmed if confirmed is not None else ratio,
                "identical": same_result(base_warm, tip_warm, sf or 0, query),
                "base_ref_match": a["ref_match"] if a else None,
                "tip_ref_match": t["ref_match"] if t else None,
                "tip_ref_order": t["ref_order"] if t else None,
                "base_oom_retries": a["oom_retries"] if a else None,
                "tip_oom_retries": t["oom_retries"] if t else None,
                "base_downgrade_bytes": a["downgrade_bytes"] if a else None,
                "tip_downgrade_bytes": t["downgrade_bytes"] if t else None,
            }
        )
    return rows


def per_pass_geomeans(
    passes: dict[tuple[str, str, str, int], float],
    pairs: list[tuple[tuple[str, str, str], list[tuple[str, str, str]]]],
) -> list[float]:
    """Geomean ratio of each hot pass: candidate over the geomean of its baselines, per query."""
    values = []
    for pass_ in range(1, PASSES):
        ratios = []
        for candidate, baselines in pairs:
            top = passes.get((*candidate, pass_))
            bottom = common.geomean(passes.get((*b, pass_)) for b in baselines)
            ratios.append(ratio_of(top, bottom))
        if (geo := common.geomean(ratios)) is not None:
            values.append(geo)
    return values


@dataclasses.dataclass
class Verdict:
    state: str  # PASS, FAIL or INCOMPLETE
    reasons: list[str]

    @staticmethod
    def of(incomplete: list[str], failures: list[str]) -> "Verdict":
        if incomplete:
            return Verdict("INCOMPLETE", incomplete + failures)
        return Verdict("FAIL" if failures else "PASS", failures)


def summarize_p1(
    stage: str, records: dict, tables: dict, out: pathlib.Path, args
) -> Verdict:
    passes = hot_by_pass(records)
    compare, lines, verdicts = [], [], []
    expected = gpu_counts(args)
    index = query_index(records)
    for gpus in expected:
        rows = p1_compare(records, gpus)
        compare += rows
        incomplete, failures = [], []
        for role, _ in P1_ROLES:
            name = f"{role}_{gpus}gpu"
            if name not in records:
                incomplete.append(f"session {name} has not run")
        present = {r["query"] for r in rows}
        missing = [q for q in common.TPCH_QUERIES if q not in present]
        if missing:
            incomplete.append(f"queries missing: {missing}")
        flagged = [r for r in rows if r["ratio"] is not None and r["ratio"] > SLOW_BAR]
        unconfirmed = [r["query"] for r in flagged if r["confirmed_ratio"] is None]
        if unconfirmed:
            incomplete.append(f"over {SLOW_BAR} and not yet confirmed: {unconfirmed}")
        broken = [
            f"{role} {q}: {row['status']}"
            for role, _ in P1_ROLES
            for q in common.TPCH_QUERIES
            if (row := index.get((f"{role}_{gpus}gpu", DEFAULT_CELL, q)))
            and row["status"] != "ok"
        ]
        if broken:
            failures.append(f"runs failed, fell back or read from disk: {broken[:20]}")
        wrong = [
            r["query"]
            for r in rows
            if not (r["identical"] and r["base_ref_match"] and r["tip_ref_match"])
        ]
        if wrong:
            failures.append(f"results not identical or not matching the CPU: {wrong}")
        geo = common.geomean(r["final_ratio"] for r in rows)
        if geo is not None and geo > P1_GEOMEAN_BAR:
            failures.append(f"geomean {geo:.4f} > {P1_GEOMEAN_BAR}")
        slow = [
            r["query"] for r in rows if r["final_ratio"] and r["final_ratio"] > SLOW_BAR
        ]
        if slow:
            failures.append(f"queries over {SLOW_BAR} after confirmation: {slow}")
        verdict = Verdict.of(incomplete, failures)
        verdicts.append(verdict)
        aa = [r["aa_ratio"] for r in rows if r["aa_ratio"]]
        aa_dev = sorted(abs(v - 1) for v in aa)
        pairs = [
            (
                (f"tip_{gpus}gpu", DEFAULT_CELL, q),
                [
                    (f"base_a_{gpus}gpu", DEFAULT_CELL, q),
                    (f"base_b_{gpus}gpu", DEFAULT_CELL, q),
                ],
            )
            for q in common.TPCH_QUERIES
        ]
        band = per_pass_geomeans(passes, pairs)
        aa_pairs = [
            (
                (f"base_b_{gpus}gpu", DEFAULT_CELL, q),
                [(f"base_a_{gpus}gpu", DEFAULT_CELL, q)],
            )
            for q in common.TPCH_QUERIES
        ]
        worst = max(rows, key=lambda r: r["final_ratio"] or 0, default=None)
        multiset = [r["query"] for r in rows if r["tip_ref_order"] == common.MULTISET]
        lines += [
            f"## {gpus} GPU(s): {verdict.state}",
            "",
            *[f"- {reason}" for reason in verdict.reasons],
            f"- geomean tip/base: {common.fmt(geo, 4)} (bar <= {P1_GEOMEAN_BAR}); noise: A/A geomean "
            f"base_b/base_a {common.fmt(common.geomean(aa), 4)}, per-pass tip/base geomeans "
            f"{common.fmt(min(band, default=None), 4)}..{common.fmt(max(band, default=None), 4)}, "
            f"per-pass A/A geomeans {common.fmt(min(per_pass_geomeans(passes, aa_pairs), default=None), 4)}"
            f"..{common.fmt(max(per_pass_geomeans(passes, aa_pairs), default=None), 4)}",
            f"- worst query: {worst['query'] if worst else '-'} at "
            f"{common.fmt(worst['final_ratio'] if worst else None, 3)} (bar <= {SLOW_BAR}); noise: A/A "
            f"per query median {common.fmt(common.median(aa_dev) * 100 if aa_dev else None)}%, max "
            f"{common.fmt(max(aa_dev) * 100 if aa_dev else None)}%",
            f"- confirmed by re-run: "
            + (
                ", ".join(
                    f"{r['query']} {common.fmt(r['ratio'], 3)} -> {common.fmt(r['confirmed_ratio'], 3)}"
                    for r in flagged
                )
                or "none needed"
            ),
            f"- tip results equal to the CPU only as multisets (row order differs; check ORDER BY ties): "
            f"{multiset or 'none'}",
            "",
            "| query | base_a ms | tip ms | base_b ms | tip/base | A/A | confirmed | identical | CPU match base/tip |",
            "|---|---|---|---|---|---|---|---|---|",
        ]
        for r in rows:
            mark = " **" if r["query"] in WATCH_QUERIES else ""
            lines.append(
                f"| {r['query']}{mark} | {common.fmt(r['base_a_ms'])} | {common.fmt(r['tip_ms'])} | "
                f"{common.fmt(r['base_b_ms'])} | {common.fmt(r['ratio'], 3)} | {common.pct(r['aa_ratio'])} | "
                f"{common.fmt(r['confirmed_ratio'], 3)} | {r['identical']} | "
                f"{r['base_ref_match']}/{r['tip_ref_match']} |"
            )
        lines += [
            "",
            "Watch list (q2, q15, q21): "
            + "; ".join(
                f"{r['query']} {common.fmt(r['final_ratio'], 3)}"
                for r in rows
                if r["query"] in WATCH_QUERIES
            ),
            "",
            *p1_resources(records, gpus),
        ]
    schemas.write_table("P1.compare", compare, out / "compare.parquet")
    states = {v.state for v in verdicts}
    overall = Verdict(
        (
            "INCOMPLETE"
            if "INCOMPLETE" in states
            else "FAIL" if "FAIL" in states else "PASS"
        ),
        [],
    )
    header = [
        f"# {stage} summary: base against tip, TPC-H SF{args.sf}, host-pinned tables",
        "",
        f"Generated {common.now_iso()}. Latency: median of 5 hot runs in one process per build and "
        "GPU count. Base: geometric mean of the two base processes (A/A).",
        "",
        f"**Overall: {overall.state}** (GPU counts {expected})",
        "",
    ]
    lines = header + lines + p4_section(tables) + sessions_section(tables)
    common.write_text_atomic(out / "summary.md", "\n".join(lines) + "\n")
    return overall


def p1_resources(records: dict, gpus: int) -> list[str]:
    """Base against tip memory evidence; information only, not a pass bar."""
    lines = [
        f"### {gpus} GPU(s): memory, base against tip (information only; not a pass bar)",
        "",
        "| session | peak GPU memory (process) | OOM retries (all queries) | downgrade GiB (all queries) |",
        "|---|---|---|---|",
    ]
    for role, _ in P1_ROLES:
        record = records.get(f"{role}_{gpus}gpu")
        if record is None:
            lines.append(f"| {role} | not run | - | - |")
            continue
        peak = record["gpu_peak_bytes"]
        oom = sum(r["oom_retries"] for r in record["runs"])
        downgrade = sum(r["downgrade_bytes"] for r in record["runs"])
        lines.append(
            f"| {role} | {common.fmt(peak / 2**30 if peak else None, 2)} GiB | {oom} | "
            f"{common.fmt(downgrade / 2**30, 2)} |"
        )
    return lines + [""]


def p4_section(tables) -> list[str]:
    """P4: publication latency and Bloom transfer paths on the multi-GPU tip process."""

    def tip_multi(row: dict) -> bool:
        return row["role"] == "tip" and row["gpus"] > 1

    pubs = [p for p in tables["publications"] if tip_multi(p)]
    measured = [p for p in pubs if p["filters_pushed"] > 0 and p["latency_ms"] > 0.0]
    blooms = [b for b in tables["bloom_transfers"] if tip_multi(b)]

    def quantiles(kind: str) -> str:
        values = sorted(p["latency_ms"] for p in measured if p["kind"] == kind)
        if not values:
            return f"{kind}: none"

        def at(q: float) -> float:
            return values[min(len(values) - 1, int(q * len(values)))]

        return f"{kind}: n={len(values)}, p50 {at(0.5):.2f}, p95 {at(0.95):.2f}, max {values[-1]:.2f}"

    transport = collections.Counter(b["transport"] for b in blooms)
    return [
        "## P4: Dynamic Filter publication on the multi-GPU tip process",
        "",
        f"- Publication Sessions logged: {len(pubs)}; with Bloom Filters through HOST staging: "
        f"{sum(p['host_staged'] for p in pubs)}; left out of the latency figures (0 filters pushed "
        f"or no latency): {len(pubs) - len(measured)}",
        f"- publication latency after the final Contribution, ms: {quantiles('warm')} (debug log, "
        f"includes one-time costs); {quantiles('hot')}",
        f"- Bloom Filter reductions by transfer path (warm runs): {dict(transport) or 'none logged'}",
        "- The counters `accumulation_publication_latency_ns` and `accumulation_publications_host_staged` "
        "are not exposed outside C++ tests; these figures come from the `publication` and "
        "`bloom_finish` log lines (Appendix C).",
        "",
    ]


def sessions_section(tables) -> list[str]:
    lines = [
        "## Processes",
        "",
        "| session | status | wall min | setup min | peak GPU GiB | host RSS GiB | min MemAvailable GiB | disk GiB | pinned (compressed) |",
        "|---|---|---|---|---|---|---|---|---|",
    ]

    def gib(value):
        return common.fmt(value / 2**30 if value else None, 1)

    for s in tables["sessions"]:
        lines.append(
            f"| {s['session']} | {s['status']} | {common.fmt(s['wall_s'] / 60)} | "
            f"{common.fmt(s['setup_s'] / 60 if s['setup_s'] else None)} | {gib(s['gpu_peak_bytes'])} | "
            f"{gib(s['host_rss_peak_bytes'])} | {gib(s['mem_available_min_bytes'])} | "
            f"{gib(s['disk_peak_bytes'])} | {s['pinned_tables']} ({s['compressed_tables']}) |"
        )
    throttled = sorted(
        {
            f"{g['session']} {g['moment']} GPU {g['gpu']}: {g['throttle_reasons']}"
            for g in tables["gpu_state"]
            if g["throttle_reasons"] not in ("none", "gpu_idle")
        }
    )
    clocks = collections.defaultdict(list)
    for g in tables["gpu_state"]:
        clocks[g["moment"]].append(g["sm_clock_mhz"])
    lines += [
        "",
        f"- GPU throttle reasons other than idle (`gpu_state.parquet`): {throttled or 'none'}",
        "- SM clock range before/after processes, MHz: "
        + "; ".join(
            f"{moment} {min(v for v in values if v is not None)}..{max(v for v in values if v is not None)}"
            for moment, values in sorted(clocks.items())
            if any(v is not None for v in values)
        ),
        "",
    ]
    return lines


# ---------------------------------------------------------------------------------------------
# P2 and P3
# ---------------------------------------------------------------------------------------------


def fold_compare(records: dict[str, dict], gpus: int) -> list[dict[str, Any]]:
    main_name, confirm_name = f"matrix_{gpus}gpu", f"confirm_{gpus}gpu"
    main = records.get(main_name)
    if main is None:
        return []
    spec = session.Spec.from_meta(main["spec"])
    index = query_index(records)
    warm = warm_index(records)
    rows = []
    for cell in spec.cells:
        if cell.policy == "never":
            continue
        never_label = cell_label(
            cell.experiment, cell.concat_divisor, "never", cell.filters
        )
        for query in spec.queries:
            row = index.get((main_name, cell.label, query))
            never = index.get((main_name, never_label, query))
            if row is None or never is None:
                continue
            ok = row["status"] == "ok" and never["status"] == "ok"
            ratio = ratio_of(ok_ms(row), ok_ms(never))
            oom = row["oom_retries"] - never["oom_retries"]
            downgrade = row["downgrade_bytes"] - never["downgrade_bytes"]
            flagged = ok and ((ratio or 0) > SLOW_BAR or oom > 0 or downgrade > 0)
            c_row = index.get((confirm_name, cell.label, query))
            c_never = index.get((confirm_name, never_label, query))
            confirmed = bool(
                c_row
                and c_never
                and c_row["status"] == "ok"
                and c_never["status"] == "ok"
            )
            c_ratio = ratio_of(ok_ms(c_row), ok_ms(c_never)) if confirmed else None
            c_oom = c_row["oom_retries"] - c_never["oom_retries"] if confirmed else None
            c_downgrade = (
                c_row["downgrade_bytes"] - c_never["downgrade_bytes"]
                if confirmed
                else None
            )
            noise = [
                v for v in (row["hot_spread"], never["hot_spread"]) if v is not None
            ]
            rows.append(
                {
                    "experiment": cell.experiment,
                    "concat_divisor": cell.concat_divisor,
                    "filters": cell.filters,
                    "policy": cell.policy,
                    "query": query,
                    "never_ms": ok_ms(never),
                    "median_ms": ok_ms(row),
                    "ratio": ratio,
                    "noise": max(noise) if noise else None,
                    "same_result_as_never": same_result(
                        warm.get((main_name, cell.label, query)),
                        warm.get((main_name, never_label, query)),
                        spec.sf,
                        query,
                    ),
                    "ref_match": row["ref_match"],
                    "oom_over_never": oom,
                    "downgrade_over_never": downgrade,
                    "folds": row["folds"],
                    "flagged": flagged,
                    "confirmed": confirmed,
                    "confirmed_ratio": c_ratio,
                    "confirmed_oom_over_never": c_oom,
                    "confirmed_downgrade_over_never": c_downgrade,
                    "final_ratio": c_ratio if confirmed else ratio,
                    "final_oom_over_never": c_oom if confirmed else oom,
                    "final_downgrade_over_never": (
                        c_downgrade if confirmed else downgrade
                    ),
                }
            )
    return rows


def summarize_fold(
    stage: str, records: dict, tables: dict, out: pathlib.Path, args
) -> Verdict:
    """Applies the spec's fold decision criteria (Build fold rule, 'Decision criteria')."""
    gpus = gpu_counts(args)[0]
    main_name = f"matrix_{gpus}gpu"
    expected = fold_cells(args, gpus)
    index = query_index(records)
    passes = hot_by_pass(records)
    compare = fold_compare(records, gpus)
    schemas.write_table("P2.compare", compare, out / "compare.parquet")
    incomplete, failures = [], []
    if main_name not in records:
        incomplete.append(f"session {main_name} has not run")
    else:
        recorded = {c["label"] for c in records[main_name]["spec"]["cells"]}
        missing_cells = [c.label for c in expected if c.label not in recorded]
        if missing_cells:
            incomplete.append(f"cells not in {main_name}: {missing_cells}")
    expected_rows = [(c.label, q) for c in expected for q in queries_of(args)]
    missing = [(c, q) for c, q in expected_rows if (main_name, c, q) not in index]
    if missing:
        incomplete.append(
            f"{len(missing)} of {len(expected_rows)} (cell, query) pairs missing"
        )
    pending = [r for r in compare if r["flagged"] and not r["confirmed"]]
    if pending:
        incomplete.append(
            f"{len(pending)} flagged (cell, query) pair(s) not yet confirmed"
        )
    present = [
        index[(main_name, c, q)] for c, q in expected_rows if (main_name, c, q) in index
    ]
    wrong = [
        f"{r['label']} {r['query']}: {r['status']}, ref_match={r['ref_match']}"
        + (f", settings: {r['settings_problems']}" if r["settings_problems"] else "")
        for r in present
        if r["status"] != "ok" or not r["ref_match"] or r["settings_problems"]
    ]
    correct = not wrong
    lines = [
        f"1. Correctness: every run completes on the GPU from pinned tables, matches the CPU, and "
        f"logs the settings its cell SET: {correct}",
        *[f"   - {w}" for w in wrong[:30]],
    ]
    candidates = sorted({r["policy"] for r in compare})
    chosen, best = "never", None
    for policy in candidates:
        mine = [r for r in compare if r["policy"] == policy]
        disqualified = [
            r
            for r in mine
            if (r["final_oom_over_never"] or 0) > 0
            or (r["final_downgrade_over_never"] or 0) > 0
        ]
        geo_all = common.geomean(r["final_ratio"] for r in mine)
        geo_e = {
            e: common.geomean(r["final_ratio"] for r in mine if r["experiment"] == e)
            for e in ("E1", "E2")
        }
        band = {}
        for e in ("E1", "E2"):
            pairs = [
                (
                    (
                        main_name,
                        cell_label(e, r["concat_divisor"], policy, r["filters"]),
                        r["query"],
                    ),
                    [
                        (
                            main_name,
                            cell_label(e, r["concat_divisor"], "never", r["filters"]),
                            r["query"],
                        )
                    ],
                )
                for r in mine
                if r["experiment"] == e
            ]
            values = per_pass_geomeans(passes, pairs)
            band[e] = (min(values, default=None), max(values, default=None))
        worst = max(mine, key=lambda r: r["final_ratio"] or 0, default=None)
        experiments = {r["experiment"] for r in mine}
        beats = experiments == {"E1", "E2"} and all(
            g is not None and g < FOLD_GEOMEAN_BAR for g in geo_e.values()
        )
        eligible = (
            correct
            and not disqualified
            and worst is not None
            and (worst["final_ratio"] or 0) <= SLOW_BAR
            and beats
        )
        lines += [
            "",
            f"## {policy}",
            "",
            f"- (1) result differs from `never` beyond float tolerance (informational; the CPU match "
            f"decides): {sum(not r['same_result_as_never'] for r in mine)}",
            f"- (2) (cell, query) pairs with more OOM retries or downgrade bytes than `never`, after "
            f"confirmation: {len(disqualified)}"
            + "".join(
                f"\n  - {r['experiment']} c{r['concat_divisor']} {r['filters']} {r['query']}: oom "
                f"+{r['final_oom_over_never']}, downgrade +{r['final_downgrade_over_never']} B"
                f"{' (confirmed)' if r['confirmed'] else ''}"
                for r in disqualified[:20]
            ),
            (
                f"- (3) geomean against never, all cells: {common.fmt(geo_all, 4)}; worst: "
                f"{worst['experiment']} c{worst['concat_divisor']} {worst['filters']} {worst['query']} "
                f"at {common.fmt(worst['final_ratio'], 3)} (bar <= {SLOW_BAR}; noise: hot-run spread "
                f"{common.fmt((worst['noise'] or 0) * 100)}%"
                f"{', confirmed' if worst['confirmed'] else ''})"
                if worst
                else "- (3) no comparable cells"
            ),
            f"- (4) geomean against never: E1 {common.fmt(geo_e['E1'], 4)} (noise: per-pass "
            f"{common.fmt(band['E1'][0], 4)}..{common.fmt(band['E1'][1], 4)}), E2 "
            f"{common.fmt(geo_e['E2'], 4)} (per-pass {common.fmt(band['E2'][0], 4)}.."
            f"{common.fmt(band['E2'][1], 4)}); both must be < {FOLD_GEOMEAN_BAR}",
            f"- confirmed by re-run: {sum(r['confirmed'] for r in mine)} of "
            f"{sum(r['flagged'] for r in mine)} flagged pair(s)",
            f"- eligible: {eligible}",
        ]
        if eligible and (best is None or geo_all < best):
            chosen, best = policy, geo_all
    verdict = Verdict.of(incomplete, [] if correct else ["correctness failed"])
    decision = (
        "INCOMPLETE"
        if verdict.state == "INCOMPLETE"
        else f"`{chosen}`"
        + ("" if correct else " (correctness failed, so the fallback applies)")
    )
    lines += fold_width_section(tables)
    header = [
        f"# {stage} summary: fold policies against `never`, TPC-H SF{args.sf}, {gpus} GPU(s)",
        "",
        f"Generated {common.now_iso()}. Latency: median of 5 hot runs; cells run with SET in "
        f"shuffled blocks (seed {records[main_name]['spec']['seed'] if main_name in records else '-'}).",
        "",
        f"**Decision (criteria 1-5): {decision}**",
        "",
        *[f"- {reason}" for reason in verdict.reasons],
        "",
    ]
    common.write_text_atomic(
        out / "summary.md",
        "\n".join(header + lines + [""] + sessions_section(tables)) + "\n",
    )
    return verdict


def fold_width_section(tables) -> list[str]:
    lines = [
        "",
        "## Fold decisions by build width (warm runs)",
        "",
        "| policy | bytes/row | joins decided | folded |",
        "|---|---|---|---|",
    ]
    buckets: collections.Counter = collections.Counter()
    folded: collections.Counter = collections.Counter()
    for fold in tables["build_folds"]:
        width = fold["bytes_per_row"]
        bucket = (
            "unknown"
            if width is None
            else "<16" if width < 16 else "16-64" if width < 64 else ">=64"
        )
        buckets[(fold["fold_policy"], bucket)] += 1
        folded[(fold["fold_policy"], bucket)] += int(fold["fold"])
    for (policy, bucket), count in sorted(buckets.items()):
        lines.append(f"| {policy} | {bucket} | {count} | {folded[(policy, bucket)]} |")
    return lines


# ---------------------------------------------------------------------------------------------
# summarize
# ---------------------------------------------------------------------------------------------

TABLE_NAMES = (
    "sessions",
    "gpu_state",
    "runs",
    "queries",
    "build_folds",
    "operators",
    "bloom_transfers",
    "publications",
    "join_strategies",
)
EXIT_CODES = {"PASS": 0, "FAIL": 1, "INCOMPLETE": common.EXIT_INCOMPLETE}


def summarize(stage: str, args: argparse.Namespace) -> int:
    records = load_records(stage)
    tables = tabulate(records)
    out = common.results_dir(stage)
    for name in TABLE_NAMES:
        schemas.write_table(f"P.{name}", tables[name], out / f"{name}.parquet")
    summarize_stage = summarize_p1 if stage == "P1" else summarize_fold
    verdict = summarize_stage(stage, records, tables, out, args)
    excerpt = [
        f"{name}: {line}"
        for name, record in records.items()
        for line in record["excerpt"]
    ]
    excerpt += [
        f"{name}: status={record['status']} {record['error']}"
        for name, record in records.items()
        if record["error"]
    ]
    common.write_excerpt(out / "excerpts.txt", excerpt)
    common.log(f"{stage}: {verdict.state}; wrote {out}/summary.md")
    return EXIT_CODES[verdict.state]


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=("plan", "run", "summarize"))
    parser.add_argument("--stage", required=True, choices=sorted(STAGES))
    parser.add_argument("--sf", type=int)
    parser.add_argument("--gpus", help="comma list of GPU counts (P1: 4,1)")
    parser.add_argument("--experiments", help="P2/P3: comma list of E1,E2")
    parser.add_argument(
        "--concat", help="P2/P3: comma list of H divisors; 0 means 0 bytes"
    )
    parser.add_argument(
        "--policies", help="P2/P3: comma list of build_fold_policy values"
    )
    parser.add_argument("--filters", help="P2/P3: comma list of on,off")
    parser.add_argument("--queries", default="", help="comma list (default: all 22)")
    parser.add_argument(
        "--seed", type=int, help="P2/P3: seed of the shuffled cell order"
    )
    parser.add_argument("--warm-log-level")
    parser.add_argument("--timeout", type=float, help="hard limit per session, seconds")
    parser.add_argument(
        "--stall", type=float, help="kill a session idle this long, seconds"
    )
    parser.add_argument(
        "--assume-query-s", type=float, help="plan: seconds per query run"
    )
    parser.add_argument(
        "--assume-setup-s", type=float, help="plan: seconds of LOAD and pinning"
    )
    parser.add_argument(
        "--retry-failed",
        action="store_true",
        help="rerun sessions whose status is not ok",
    )
    args = parser.parse_args(argv)
    for name, value in STAGES[args.stage].items():
        if getattr(args, name, None) is None:
            setattr(args, name, value)
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    return {"plan": plan, "run": run, "summarize": summarize}[args.command](
        args.stage, args
    )


if __name__ == "__main__":
    sys.exit(main())
