#!/usr/bin/env python3
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
"""Correctness runs for stages C2, C4 and C5 over the key-parity script.

`run` drives `tools/dynamic_filter_key_parity.py run` one query at a time, so a run can stop and
resume: a query whose `result.json` exists is skipped (with `--retry-failed`, one that failed is
set aside and rerun), and an unfinished query directory is set aside and rerun. Each query runs
once on the GPU and once with `gpu_execution = false` in one DuckDB CLI process that reads Parquet
from disk, at debug log level, with the parity script's default `enable_dynamic_zone_map_filter =
true`. The process sees no inherited `SIRIUS_*` or `CUDA_VISIBLE_DEVICES` variable. Raw output
goes to `S5B_WORK/<stage>/<label>/`.

`diff` runs the parity script's `diff` of two labels and keeps its report under `results/`.

`summarize` turns the raw output of the named labels into the `schemas.py` tables and
`summary.md` under `results/<stage>/`, and exits 0 (PASS), 1 (FAIL) or 3 (INCOMPLETE: a label or
query has not run). The Key Outcome bar is strict: a `weaker`, `missing` or `duplicate` row, or a
lost Zone Map, fails it even when a declared delta explains it; those rows are listed for the user.
"""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import shutil
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import common  # noqa: E402
import formats  # noqa: E402
import schemas  # noqa: E402
from schemas import merge_row  # noqa: E402


def label_dir(stage: str, label: str) -> pathlib.Path:
    return common.env_path("S5B_WORK") / stage / label


def parse_sets(pairs: list[str]) -> list[tuple[str, str]]:
    """`name=value` pairs; a value of the form `H/<n>` becomes the resolved H divided by n."""
    parsed = []
    for pair in pairs:
        name, sep, value = pair.partition("=")
        if not sep:
            sys.exit(f"error: --set expects name=value, got {pair!r}")
        parsed.append((name.strip(), value.strip()))
    return parsed


def resolve_sets(sets: list[tuple[str, str]], build: str, gpus: int) -> list[str]:
    resolved = []
    for name, value in sets:
        if value.startswith("H/"):
            h = int(common.resolved_settings(build, gpus)["hash_partition_bytes"])
            value = str(h // int(value[2:]))
        resolved.append(f"{name}={value}")
    return resolved


def query_failed(result_path: pathlib.Path) -> bool:
    result = json.loads(result_path.read_text())
    return (
        result.get("returncode") != 0
        or not result.get("result_match")
        or result.get("gpu_fallback") is not False
    )


def run(args: argparse.Namespace) -> int:
    out = label_dir(args.stage, args.label)
    out.mkdir(parents=True, exist_ok=True)
    sets = resolve_sets(parse_sets(args.set), args.build, args.gpus)
    meta = {
        "stage": args.stage,
        "label": args.label,
        "build": args.build,
        "gpus": args.gpus,
        "sf": args.sf,
        "settings": sets,
        "commit": common.git_head(common.worktree(args.build)),
        "config": str(common.config_file(args.gpus)),
    }
    meta_path = out / "meta.json"
    if meta_path.is_file():
        previous = json.loads(meta_path.read_text())
        if {k: previous.get(k) for k in meta} != meta:
            sys.exit(
                f"error: {out} holds a run with other parameters ({previous}); use a new label"
            )
    common.write_json_atomic(meta_path, meta)
    queries = args.queries.split(",") if args.queries else list(common.TPCH_QUERIES)
    source = common.queries_dir(args.sf)
    # The one-query directory the parity script reads sits outside the label directory, which
    # the parity diff reads as one subdirectory per query.
    staging = out.parent / f".staging-{args.label}"
    failures = 0
    for query in queries:
        target = out / query
        result_path = target / "result.json"
        if result_path.is_file() and not (
            args.retry_failed and query_failed(result_path)
        ):
            continue
        common.set_aside_partial(target)
        shutil.rmtree(staging, ignore_errors=True)
        staging.mkdir()
        shutil.copy(source / f"{query}.sql", staging / f"{query}.sql")
        command = [
            sys.executable,
            "-I",
            str(common.PARITY_SCRIPT),
            "run",
            "--extension", str(common.extension(args.build)),
            "--duckdb", str(common.duckdb_cli(args.build)),
            "--parquet", str(common.tpch_dir(args.sf)),
            "--queries", str(staging),
            "--out", str(out),
            "--timeout", str(args.timeout),
        ]  # fmt: skip
        for setting in sets:
            command += ["--set", setting]
        env = common.clean_env(SIRIUS_CONFIG_FILE=str(common.config_file(args.gpus)))
        common.wait_for_idle_gpus()
        common.log(f"{args.stage}/{args.label}: {query}")
        completed = subprocess.run(command, env=env, capture_output=True, text=True)
        (out / f".{query}.parity.txt").write_text(completed.stdout + completed.stderr)
        print(completed.stdout.strip(), flush=True)
        failures += int(completed.returncode != 0)
    shutil.rmtree(staging, ignore_errors=True)
    common.log(f"{args.stage}/{args.label}: done, {failures} query failure(s)")
    return 1 if failures else 0


def diff(args: argparse.Namespace) -> int:
    completed = subprocess.run(
        [
            sys.executable,
            "-I",
            str(common.PARITY_SCRIPT),
            "diff",
            str(label_dir(args.stage, args.base)),
            str(label_dir(args.stage, args.candidate)),
        ],
        capture_output=True,
        text=True,
    )
    report = (
        common.results_dir(args.stage) / f"diff_{args.base}_vs_{args.candidate}.txt"
    )
    common.write_text_atomic(
        report, completed.stdout + completed.stderr + f"exit={completed.returncode}\n"
    )
    print(
        completed.stdout.strip().splitlines()[-1]
        if completed.stdout.strip()
        else completed.stderr
    )
    common.log(f"diff exit {completed.returncode}; report {report}")
    return completed.returncode


STRICT_FAILURES = ("weaker", "missing", "duplicate")


def diff_rows(parity, stage: str, base_label: str, candidate_label: str) -> list[dict]:
    base = parity.load_run(label_dir(stage, base_label))
    candidate = parity.load_run(label_dir(stage, candidate_label))
    empty = parity.QueryRun({}, [], None)
    rows = []
    for query in sorted(base.keys() | candidate.keys()):
        for row in parity.compare_keys(
            query, base.get(query, empty), candidate.get(query, empty)
        ):
            sig, occ, condition = row.identity
            rows.append(
                {
                    "stage": stage,
                    "base_label": base_label,
                    "candidate_label": candidate_label,
                    "query": query,
                    "sig": sig,
                    "occ": occ,
                    "condition": condition,
                    "status": row.status,
                    "zone_map": row.zone_map,
                    "base_kind": row.base.kind if row.base else None,
                    "candidate_kind": row.candidate.kind if row.candidate else None,
                    "base_size": row.base.size if row.base else None,
                    "candidate_size": row.candidate.size if row.candidate else None,
                    "base_reason": row.base.reason if row.base else None,
                    "candidate_reason": row.candidate.reason if row.candidate else None,
                    "declared_delta": bool(
                        row.candidate and row.candidate.reason in parity.DECLARED_DELTAS
                    ),
                    "failed": row.failed,
                    "strict_failed": row.status in STRICT_FAILURES
                    or row.zone_map == "lost",
                }
            )
    return rows


def key_row(text: str) -> dict | None:
    """All fields of one Key Outcome line, typed as `schemas.TABLES['C.keys']` declares."""
    tokens = formats.parse_key_tokens(text)
    return None if tokens is None else formats.typed_key_row(tokens)


def resource_check(queries: list[dict], baseline: str, lines: list[str]) -> bool:
    """Fails when a label has more OOM retries than @p baseline on a query; reports downgrades.

    Downgrade bytes vary with timing, so their differences are reported for the user to judge.
    """
    reference = {row["query"]: row for row in queries if row["label"] == baseline}
    worse: dict[str, list[str]] = {"oom_retries": [], "downgrade_bytes": []}
    for row in queries:
        base = reference.get(row["query"])
        if row["label"] == baseline or base is None:
            continue
        for metric, entries in worse.items():
            if (row[metric] or 0) > (base[metric] or 0):
                entries.append(
                    f"{row['label']} {row['query']} {metric} {row[metric]} > {base[metric]}"
                )
    lines += [
        f"## Resources against `{baseline}`",
        "",
        f"- OOM retries above `{baseline}` (fails the bar): {len(worse['oom_retries'])}",
        *[f"  - {entry}" for entry in worse["oom_retries"][:50]],
        f"- downgrade bytes above `{baseline}` (reported, not a failure): {len(worse['downgrade_bytes'])}",
        *[f"  - {entry}" for entry in worse["downgrade_bytes"][:50]],
        "",
    ]
    return not worse["oom_retries"]


def summarize_label(
    stage: str, label: str, tables, lines: list[str], excerpts
) -> tuple[bool, bool]:
    """Tabulates one label; returns (complete, passed)."""
    directory = label_dir(stage, label)
    if not (directory / "meta.json").is_file():
        lines += [f"## {label}", "", "- not run", ""]
        return False, False
    meta = json.loads((directory / "meta.json").read_text())
    settings = common.settings_dict(meta["settings"])
    context = {
        "stage": stage,
        "label": label,
        "build": meta["build"],
        "commit": meta["commit"],
        "gpus": meta["gpus"],
        "sf": meta["sf"],
        "settings": " ".join(meta["settings"]),
    }
    events_context = {"stage": stage, "label": label}
    kinds: collections.Counter = collections.Counter()
    reasons: collections.Counter = collections.Counter()
    transport: collections.Counter = collections.Counter()
    excerpt: list[str] = []
    mismatches, fallbacks, missing, multiset, problems = [], [], [], [], []
    multi_partition = 0
    for query in common.TPCH_QUERIES:
        qdir = directory / query
        result_path = qdir / "result.json"
        if not result_path.is_file():
            missing.append(query)
            continue
        result = json.loads(result_path.read_text())
        events = common.parse_log_dir(qdir)
        keys = [k for k in (key_row(t) for t in events.key_lines) if k]
        for key in keys:
            kinds[key["kind"]] += 1
            reasons[key["reason"]] += 1
            tables["keys"].append(
                merge_row(
                    {
                        "stage": stage,
                        "label": label,
                        "build": meta["build"],
                        "gpus": meta["gpus"],
                        "sf": meta["sf"],
                        "query": query,
                    },
                    key,
                )
            )
        where = merge_row(events_context, {"query": query})
        for event in events.bloom_transfers:
            transport[event["transport"]] += 1
            tables["bloom_transfers"].append(merge_row(where, event))
        for event in events.publications:
            tables["publications"].append(merge_row(where, event))
        for event in events.join_strategies:
            tables["join_strategies"].append(merge_row(where, event))
        for event in events.build_folds:
            tables["build_folds"].append(merge_row(where, event))
        query_multi = sum(1 for e in events.join_strategies if e["partitions"] > 1)
        multi_partition += query_multi
        plan_fallback = bool(events.plan_fallbacks)
        gpu_fallback = bool(result.get("gpu_fallback")) or plan_fallback
        query_problems = common.settings_problems(settings, events)
        if not result.get("result_match"):
            mismatches.append(query)
        if gpu_fallback:
            fallbacks.append(query)
        if result.get("result_order") == formats.RESULT_MULTISET:
            multiset.append(query)
        if query_problems:
            problems.append(f"{query}: {'; '.join(query_problems[:3])}")
        excerpt += [f"{query}: {line}" for line in events.excerpt]
        tables["queries"].append(
            merge_row(
                context,
                {"query": query},
                {
                    "returncode": result.get("returncode"),
                    "result_match": result.get("result_match"),
                    "result_order": result.get("result_order"),
                    "gpu_fallback": bool(result.get("gpu_fallback")),
                    "plan_fallback": plan_fallback,
                    "gpu_error": result.get("gpu_error"),
                    "key_lines": len(keys),
                    "constructions_started": events.constructions_started,
                    "publications": len(events.publications),
                    "bloom_transfers_peer": sum(
                        e["transport"] == "peer" for e in events.bloom_transfers
                    ),
                    "bloom_transfers_host": sum(
                        e["transport"] == "host_staging" for e in events.bloom_transfers
                    ),
                    "multi_partition_joins": query_multi,
                    "oom_retries": events.oom_retries,
                    "reschedules": events.reschedules,
                    "downgrade_bytes": events.downgrade_bytes,
                    "reservation_shortfalls": events.reservation_shortfalls,
                    "reservation_failures": events.reservation_failures,
                    "replica_problems": len(events.replica_problems),
                    "severe_lines": len(events.severe),
                    "settings_problems": "; ".join(query_problems) or None,
                    "gpu_peak_bytes": events.max_gpu_peak_bytes,
                },
            )
        )
    excerpts[label] = excerpt
    passed = not mismatches and not fallbacks and not missing and not problems
    lines += [
        f"## {label}",
        "",
        f"- build `{meta['build']}` ({meta['commit'][:8]}), {meta['gpus']} GPU(s), SF{meta['sf']}, "
        f"settings: `{' '.join(meta['settings']) or 'defaults'}`",
        f"- CPU match: {'PASS' if passed else 'FAIL'}; mismatches {mismatches or 'none'}; GPU "
        f"fallback {fallbacks or 'none'}; not run {missing or 'none'}",
        f"- equal to the CPU only as multisets (row order differs; check ORDER BY ties): "
        f"{multiset or 'none'}",
        f"- logged settings that disagree with the label: {problems or 'none'}",
        f"- Key Outcome kinds: {dict(sorted(kinds.items()))}",
        f"- reasons: {dict(sorted(reasons.items()))}",
        f"- Bloom Filter transfer path (`bloom_finish`): "
        f"{dict(transport) or 'no multi-GPU Bloom Filter publication logged'}",
        f"- hash joins with more than one partition: {multi_partition}",
        "",
    ]
    return not missing, passed


TABLE_NAMES = (
    "queries",
    "keys",
    "bloom_transfers",
    "publications",
    "join_strategies",
    "build_folds",
)


def summarize(args: argparse.Namespace) -> int:
    parity = common.load_parity_module()
    stage = args.stage
    out = common.results_dir(stage)
    tables: dict[str, list[dict]] = collections.defaultdict(list)
    lines: list[str] = []
    verdicts, complete = [], True
    info_labels = set(filter(None, args.info_labels.split(",")))
    excerpts: dict[str, list[str]] = {}
    for label in args.labels.split(","):
        label_complete, passed = summarize_label(stage, label, tables, lines, excerpts)
        complete &= label_complete
        if label in info_labels:
            lines[-1:-1] = ["- (informational: left out of the verdict)"]
        else:
            verdicts.append(passed)
    for label, excerpt in excerpts.items():
        common.write_excerpt(out / "excerpts" / f"{label}.txt", excerpt)
    if args.resource_baseline:
        verdicts.append(
            resource_check(tables["queries"], args.resource_baseline, lines)
        )
    for name in TABLE_NAMES:
        schemas.write_table(f"C.{name}", tables[name], out / f"{name}.parquet")
    diff_tables = []
    for pair in args.diff or []:
        base_label, candidate_label = pair.split(":")
        if not all(
            (label_dir(stage, x) / "meta.json").is_file()
            for x in (base_label, candidate_label)
        ):
            complete = False
            continue
        rows = diff_rows(parity, stage, base_label, candidate_label)
        diff_tables += rows
        statuses = collections.Counter(r["status"] for r in rows)
        zone_lost = [r for r in rows if r["zone_map"] == "lost"]
        strict = [r for r in rows if r["strict_failed"]]
        declared = [r for r in strict if r["declared_delta"]]
        lines += [
            f"## Key Outcomes {base_label} -> {candidate_label}",
            "",
            f"- statuses: {dict(sorted(statuses.items()))}; Zone Maps lost: {len(zone_lost)}",
            f"- rows failing the strict bar: {len(strict)}, of which explained by a declared delta "
            f"(for the user to judge): {len(declared)} (see `diff_{base_label}_vs_{candidate_label}.txt`)",
            *[
                f"  - {r['query']} sig={r['sig']} occ={r['occ']} condition={r['condition']}: {r['status']}, "
                f"zone_map {r['zone_map']}, {r['base_kind']} -> {r['candidate_kind']} "
                f"({r['candidate_reason']})"
                for r in strict[:40]
            ],
            "",
        ]
        verdicts.append(not strict)
    if args.diff:
        schemas.write_table("C.key_diff", diff_tables, out / "key_diff.parquet")
    state = "INCOMPLETE" if not complete else "PASS" if all(verdicts) else "FAIL"
    header = [
        f"# {stage} summary",
        "",
        f"Generated {common.now_iso()}.",
        "",
        f"**Overall: {state}** (CPU match, no fallback and logged settings as SET for every label; "
        "no weaker, missing or duplicate Key Outcome and no lost Zone Map in any diff"
        + (
            f"; no OOM retries above `{args.resource_baseline}`"
            if args.resource_baseline
            else ""
        )
        + ")",
        "",
    ]
    common.write_text_atomic(out / "summary.md", "\n".join(header + lines) + "\n")
    common.log(f"{stage}: {state}; wrote {out}/summary.md")
    return {"PASS": 0, "FAIL": 1, "INCOMPLETE": common.EXIT_INCOMPLETE}[state]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    run_cmd = commands.add_parser("run")
    run_cmd.add_argument("--stage", required=True)
    run_cmd.add_argument("--label", required=True)
    run_cmd.add_argument("--build", required=True, choices=common.BUILDS)
    run_cmd.add_argument("--gpus", type=int, required=True)
    run_cmd.add_argument("--sf", type=int, required=True)
    run_cmd.add_argument(
        "--set",
        action="append",
        default=[],
        metavar="NAME=VALUE",
        help="SQL setting; VALUE may be H/<n> for hash_partition_bytes/<n>",
    )
    run_cmd.add_argument("--queries", default="", help="comma list (default: all 22)")
    run_cmd.add_argument("--timeout", type=float, default=1800.0)
    run_cmd.add_argument(
        "--retry-failed", action="store_true", help="rerun failed queries"
    )
    diff_cmd = commands.add_parser("diff")
    diff_cmd.add_argument("--stage", required=True)
    diff_cmd.add_argument("--base", required=True)
    diff_cmd.add_argument("--candidate", required=True)
    sum_cmd = commands.add_parser("summarize")
    sum_cmd.add_argument("--stage", required=True)
    sum_cmd.add_argument(
        "--labels", required=True, help="comma list of expected labels"
    )
    sum_cmd.add_argument(
        "--diff",
        action="append",
        metavar="BASE:CANDIDATE",
        help="also tabulate a key diff",
    )
    sum_cmd.add_argument(
        "--info-labels",
        default="",
        help="comma list of labels reported but left out of the verdict",
    )
    sum_cmd.add_argument(
        "--resource-baseline",
        default="",
        help="label whose OOM retries no other label may exceed (downgrade bytes are reported)",
    )
    args = parser.parse_args()
    return {"run": run, "diff": diff, "summarize": summarize}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
