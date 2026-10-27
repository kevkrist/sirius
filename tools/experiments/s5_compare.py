#!/usr/bin/env python3
# Copyright 2026, Sirius Contributors.
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repository root for the full text.
"""Bounded S5 CLI correctness, gate diagnostics, and fresh-process comparisons."""

from __future__ import annotations

import argparse
from collections import Counter
import csv
import ctypes
from decimal import Decimal
import hashlib
import importlib.util
import io
import json
import math
import os
from pathlib import Path
import random
import re
import shutil
import signal
import statistics
import subprocess
import sys
import time

sys.dont_write_bytecode = True
TABLES = (
    "customer",
    "lineitem",
    "nation",
    "orders",
    "part",
    "partsupp",
    "region",
    "supplier",
)
UNIQUE = {
    "customer": "c_custkey",
    "orders": "o_orderkey",
    "supplier": "s_suppkey",
    "part": "p_partkey",
    "nation": "n_nationkey",
    "region": "r_regionkey",
}
ARMS = ("0.9", "0.45", "2.0")
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
Q1_COLUMNS = (
    "l_returnflag",
    "l_linestatus",
    "sum_qty",
    "sum_base_price",
    "sum_disc_price",
    "sum_charge",
    "avg_qty",
    "avg_price",
    "avg_disc",
    "count_order",
)
GPU_PLAN = "Transparent execution: physical plan replaced with GPU operator"
GATE = re.compile(
    r"\[dynamic_filter_domain\] path=(ordinary|accumulated) condition=(\d+) rows=(\d+) domain=(\d+) provenance=(unknown|catalog_bound|pinned_parquet_exact) eligible=([01]) threshold=([0-9.eE+-]+) decision=(skip|keep)(?:\s|$)"
)


def require(ok, reason):
    if not ok:
        raise ValueError(reason)


def sha(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(4 * 1024**2), b""):
            result.update(block)
    return result.hexdigest()


def artifact(path):
    path = Path(path).resolve(strict=True)
    return {"path": str(path), "sha256": sha(path), "size": path.stat().st_size}


def save(path, value):
    with Path(path).open("x") as output:
        json.dump(value, output, indent=2, sort_keys=True, allow_nan=False)
        output.write("\n")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def load(path, typed=False):
    return json.loads(
        Path(path).read_text(),
        object_pairs_hook=unique_object,
        parse_float=Decimal if typed else float,
        parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)),
    )


def capture(command, **kwargs):
    p = subprocess.run(command, capture_output=True, text=True, timeout=60, **kwargs)
    require(p.returncode == 0, f"Command failed: {command}: {p.stderr}")
    return p.stdout


def literal(value):
    return "'" + str(value).replace("'", "''") + "'"


def dot_path(path):
    return json.dumps(str(path))


def rows(path, columns):
    text = Path(path).read_text().strip()
    value = load(path, typed=True) if text else []
    require(isinstance(value, list), f"Expected JSON rows: {path}")
    require(
        all(isinstance(row, dict) and list(row) == list(columns) for row in value),
        f"Result columns differ: {path}",
    )
    return [tuple(row[column] for column in columns) for row in value]


def same_rows(expected, actual, schema, query):
    names = tuple(row["column_name"] for row in schema)
    types = tuple(row["column_type"] for row in schema)
    if len(expected) != len(actual) or any(
        len(row) != len(names) for row in expected + actual
    ):
        return False
    if query != 1:
        return Counter(expected) == Counter(actual)
    if (
        names != Q1_COLUMNS
        or types[0:2] != ("VARCHAR", "VARCHAR")
        or types[8] != "DOUBLE"
    ):
        return False
    left = {row[:2]: row for row in expected}
    right = {row[:2]: row for row in actual}
    if (
        len(left) != len(expected)
        or len(right) != len(actual)
        or left.keys() != right.keys()
    ):
        return False
    for key, a in left.items():
        for i, (x, y) in enumerate(zip(a, right[key])):
            if x == y:
                continue
            if (
                i != 8
                or not isinstance(x, (int, Decimal))
                or not isinstance(y, (int, Decimal))
            ):
                return False
            x, y = Decimal(x), Decimal(y)
            if not x.is_finite() or not y.is_finite():
                return False
            if abs(x - y) > Decimal("1e-12") and abs(x - y) > Decimal("1e-12") * max(
                abs(x), abs(y)
            ):
                return False
    return True


def parse_times(text, count):
    expected = [f"S5_BEGIN_{i}" for i in range(count)]
    markers = re.findall(r"^S5_BEGIN_\d+$", text, re.M)
    require(markers == expected, "Missing, duplicate or reordered execution markers")
    timing = re.compile(
        r"^Run Time \(s\): real ([0-9.]+) user [0-9.]+ sys [0-9.]+$", re.M
    )
    require(len(timing.findall(text)) == count, "Wrong execution timing count")
    parts = re.split(r"^S5_BEGIN_\d+\r?$", text, flags=re.M)[1:]
    result = []
    for i, part in enumerate(parts):
        require(
            part.count(f"S5_END_{i}") == 1, "Missing/duplicate execution end marker"
        )
        matches = timing.findall(part)
        require(len(matches) == 1, "Missing/duplicate query timing")
        value = float(matches[0])
        require(math.isfinite(value) and value > 0, "Invalid query duration")
        result.append(value)
    require(
        len(re.findall(r"^S5_END_\d+$", text, re.M)) == count, "Wrong end marker count"
    )
    return result


def gate_records(text):
    result = []
    for line in text.splitlines():
        if "[dynamic_filter_domain]" not in line:
            continue
        match = GATE.search(line)
        require(match is not None, f"Unrecognized domain evidence: {line}")
        path, key, n, domain, source, eligible, threshold, decision = match.groups()
        result.append(
            dict(
                path=path,
                condition=int(key),
                rows=int(n),
                domain=int(domain),
                provenance=source,
                eligible=bool(int(eligible)),
                threshold=float(threshold),
                decision=decision,
            )
        )
    return result


def cuda_devices(count):
    driver = ctypes.CDLL("libcuda.so.1")

    def call(name, *args):
        require(
            getattr(driver, name)(*args) == 0, f"CUDA identity query failed: {name}"
        )

    call("cuInit", 0)
    visible = ctypes.c_int()
    call("cuDeviceGetCount", ctypes.byref(visible))
    require(visible.value >= count, "Fewer visible CUDA devices than requested")
    devices = []
    for ordinal in range(count):
        device, uuid, name, bus, memory = (
            ctypes.c_int(),
            (ctypes.c_ubyte * 16)(),
            ctypes.create_string_buffer(256),
            ctypes.create_string_buffer(32),
            ctypes.c_size_t(),
        )
        call("cuDeviceGet", ctypes.byref(device), ordinal)
        call("cuDeviceGetUuid", ctypes.byref(uuid), device)
        call("cuDeviceGetName", name, 256, device)
        call("cuDeviceGetPCIBusId", bus, 32, device)
        call("cuDeviceTotalMem_v2", ctypes.byref(memory), device)
        hexed = bytes(uuid).hex()
        guid = "GPU-" + "-".join(
            (hexed[:8], hexed[8:12], hexed[12:16], hexed[16:20], hexed[20:])
        )
        devices.append(
            dict(
                ordinal=ordinal,
                uuid=guid,
                name=name.value.decode(),
                pci_bus=bus.value.decode(),
                memory_bytes=memory.value,
            )
        )
    return {
        "visible_count": visible.value,
        "selected": devices,
        "CUDA_VISIBLE_DEVICES": os.environ.get("CUDA_VISIBLE_DEVICES"),
        "CUDA_DEVICE_ORDER": os.environ.get("CUDA_DEVICE_ORDER"),
    }


def gpu_apps(selected, allowed=()):
    output = capture(
        [
            "nvidia-smi",
            "--query-compute-apps=gpu_uuid,pid",
            "--format=csv,noheader,nounits",
        ]
    )
    apps = []
    for row in csv.reader(io.StringIO(output)):
        if not row:
            continue
        require(len(row) == 2, "Unrecognized GPU process observation")
        uuid, pid = row[0].strip(), int(row[1].strip())
        if uuid in selected:
            apps.append({"uuid": uuid, "pid": pid})
    require(all(app["pid"] in allowed for app in apps), f"Selected GPUs busy: {apps}")
    return apps


def execute(binary, script, directory, environment, timeout, selected):
    directory.mkdir(parents=True, exist_ok=False)
    (directory / "sirius-log").mkdir()
    (directory / "script.sql").write_text(script)
    command = [
        str(binary),
        "-unsigned",
        "-bail",
        "-no-agent",
        "-init",
        "/dev/null",
        ":memory:",
    ]
    save(
        directory / "command.json",
        {
            "argv": command,
            "environment": {
                k: v
                for k, v in environment.items()
                if k.startswith(("SIRIUS_", "CUDA_", "OMP_", "LIBCUDF_"))
                or k == "LD_LIBRARY_PATH"
            },
        },
    )
    started = time.monotonic()
    with (directory / "script.sql").open() as stdin, (directory / "stdout.log").open(
        "x"
    ) as stdout, (directory / "stderr.log").open("x") as stderr, (
        directory / "gpu-activity.jsonl"
    ).open(
        "x"
    ) as activity:

        def observe(allowed=()):
            apps = gpu_apps(selected, allowed)
            activity.write(json.dumps({"unix": time.time(), "apps": apps}) + "\n")
            activity.flush()

        observe()
        with subprocess.Popen(
            command,
            stdin=stdin,
            stdout=stdout,
            stderr=stderr,
            cwd=directory,
            env=environment,
            start_new_session=True,
        ) as process:
            try:
                while process.poll() is None:
                    require(time.monotonic() - started < timeout, "CLI process timeout")
                    observe((process.pid,))
                    try:
                        process.wait(timeout=1)
                    except subprocess.TimeoutExpired:
                        pass
                require(
                    process.returncode == 0,
                    f"CLI exited {process.returncode}; see {directory}",
                )
                observe()
            except BaseException:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait()
                raise
    return (directory / "stdout.log").read_text()


def preparation(files, columns, hints, patterns=None):
    statements = []
    for table in TABLES:
        paths = ",".join(literal(path) for path in files[table])
        statements.append(
            f"CREATE VIEW {table} AS SELECT * FROM read_parquet([{paths}]);"
        )
    for table, cols in sorted(columns.items()):
        hint = UNIQUE.get(table) if hints else None
        unique = f", unique_cols=>[{literal(hint)}]" if hint in cols else ""
        names = ",".join(literal(name) for name in cols)
        require(patterns is not None and table in patterns, "Missing exact pin glob")
        pattern = patterns[table]
        statements.append(
            f"CALL pin_table({literal(pattern)}, name={literal(table)}, tier='gpu', cols=[{names}], compression=false{unique});"
        )
    return statements


def make_script(
    query, prep, directory, extension=None, threshold="0.9", enabled=True, executions=1
):
    result = [".bail on", ".echo off", ".mode json", ".timer off"]
    if extension:
        result += [
            f"LOAD {literal(extension)};",
            "SET gpu_execution=false;",
            "SET enable_duckdb_fallback=false;",
            f"SET enable_dynamic_filter={str(enabled).lower()};",
            "SET enable_dynamic_filter_multi_partition=true;",
            "SET max_dynamic_filter_bloom_bytes_per_gpu=268435456;",
            f"SET dynamic_filter_domain_coverage_threshold={threshold};",
        ]
    result.extend(prep)
    result += [
        f".once {dot_path(directory / 'schema.json')}",
        "DESCRIBE " + query + ";",
    ]
    if extension:
        result += [
            f".once {dot_path(directory / 'settings.json')}",
            "SELECT current_setting('enable_duckdb_fallback') AS fallback, current_setting('enable_dynamic_filter') AS enabled, current_setting('enable_dynamic_filter_multi_partition') AS multipartition, current_setting('max_dynamic_filter_bloom_bytes_per_gpu')::BIGINT AS cap, current_setting('dynamic_filter_domain_coverage_threshold') AS coverage;",
            "SET gpu_execution=true;",
        ]
    for repetition in range(executions):
        result += [
            f".print S5_BEGIN_{repetition}",
            ".timer on 9",
            f".once {dot_path(directory / f'result-{repetition}.json')}",
            query + ";",
            ".timer off",
            f".print S5_END_{repetition}",
        ]
    result.append(".print S5_COMPLETE")
    return "\n".join(result) + "\n"


def plan(queries, arms, blocks, warmups, reps, seed, correctness_only=False):
    require(
        len(arms) == len(set(arms)) and set(arms) == set(ARMS),
        "Need each of the three threshold arms exactly once",
    )
    rng = random.Random(seed)
    jobs = []
    for query in rng.sample(queries, len(queries)):
        jobs.append(
            dict(
                phase="df_off",
                query=query,
                block=-1,
                arm="0.9",
                hints=True,
                warmups=0,
                reps=1,
            )
        )
        diagnostic_arms = [(a, True) for a in arms] + (
            [] if correctness_only else [("0.9", False)]
        )
        for arm, hints in diagnostic_arms:
            jobs.append(
                dict(
                    phase="correctness" if correctness_only else "diagnostic",
                    query=query,
                    block=-1,
                    arm=arm,
                    hints=hints,
                    warmups=0,
                    reps=1,
                )
            )
    if not correctness_only:
        for block in range(blocks):
            for query in rng.sample(queries, len(queries)):
                for arm in rng.sample(arms, len(arms)):
                    jobs.append(
                        dict(
                            phase="timing",
                            query=query,
                            block=block,
                            arm=arm,
                            hints=True,
                            warmups=warmups,
                            reps=reps,
                        )
                    )
    for number, job in enumerate(jobs):
        job["id"] = number
    return jobs


def interval(logs):
    center = statistics.mean(logs)
    bounds = None
    if 5 <= len(logs) <= 10:
        half = T975[len(logs) - 1] * statistics.stdev(logs) / math.sqrt(len(logs))
        bounds = [math.exp(center - half), math.exp(center + half)]
    return {
        "independent_blocks": len(logs),
        "ratio": math.exp(center),
        "ci95": bounds,
        "block_ratios": [math.exp(x) for x in logs],
        "margin": 0.03,
        "within_3pct": bounds[1] <= 1.03 if bounds else None,
        "verdict": (
            "screen_only"
            if bounds is None
            else (
                "nonregression_observed"
                if bounds[1] <= 1.03
                else "confirmed_regression" if bounds[0] > 1.03 else "inconclusive"
            )
        ),
    }


def confirmation_plan(queries, candidate, reference, seed):
    require(
        candidate in ARMS and reference in ARMS and candidate != reference,
        "Invalid confirmation comparison",
    )
    jobs = [j for j in plan(queries, ARMS, 1, 2, 6, seed) if j["phase"] != "timing"]
    rng = random.Random(seed)
    for block in range(5):
        for query in rng.sample(queries, len(queries)):
            for phase in rng.sample(["confirmation", "control"], 2):
                for side in rng.sample(["candidate", "reference"], 2):
                    arm = (
                        candidate
                        if phase == "confirmation" and side == "candidate"
                        else reference
                    )
                    jobs.append(
                        dict(
                            id=len(jobs),
                            phase=phase,
                            query=query,
                            block=block,
                            arm=arm,
                            side=side,
                            hints=True,
                            warmups=2,
                            reps=6,
                        )
                    )
    return jobs


def summarize(manifest, records):
    require(len(records) == len(manifest["jobs"]), "Incomplete experiment")
    by_id = {record["job"]["id"]: record for record in records}
    require(len(by_id) == len(records), "Duplicate process record")
    for job in manifest["jobs"]:
        record = by_id.get(job["id"])
        require(
            record is not None and record["job"] == job and record["valid"] is True,
            "Missing/invalid arm or request",
        )
        require(
            len(record["times_seconds"]) == job["warmups"] + job["reps"],
            "Missing warmup/repetition",
        )
    comparisons = []
    for query in manifest["queries"]:
        if manifest.get("confirmation"):
            for phase in ("confirmation", "control"):
                logs = []
                for block in range(5):
                    paired = [
                        r
                        for r in records
                        if r["job"]["phase"] == phase
                        and r["job"]["query"] == query
                        and r["job"]["block"] == block
                    ]
                    require(
                        len(paired) == 2,
                        "Duplicate/missing confirmation/control process",
                    )
                    pair = {r["job"]["side"]: r for r in paired}
                    require(
                        set(pair) == {"candidate", "reference"},
                        "Missing confirmation/control arm",
                    )
                    median = lambda side: statistics.median(
                        pair[side]["times_seconds"][2:]
                    )
                    logs.append(math.log(median("candidate") / median("reference")))
                estimate = interval(logs)
                comparisons.append(
                    dict(
                        query=query,
                        phase=phase,
                        **estimate,
                        baseline_drift_within_3pct=(
                            (
                                abs(estimate["ratio"] - 1) <= 0.03
                                and estimate["ci95"][0] <= 1 <= estimate["ci95"][1]
                            )
                            if phase == "control"
                            else None
                        ),
                        gain_at_least_5pct=(
                            (estimate["ratio"] <= 0.95 and estimate["ci95"][1] < 1)
                            if phase == "confirmation"
                            else None
                        ),
                    )
                )
            confirmation, control = comparisons[-2:]
            confirmation["baseline_drift_within_3pct"] = control[
                "baseline_drift_within_3pct"
            ]
            if not control["baseline_drift_within_3pct"]:
                confirmation["verdict"] = "inconclusive"
                confirmation["within_3pct"] = None
                confirmation["gain_at_least_5pct"] = None
                confirmation["attribution_reason"] = (
                    "same_query_baseline_control_failed"
                )
            continue
        for candidate, reference in (("0.9", "2.0"), ("0.45", "2.0"), ("0.45", "0.9")):
            logs = []
            for block in range(manifest["blocks"]):
                paired = [
                    r
                    for r in records
                    if r["job"]["phase"] == "timing"
                    and r["job"]["query"] == query
                    and r["job"]["block"] == block
                ]
                require(len(paired) == len(ARMS), "Duplicate/missing threshold process")
                pair = {r["job"]["arm"]: r for r in paired}
                require(set(pair) == set(ARMS), "Missing threshold arm")
                median = lambda arm: statistics.median(
                    pair[arm]["times_seconds"][pair[arm]["job"]["warmups"] :]
                )
                logs.append(math.log(median(candidate) / median(reference)))
            if logs:
                comparisons.append(
                    dict(
                        query=query,
                        candidate=candidate,
                        reference=reference,
                        **interval(logs),
                    )
                )
    return {
        "status": "complete",
        "gpu_count": len(manifest["hardware"]["selected"]),
        "queries": manifest["queries"],
        "comparisons": comparisons,
        "interpretation": "Ratios of process medians; independent randomized fresh-process blocks; no screen/diagnostic pooling or four-GPU acceptance claim.",
        "diagnostic_gate_counts": {
            str(r["job"]["id"]): dict(
                Counter(e["decision"] for e in r["domain_evidence"])
            )
            for r in records
            if r["job"]["phase"] == "diagnostic"
        },
    }


def validate_process(directory, job, expected, schema, gpu_count):
    count = job["warmups"] + job["reps"]
    stdout = (directory / "stdout.log").read_text()
    require(
        stdout.splitlines().count("S5_COMPLETE") == 1,
        "CLI did not complete exactly once",
    )
    times = parse_times(stdout, count)
    actual_schema = load(directory / "schema.json")
    require(actual_schema == schema, "CPU/GPU query schemas differ")
    columns = [row["column_name"] for row in schema]
    require(
        {p.name for p in directory.glob("result-*.json")}
        == {f"result-{i}.json" for i in range(count)},
        "Missing or extra result file",
    )
    for repetition in range(count):
        require(
            same_rows(
                expected,
                rows(directory / f"result-{repetition}.json", columns),
                schema,
                job["query"],
            ),
            f"CPU oracle mismatch, including warmups: execution {repetition}",
        )
    settings = load(directory / "settings.json", typed=True)
    wanted = dict(
        fallback=False,
        enabled=job["phase"] != "df_off",
        multipartition=True,
        cap=268435456,
        coverage=Decimal(job["arm"]),
    )
    require(settings == [wanted], f"Effective settings mismatch: {settings}")
    logs = "\n".join(
        p.read_text() for p in sorted((directory / "sirius-log").glob("*.log"))
    )
    require(
        logs.count(GPU_PLAN) == count, "Wrong successful GPU plan/interception count"
    )
    require(
        not re.search(
            r"Transparent execution fallback|runtime fallback|falling back|\[(?:error|critical)\]",
            logs,
            re.I,
        ),
        "Fallback or engine error contaminated process",
    )
    require(
        not re.search(
            r"(?:^|\n)(?:Error:|.*(?:Catalog|Binder|Parser|Invalid Input|IO) Error:)",
            (directory / "stderr.log").read_text(),
        ),
        "CLI error output",
    )
    topology = re.findall(r"topology summary.*?(\d+) GPU\(s\)", logs)
    require(topology == [str(gpu_count)], f"Actual topology mismatch: {topology}")
    if job["phase"] in ("timing", "confirmation", "control"):
        require(
            not re.search(r"\[(?:debug|trace)\]", logs),
            "DEBUG/TRACE output in timing process",
        )
    evidence = gate_records(logs) if job["phase"] == "diagnostic" else []
    for event in evidence:
        require(event["threshold"] == float(job["arm"]), "Gate log threshold mismatch")
        require(
            job["arm"] != "2.0" or event["decision"] == "keep",
            "Gate-off arm skipped a key",
        )
        require(
            event["decision"] != "skip" or (event["eligible"] and event["domain"] > 0),
            "Ineligible gate skip",
        )
    return {
        "job": job,
        "valid": True,
        "times_seconds": times,
        "domain_evidence": evidence,
        "gpu_executions": count,
        "oracle_checks": count,
        "files": {
            str(p.relative_to(directory)): artifact(p)
            for p in directory.rglob("*")
            if p.is_file()
        },
    }


def source_identity(root, output):
    evidence = {}
    for name, args in {
        "head": ["rev-parse", "HEAD"],
        "tree": ["rev-parse", "HEAD^{tree}"],
        "diff": ["diff", "--binary", "HEAD"],
        "submodules": ["submodule", "status", "--recursive"],
    }.items():
        text = capture(["git", "-C", str(root), *args])
        path = output / ("source-" + name + (".patch" if name == "diff" else ".txt"))
        path.write_text(text)
        evidence[name] = artifact(path)
    untracked = capture(
        [
            "git",
            "-C",
            str(root),
            "ls-files",
            "--others",
            "--exclude-standard",
            "-z",
            "--",
            "src",
            "include",
            "test",
            "cmake",
            "CMakeLists.txt",
            "Makefile",
            "sirius-duckdb/src",
        ]
    )
    evidence["untracked_source_files"] = {
        name: artifact(root / name) for name in untracked.split("\0") if name
    }
    evidence["root"] = str(root)
    return evidence


def environment(config, log_directory, debug=False, cpu=False):
    require(
        not any(
            os.environ.get(k)
            for k in ("LD_PRELOAD", "CUDA_INJECTION64_PATH", "NVTX_INJECTION64_PATH")
        ),
        "Profiling/preload injection must be unset",
    )
    env = {k: v for k, v in os.environ.items() if not k.startswith("SIRIUS_")}
    env.update(
        SIRIUS_CONFIG_FILE=str(config),
        SIRIUS_LOG_DIR=str(log_directory),
        SIRIUS_LOG_LEVEL="debug" if debug else "info",
        SIRIUS_LOG_BACKEND="spdlog",
        SIRIUS_ENABLE_TEST_OPTIONS="1",
    )
    if cpu:
        env["SIRIUS_DISABLE"] = "1"
    return env


def data_files(data, pin_module):
    import glob

    files, identities, patterns = {}, [], {}
    for table in TABLES:
        patterns[table] = pin_module.detect_pin_glob(str(data), table)
        files[table] = sorted(glob.glob(patterns[table]))
        require(bool(files[table]), f"No data for {table}")
        for name in files[table]:
            path = Path(name).resolve()
            stat = path.stat()
            identities.append(
                dict(
                    table=table,
                    path=str(path),
                    size=stat.st_size,
                    mtime_ns=stat.st_mtime_ns,
                )
            )
    return files, identities, patterns


def unchanged(manifest):
    for info in manifest["artifacts"].values():
        require(
            sha(info["path"]) == info["sha256"], f"Artifact changed: {info['path']}"
        )
    for info in manifest["data_files"]:
        stat = Path(info["path"]).stat()
        require(
            stat.st_size == info["size"] and stat.st_mtime_ns == info["mtime_ns"],
            f"Data changed: {info['path']}",
        )


def run(args):
    import yaml

    output, root = args.out.resolve(), args.root.resolve()
    output.mkdir(parents=True, exist_ok=False)
    try:
        queries = (
            list(range(1, 23))
            if args.queries == "all22"
            else [int(q) for q in args.queries.split(",")]
        )
        require(
            queries
            and len(queries) == len(set(queries))
            and all(1 <= q <= 22 for q in queries),
            "Invalid/duplicate queries",
        )
        pin_source = root / "test/tpch_performance/tpch_pin_columns.py"
        spec = importlib.util.spec_from_file_location("s5_pins", pin_source)
        pins = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(pins)
        files, identities, patterns = data_files(args.data.resolve(), pins)
        config = yaml.safe_load(args.config.read_text())
        topology = config["sirius"].setdefault("topology", {})
        topology.pop("gpu_ids", None)
        topology["num_gpus"] = args.gpus
        (output / "sirius.yaml").write_text(yaml.safe_dump(config, sort_keys=False))
        shutil.copy2(args.config, output / "input-config.yaml")
        shutil.copy2(Path(__file__), output / "s5_compare.py")
        shutil.copy2(pin_source, output / "tpch_pin_columns.py")
        artifacts = {
            "cli": artifact(args.cli or root / "sirius-duckdb/build/release/duckdb"),
            "extension": artifact(
                args.extension
                or root
                / "sirius-duckdb/build/release/extension/sirius/sirius.duckdb_extension"
            ),
            "library": artifact(
                args.library or root / "build/release/install/lib/libsirius.so"
            ),
            "harness": artifact(output / "s5_compare.py"),
            "pins": artifact(output / "tpch_pin_columns.py"),
            "config": artifact(output / "sirius.yaml"),
        }
        for query in queries:
            target = output / f"q{query}.sql"
            shutil.copy2(
                root / f"test/tpch_performance/tpch_queries/orig/q{query}.sql", target
            )
            artifacts[f"q{query}"] = artifact(target)
        metadata = None
        if args.data_metadata:
            metadata = load(args.data_metadata)
            require(
                set(metadata) == set(TABLES),
                "Dataset metadata must cover all eight tables",
            )
            for table in TABLES:
                require(
                    metadata[table]["files"] == len(files[table])
                    and metadata[table]["bytes"]
                    == sum(x["size"] for x in identities if x["table"] == table),
                    "Data metadata file/byte counts mismatch",
                )
            shutil.copy2(args.data_metadata, output / "data-preflight.json")
            artifacts["data_metadata"] = artifact(output / "data-preflight.json")
        env = environment(output / "sirius.yaml", output / "loader-logs")
        for info in artifacts.values():
            path = Path(info["path"])
            if path.is_relative_to(root):
                info["project_relative"] = str(path.relative_to(root))
            if path.is_relative_to(output):
                info["archive_relative"] = str(path.relative_to(output))
        linkage = {}
        for name in ("cli", "extension"):
            text = capture(["ldd", artifacts[name]["path"]], env=env)
            require("not found" not in text, f"Unresolved {name} dependency")
            resolved = re.findall(r"libsirius\.so[^\s]*\s+=>\s+(\S+)", text)
            require(
                name != "extension" or len(resolved) == 1,
                "Extension has no unique libsirius dependency",
            )
            require(
                all(
                    Path(p).resolve() == Path(artifacts["library"]["path"])
                    for p in resolved
                ),
                "Mismatched loaded libsirius",
            )
            (output / f"{name}-ldd.txt").write_text(text)
            linkage[name] = resolved
        hardware = cuda_devices(args.gpus)
        selected = {d["uuid"] for d in hardware["selected"]}
        gpu_apps(selected)
        (output / "nvidia-smi-before.txt").write_text(capture(["nvidia-smi"]))
        (output / "topology.txt").write_text(capture(["nvidia-smi", "topo", "-m"]))
        jobs = plan(
            queries,
            ARMS,
            args.blocks,
            args.warmups,
            args.reps,
            args.seed,
            args.correctness_only,
        )
        confirmation = None
        if args.confirm:
            require(
                not args.correctness_only
                and len(queries) == 2
                and args.control_query in queries,
                "Confirmation requires exactly the selected query plus the declared control, and no correctness-only flag",
            )
            candidate, reference = args.confirm.split(":")
            jobs = confirmation_plan(queries, candidate, reference, args.seed)
            confirmation = dict(
                candidate=candidate,
                reference=reference,
                control_query=args.control_query,
                rule="Fresh five-block confirmation and five reference/reference pairs; no screen pooling or automatic default change",
            )
        manifest = dict(
            schema="s5_compare_v1",
            source=source_identity(root, output),
            artifacts=artifacts,
            output_root=str(output),
            data_files=identities,
            data_identity_method="path_size_mtime_ns; optional supplied footer evidence",
            supplied_data_metadata=metadata,
            hardware=hardware,
            linkage=linkage,
            queries=queries,
            arms=list(ARMS),
            blocks=0 if args.correctness_only else args.blocks,
            jobs=jobs,
            confirmation=confirmation,
            seed=args.seed,
            command=sys.argv,
            working_directory=str(Path.cwd()),
            oracle="exact_except_q1_avg_disc_v1",
            margin=0.03,
            started_unix=time.time(),
        )
        save(output / "experiment.json", manifest)
        oracle = {}
        for query in queries:
            unchanged(manifest)
            directory = output / "oracle" / f"q{query}"
            sql = (output / f"q{query}.sql").read_text().strip().rstrip(";")
            prep = preparation(files, {}, False)
            script = make_script(sql, prep, directory)
            stdout = execute(
                artifacts["cli"]["path"],
                script,
                directory,
                environment(output / "sirius.yaml", directory / "sirius-log", cpu=True),
                args.timeout,
                selected,
            )
            require(
                stdout.splitlines().count("S5_COMPLETE") == 1, "Incomplete CPU oracle"
            )
            parse_times(stdout, 1)
            schema = load(directory / "schema.json")
            names = [row["column_name"] for row in schema]
            require(names and len(names) == len(set(names)), "Invalid CPU schema")
            oracle[query] = (schema, rows(directory / "result-0.json", names))
        save(
            output / "oracle-files.json",
            {
                str(p.relative_to(output)): artifact(p)
                for p in (output / "oracle").rglob("*")
                if p.is_file()
            },
        )
        records = []
        with (output / "records.jsonl").open("x") as stream:
            for job in jobs:
                unchanged(manifest)
                if job["phase"] in ("timing", "confirmation", "control"):
                    require(
                        any(
                            e["decision"] == "skip"
                            for r in records
                            for e in r["domain_evidence"]
                        ),
                        "No active domain gate in diagnostics; timing experiment unexercised",
                    )
                directory = (
                    output
                    / "runs"
                    / f"{job['id']:04d}-{job['phase']}-q{job['query']}-{job['arm']}"
                )
                sql = (output / f"q{job['query']}.sql").read_text().strip().rstrip(";")
                prep = preparation(
                    files, pins.QUERY_COLUMNS[job["query"]], job["hints"], patterns
                )
                script = make_script(
                    sql,
                    prep,
                    directory,
                    artifacts["extension"]["path"],
                    job["arm"],
                    job["phase"] != "df_off",
                    job["warmups"] + job["reps"],
                )
                print(f"{job['id'] + 1}/{len(jobs)} {job}", flush=True)
                execute(
                    artifacts["cli"]["path"],
                    script,
                    directory,
                    environment(
                        output / "sirius.yaml",
                        directory / "sirius-log",
                        job["phase"] == "diagnostic",
                    ),
                    args.timeout,
                    selected,
                )
                schema, expected = oracle[job["query"]]
                record = validate_process(directory, job, expected, schema, args.gpus)
                record["directory"] = str(directory)
                stream.write(json.dumps(record, sort_keys=True, allow_nan=False) + "\n")
                stream.flush()
                records.append(record)
        unchanged(manifest)
        save(output / "summary.json", summarize(manifest, records))
        (output / "nvidia-smi-after.txt").write_text(capture(["nvidia-smi"]))
        save(
            output / "completion.json",
            {
                "status": "complete",
                "experiment_sha256": sha(output / "experiment.json"),
                "records_sha256": sha(output / "records.jsonl"),
                "oracle_files_sha256": sha(output / "oracle-files.json"),
                "finished_unix": time.time(),
            },
        )
    except BaseException as error:
        save(
            output / "failure.json", {"error": repr(error), "failed_unix": time.time()}
        )
        raise


def analyze(output):
    completion = load(output / "completion.json")
    require(
        completion["status"] == "complete",
        "Interrupted experiment cannot support acceptance",
    )
    for name in ("experiment", "records"):
        path = output / (name + (".json" if name == "experiment" else ".jsonl"))
        require(sha(path) == completion[name + "_sha256"], f"Changed {name} evidence")
    manifest = load(output / "experiment.json")
    require(
        sha(output / "oracle-files.json") == completion["oracle_files_sha256"],
        "Changed oracle inventory",
    )
    for relative, info in load(output / "oracle-files.json").items():
        require(
            sha(output / relative) == info["sha256"], f"Changed CPU oracle: {relative}"
        )
    for info in manifest["artifacts"].values():
        if "archive_relative" in info:
            require(
                sha(output / info["archive_relative"]) == info["sha256"],
                "Changed frozen input/harness",
            )

    def relocated(path):
        original = Path(path)
        return output / original.relative_to(manifest["output_root"])

    records = [
        json.loads(line, object_pairs_hook=unique_object)
        for line in (output / "records.jsonl").read_text().splitlines()
    ]
    for record in records:
        for info in record["files"].values():
            require(
                sha(relocated(info["path"])) == info["sha256"],
                f"Changed raw evidence: {info['path']}",
            )
        query = record["job"]["query"]
        directory = output / "oracle" / f"q{query}"
        schema = load(directory / "schema.json")
        expected = rows(
            directory / "result-0.json", [row["column_name"] for row in schema]
        )
        validated = validate_process(
            relocated(record["directory"]),
            record["job"],
            expected,
            schema,
            len(manifest["hardware"]["selected"]),
        )
        require(
            validated["times_seconds"] == record["times_seconds"]
            and validated["domain_evidence"] == record["domain_evidence"],
            "Serialized record differs from raw evidence",
        )
    return summarize(manifest, records)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    measure = sub.add_parser("run")
    measure.add_argument("--root", type=Path, required=True)
    measure.add_argument("--data", type=Path, required=True)
    measure.add_argument("--data-metadata", type=Path)
    measure.add_argument("--config", type=Path, required=True)
    measure.add_argument(
        "--gpus",
        type=int,
        choices=(2, 4),
        required=True,
        help="Number of first visible CUDA ordinals; CUDA_VISIBLE_DEVICES is preserved",
    )
    for name in ("cli", "extension", "library"):
        measure.add_argument("--" + name, type=Path)
    measure.add_argument(
        "--queries", default="4,7,10,21", help="Comma-separated IDs or all22"
    )
    measure.add_argument("--blocks", type=int)
    measure.add_argument("--warmups", type=int)
    measure.add_argument("--reps", type=int)
    measure.add_argument("--seed", type=int, default=18235)
    measure.add_argument("--timeout", type=float, default=7200)
    measure.add_argument("--correctness-only", action="store_true")
    measure.add_argument(
        "--confirm",
        choices=("0.45:0.9", "0.45:2.0", "0.9:2.0"),
        help="Explicit selected candidate:reference confirmation, five fresh pairs plus five B/B pairs",
    )
    measure.add_argument("--control-query", type=int, default=4)
    for command in (measure, sub.add_parser("analyze")):
        command.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "analyze":
        print(json.dumps(analyze(args.out.resolve()), indent=2, allow_nan=False))
    else:
        defaults = (5, 2, 6) if args.confirm else (1, 1, 1)
        for name, value in zip(("blocks", "warmups", "reps"), defaults):
            if getattr(args, name) is None:
                setattr(args, name, value)
        require(
            not args.confirm or (args.blocks, args.warmups, args.reps) == (5, 2, 6),
            "Confirmation uses exactly 5 blocks, 2 warmups, 6 measured repetitions",
        )
        require(
            1 <= args.blocks <= 10
            and args.warmups >= 0
            and args.reps >= 1
            and args.timeout > 0,
            "Invalid run bounds",
        )
        run(args)


if __name__ == "__main__":
    main()
