#!/usr/bin/env python3
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
"""Validate S4 bundles and summarize independent process-pair observations."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import statistics
import sys

from run_s4_experiments import (
    METRICS,
    COMMON_SQL_OUTCOMES,
    LEGACY_SCHEMA_MESSAGE,
    file_hash,
    jsonl,
    load_json,
    require,
    validate_native,
)

# Two-sided 95% Student-t critical values; the declared design has at most ten clusters.
T975 = {
    1: 12.706204736,
    2: 4.302652730,
    3: 3.182446305,
    4: 2.776445105,
    5: 2.570581836,
    6: 2.446911851,
    7: 2.364624252,
    8: 2.306004135,
    9: 2.262157163,
}


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    lower = math.floor(index)
    upper = math.ceil(index)
    return (
        ordered[lower] * (upper - index) + ordered[upper] * (index - lower)
        if lower != upper
        else ordered[lower]
    )


def cluster_interval(cluster_logs: dict[str, list[float]]) -> dict:
    require(bool(cluster_logs) and all(cluster_logs.values()), "Empty process clusters")
    means = [statistics.mean(values) for values in cluster_logs.values()]
    center = statistics.mean(means)
    interval = None
    if 5 <= len(means) <= 10:
        margin = T975[len(means) - 1] * statistics.stdev(means) / math.sqrt(len(means))
        interval = [math.exp(center - margin), math.exp(center + margin)]
    ratios = [math.exp(value) for values in cluster_logs.values() for value in values]
    return {
        "independent_process_pairs": len(means),
        "measured_pairs": len(ratios),
        "geometric_mean_ratio": math.exp(center),
        "median_ratio": statistics.median(ratios),
        "p90_ratio": percentile(ratios, 0.9),
        "ratio_ci95": interval,
        "estimator": "equal-weight process-pair mean log ratios; Student-t interval across clusters",
        "cluster_mean_log_ratios": dict(zip(cluster_logs, means)),
        "paired_ratios": ratios,
    }


def validated_bundle(directory: Path) -> tuple[dict, list[dict]]:
    run = load_json(directory / "experiment.json")
    completion = load_json(directory / "completion.json")
    require(run.get("schema_version") == 2, LEGACY_SCHEMA_MESSAGE)
    require(completion.get("status") == "complete", f"Incomplete campaign: {directory}")
    require(
        completion["samples_sha256"] == file_hash(directory / "samples.jsonl"),
        "samples.jsonl hash mismatch",
    )
    require(
        completion["experiment_sha256"] == file_hash(directory / "experiment.json"),
        "Experiment manifest changed",
    )
    require(completion["processes"] == len(run["jobs"]), "Missing processes")
    records = jsonl(directory / "samples.jsonl")
    jobs = {j["process_id"]: j for j in run["jobs"]}
    workloads = {w["id"]: w for w in run["workloads"]}
    groups = defaultdict(list)
    identities = set()
    for record in records:
        require(record["sample_id"] not in identities, "Duplicate sample ID")
        identities.add(record["sample_id"])
        harness = record.get("harness", {})
        require(harness.get("process_id") in jobs, "Unplanned process")
        job = jobs[harness["process_id"]]
        for key, value in job.items():
            require(harness.get(key) == value, f"Sample changed planned {key}")
        require(
            harness.get("profile") == run["profile"]
            and harness.get("phase") == run["phase"],
            "Wrong sample profile/phase",
        )
        require(
            harness.get("held_out") == workloads[job["workload"]]["held_out"],
            "Wrong held-out classification",
        )
        groups[job["process_id"]].append(record)
    require(set(groups) == set(jobs), "Missing planned process samples")
    for process_id, group in groups.items():
        job = jobs[process_id]
        validate_native(
            group,
            run["builds"][job["revision_role"]],
            workloads[job["workload"]],
            job,
            run,
        )
    return run, records


def equivalent_publication(left: dict, right: dict) -> bool:
    fields = (
        "N",
        "K",
        "B",
        "C",
        "input_bitmap_bytes",
        "ready_replica_bytes",
        "published_bitmap_bytes",
        "logical_edge_bytes",
        "actual_transport",
    )
    if left["mode"] == "sql":
        if not left.get("publication_metrics_supported") or not right.get(
            "publication_metrics_supported"
        ):
            return False
        if not left["published_bitmap_bytes"] or not right["published_bitmap_bytes"]:
            return False
        sql_fields = (
            "accumulation_admitted_keys",
            "accumulation_contributing_gpus",
            "accumulation_direct_publications",
            "accumulation_host_publications",
            "publication_count",
            "accumulation_expected_contributions",
            "accumulation_completed_contributions",
        )
        if any(
            left.get(field) is None or right.get(field) is None for field in sql_fields
        ):
            return False
        fields += sql_fields
    return all(left.get(field) == right.get(field) for field in fields)


def summarize_pair_group(records: list[dict], metric: str) -> dict:
    matched = defaultdict(dict)
    for record in records:
        key = (record["process_cluster"], record["seed"], record["repetition"])
        arm = record["harness"]["arm"]
        require(arm not in matched[key], "Duplicate paired arm")
        matched[key][arm] = record
    logs = defaultdict(list)
    excluded = Counter()
    changed_work = []
    baseline_values, candidate_values = [], []
    work = []
    for (cluster, seed, repetition), arms in matched.items():
        require(
            set(arms) == {"baseline", "candidate"},
            "Unmatched baseline/candidate sample",
        )
        left, right = arms["baseline"], arms["candidate"]
        if left["status"] != "ok" or right["status"] != "ok":
            excluded[f"status:{left['status']}/{right['status']}"] += 1
            continue
        if left["mode"] == "sql" and not (
            left["gpu_intercepted"] and right["gpu_intercepted"]
        ):
            excluded["gpu_interception_not_proven"] += 1
            continue
        if left["mode"] == "sql":
            differences = {
                name: [left.get(name), right.get(name)]
                for name in COMMON_SQL_OUTCOMES
                if left.get(name) is None
                or right.get(name) is None
                or left[name] != right[name]
            }
            if differences:
                excluded["common_sql_outcomes_changed_or_missing"] += 1
                changed_work.append(
                    {
                        "baseline_sample": left["sample_id"],
                        "candidate_sample": right["sample_id"],
                        "outcomes": differences,
                    }
                )
                continue
        if metric == "host_elapsed_ns" and not equivalent_publication(left, right):
            excluded["publication_work_not_equivalent_or_not_published"] += 1
            continue
        a, b = left[metric], right[metric]
        if a is None or b is None or a <= 0 or b <= 0:
            excluded["timing_not_measured"] += 1
            continue
        logs[cluster].append(math.log(b / a))
        baseline_values.append(a)
        candidate_values.append(b)
        work.append(
            {
                "cluster": cluster,
                "seed": seed,
                "repetition": repetition,
                "baseline_sample": left["sample_id"],
                "candidate_sample": right["sample_id"],
                "baseline": {name: left.get(name) for name in METRICS},
                "candidate": {name: right.get(name) for name in METRICS},
            }
        )
    result = {
        "metric": metric,
        "excluded_pairs": dict(excluded),
        "changed_work": changed_work,
        "work_and_raw_timings": work,
        "complete_comparison": not excluded,
        "statistics": cluster_interval(logs) if logs else None,
    }
    for label, values in (
        ("baseline", baseline_values),
        ("candidate", candidate_values),
    ):
        result[label] = (
            {"median_ns": statistics.median(values), "p90_ns": percentile(values, 0.9)}
            if values
            else None
        )
    return result


def stage_a_evidence(directory: Path, run: dict, records: list[dict]) -> dict:
    transport = [r for r in records if r["mode"] == "transport" and not r["warmup"]]
    source = directory
    if not transport and run.get("stage_a_results"):
        source = Path(run["stage_a_results"])
        require(
            file_hash(source / "experiment.json")
            == run.get("stage_a_experiment_sha256"),
            "Referenced Stage A manifest changed",
        )
        prior, prior_records = validated_bundle(source)
        require(prior["devices"] == run["devices"], "Stage A GPU mapping differs")
        require(prior["builds"] == run["builds"], "Stage A used different builds")
        require(
            prior["config"]["sha256"] == run["config"]["sha256"],
            "Stage A used different configuration",
        )
        current_host = load_json(directory / "machine-before.json")
        prior_host = load_json(source / "machine-before.json")
        require(
            prior_host["hostname"] == current_host["hostname"],
            "Stage A ran on a different host",
        )
        transport = [
            r for r in prior_records if r["mode"] == "transport" and not r["warmup"]
        ]
    return {
        "directory": str(source) if transport else None,
        "successful_samples": sum(r["status"] == "ok" for r in transport),
        "unavailable_samples": sum(r["status"] != "ok" for r in transport),
        "complete": bool(transport) and all(r["status"] == "ok" for r in transport),
    }


def assess(
    result: dict, control: dict | None, phase: str, metric: str, eligible: bool
) -> str:
    stats = result["statistics"]
    if phase == "screen":
        return "exploratory_screen_no_selection"
    if (
        not eligible
        or not result["complete_comparison"]
        or not stats
        or stats["ratio_ci95"] is None
    ):
        return "insufficient_evidence"
    noise = (
        control["statistics"] if control and control["complete_comparison"] else None
    )
    if noise is None or noise["ratio_ci95"] is None:
        return "baseline_control_missing"
    if not (noise["ratio_ci95"][0] <= 1 <= noise["ratio_ci95"][1]):
        return "inconclusive_baseline_control_drift"
    low, high = stats["ratio_ci95"]
    if metric == "query_elapsed_ns":
        return (
            "regression_guard_pass"
            if high <= 1.03
            else "regression" if low > 1.03 else "regression_guard_inconclusive"
        )
    return "publication_timing_observation"


def analyze(directory: Path) -> dict:
    run, all_records = validated_bundle(directory)
    if run.get("extension_of"):
        reference = run["extension_of"]
        previous_dir = Path(reference["directory"])
        for name in ("experiment", "samples"):
            suffix = ".json" if name == "experiment" else ".jsonl"
            require(
                file_hash(previous_dir / (name + suffix))
                == reference[name + "_sha256"],
                "Original extension evidence changed",
            )
        prior, previous_records = validated_bundle(previous_dir)
        require(not prior.get("extension_of"), "Only one extension is allowed")
        require(
            not (
                {j["process_cluster"] for j in run["jobs"]}
                & {j["process_cluster"] for j in prior["jobs"]}
            ),
            "Extension cluster IDs overlap original clusters",
        )
        all_records = previous_records + all_records
    records = [r for r in all_records if not r["warmup"]]
    stage_a = stage_a_evidence(directory, run, records)
    groups = defaultdict(list)
    transport = defaultdict(list)
    builder = defaultdict(list)
    for record in records:
        harness = record["harness"]
        if record["mode"] == "transport":
            transport[harness["workload"]].append(record)
        elif record["mode"] == "builder":
            builder[harness["workload"]].append(record)
        else:
            groups[(harness["workload"], harness["comparison"])].append(record)
    summaries = {}
    for (workload, comparison), group in groups.items():
        metrics = (
            ["query_elapsed_ns", "host_elapsed_ns"]
            if group[0]["mode"] == "sql"
            else ["host_elapsed_ns"]
        )
        summaries[(workload, comparison)] = {
            metric: summarize_pair_group(group, metric) for metric in metrics
        }
    outcomes = []
    workloads = {w["id"]: w for w in run["workloads"]}
    eligible = (
        run["four_gpu_handoff"]
        and run["profile"] == "default256"
        and run["scale"] == "100"
        and run["dataset"]["availability"] == "available"
        and all(
            record["harness"]["warmups"] >= 3 and record["harness"]["repetitions"] >= 6
            for record in all_records
        )
    )
    for (workload, comparison), metrics in summaries.items():
        for metric, summary in metrics.items():
            control = summaries.get((workload, "baseline_baseline"), {}).get(metric)
            decision = (
                assess(summary, control, run["phase"], metric, eligible)
                if comparison == "baseline_final"
                else "noise_control"
            )
            outcomes.append(
                {
                    "workload": workloads[workload],
                    "comparison": comparison,
                    "assessment": decision,
                    **summary,
                }
            )
    diagnostics = []
    for workload, samples in transport.items():
        passed = [r for r in samples if r["status"] == "ok"]
        rates = [
            r["logical_edge_bytes"] / r["host_elapsed_ns"]
            for r in passed
            if r["logical_edge_bytes"] is not None
        ]
        diagnostics.append(
            {
                "workload": workloads[workload],
                "statuses": dict(Counter(r["status"] for r in samples)),
                "median_logical_GB_per_second": (
                    statistics.median(rates) if rates else None
                ),
                "physical_route": "unavailable; logical bytes divided by host interval do not identify the route",
            }
        )
    query_guards = [
        o
        for o in outcomes
        if o["comparison"] == "baseline_final" and o["metric"] == "query_elapsed_ns"
    ]
    all22 = (
        len(query_guards) == 22
        and all(o["assessment"] == "regression_guard_pass" for o in query_guards)
        and run["baseline_kind"] == "prerequisite"
    )
    builder_diagnostics = []
    for workload, samples in builder.items():
        values = [r["host_elapsed_ns"] for r in samples if r["status"] == "ok"]
        builder_diagnostics.append(
            {
                "workload": workloads[workload],
                "revision_role": "final",
                "assessment": "final_only_correctness_and_timing; no baseline comparison",
                "statuses": dict(Counter(r["status"] for r in samples)),
                "median_ns": statistics.median(values) if values else None,
                "p90_ns": percentile(values, 0.9) if values else None,
                "samples": samples,
            }
        )
    return {
        "directory": str(directory),
        "phase": run["phase"],
        "run_id": run["run_id"],
        "profile": run["profile"],
        "scale": run["scale"],
        "dataset": run["dataset"],
        "baseline_kind": run["baseline_kind"],
        "build_sources": {
            role: build.get("source") for role, build in run["builds"].items()
        },
        "stage_a": stage_a,
        "status_counts": dict(Counter(r["status"] for r in records)),
        "publication_skip_reasons": dict(
            Counter(
                str(r.get("publication_skip_reason"))
                for r in records
                if r["mode"] == "sql"
            )
        ),
        "outcomes": outcomes,
        "transport": diagnostics,
        "builder": builder_diagnostics,
        "all22_regression_guard_pass": all22,
        "decision": "revision validation observations; local two-GPU runs do not establish four-GPU acceptance",
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--results",
        type=Path,
        required=True,
        help="Campaign directory or parent containing campaigns",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="New JSON report path; default prints JSON to stdout",
    )
    args = parser.parse_args(argv)
    try:
        directories = (
            [args.results]
            if (args.results / "experiment.json").exists()
            else sorted({p.parent for p in args.results.rglob("experiment.json")})
        )
        require(bool(directories), "No experiment.json found")
        report = {
            "schema_version": 2,
            "campaigns": [analyze(d.resolve()) for d in directories],
        }
        text = json.dumps(report, indent=2, sort_keys=True, allow_nan=False) + "\n"
        if args.output:
            with args.output.open("x") as output:
                output.write(text)
            print(args.output)
        else:
            print(text, end="")
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(f"S4 analysis: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
