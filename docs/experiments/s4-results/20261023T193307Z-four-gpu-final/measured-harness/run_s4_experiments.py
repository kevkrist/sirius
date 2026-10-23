#!/usr/bin/env python3
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
"""Freeze build identities and run bounded, paired S4 experiments."""

from __future__ import annotations

import argparse
import glob
import hashlib
import importlib.util
import json
import math
import os
import platform
import random
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASELINE_TREE = "9f59d68e4cdc7ca47999c3579407f6f5168ea652"
REVISION_ROLES = ("baseline", "final")
BASELINE_REVISIONS = {
    "prerequisite": "78a3e34c66143152ff075729f4c018806c11e67d",
    "s4_control": "9b2e8201787f381b62715c01405f659b925c49a7",
}
BASELINE_OVERLAY = (
    "test/cpp/operator/s4_experiment.cpp",
    "test/cpp/operator/s4_experiment_common.hpp",
    "test/cpp/operator/s4_experiment_sql.cpp",
    "test/cpp/operator/s4_experiment_sql_oracle.hpp",
    "cmake/sirius-s4-baseline-fixture.cmake",
)
SQL_ORACLE_POLICY = {
    "oracle_policy": "exact_except_q1_avg_disc_v1",
    "oracle_rel_tol": 1e-12,
    "oracle_abs_tol": 1e-12,
}
LEGACY_SCHEMA_MESSAGE = "Schema v2 is required; analyze archived v1 campaigns with the runner and analyzer from commit 9b2e8201787f381b62715c01405f659b925c49a7"
MIB = 1024**2
CAPS = {"default256": 256 * MIB, "raised2g": 2048 * MIB}
METRICS = (
    "host_elapsed_ns",
    "query_elapsed_ns",
    "N",
    "K",
    "B",
    "C",
    "input_bitmap_bytes",
    "ready_replica_bytes",
    "published_bitmap_bytes",
    "logical_edge_bytes",
    "d2h_bytes",
    "h2d_bytes",
    "scratch_requested_bytes",
    "scratch_aligned_bytes",
    "host_staging_physical_bytes",
)
COMMON_SQL_OUTCOMES = (
    "publication_count",
    "accumulations_started",
    "accumulation_expected_contributions",
    "accumulation_completed_contributions",
    "accumulations_skipped_inventory",
    "accumulations_skipped_admission",
    "accumulations_skipped_error",
    "accumulations_skipped_transient",
    "accumulations_incomplete",
    "accumulations_abandoned",
    "accumulation_storage_leaks",
    "keys_skipped_bloom_size_gate",
    "keys_skipped_domain_gate",
    "keys_skipped_type_mismatch",
    "keys_build_exceeded_domain",
    "membership_filters_built",
    "filters_pushed",
    "result_rows",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def canonical(value: object) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), allow_nan=False
    ).encode()


def digest(value: object) -> str:
    return hashlib.sha256(canonical(value)).hexdigest()


def file_hash(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(4 * MIB), b""):
            result.update(block)
    return result.hexdigest()


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x") as output:
        output.write(
            json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + "\n"
        )
    path.chmod(0o444)


def decode_json(text: str) -> dict:
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f"Duplicate JSON key: {key}")
            result[key] = value
        return result

    return json.loads(
        text,
        object_pairs_hook=unique,
        parse_constant=lambda s: (_ for _ in ()).throw(ValueError(s)),
    )


def load_json(path: Path) -> dict:
    return decode_json(path.read_text())


def jsonl(path: Path) -> list[dict]:
    records = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        require(bool(line.strip()), f"Blank JSONL line {path}:{number}")
        record = decode_json(line)
        require(isinstance(record, dict), f"Expected object at {path}:{number}")
        records.append(record)
    require(bool(records), f"Empty JSONL file: {path}")
    return records


def capture(command: list[str], *, env=None, timeout=30, cwd=None) -> dict:
    try:
        result = subprocess.run(
            command,
            env=env,
            cwd=cwd,
            text=True,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
        return {
            "command": command,
            "returncode": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr,
            "availability": "available" if result.returncode == 0 else "unavailable",
        }
    except (OSError, subprocess.TimeoutExpired) as error:
        return {
            "command": command,
            "returncode": None,
            "availability": "unavailable",
            "stdout": "",
            "stderr": str(error),
        }


def checked(command: list[str], **kwargs) -> str:
    result = capture(command, **kwargs)
    require(result["returncode"] == 0, f"Command failed: {command}: {result['stderr']}")
    return result["stdout"].strip()


def artifact(path: Path) -> dict:
    path = path.resolve(strict=True)
    require(path.is_file(), f"Not a file: {path}")
    return {"path": str(path), "sha256": file_hash(path), "size": path.stat().st_size}


def build_environment(build: dict) -> dict:
    env = dict(os.environ)
    require(not env.get("LD_PRELOAD"), "LD_PRELOAD must be unset for S4 measurements")
    env["LD_LIBRARY_PATH"] = ":".join(build["library_search_paths"])
    return env


def fixture_environment(build: dict, config: Path) -> dict:
    env = build_environment(build)
    env["SIRIUS_CONFIG_FILE"] = str(config)
    env["SIRIUS_ENABLE_TEST_OPTIONS"] = "1"
    env.pop("SIRIUS_DISABLE", None)
    return env


def loader_identity(
    path: Path, library: Path, env: dict, *, needs_sirius: bool
) -> dict:
    result = capture(["ldd", str(path)], env=env)
    require(result["returncode"] == 0, f"ldd failed for {path}: {result['stderr']}")
    require(
        "not found" not in result["stdout"],
        f"Unresolved library in {path}: {result['stdout']}",
    )
    matches = re.findall(r"libsirius[^\s]*\s+=>\s+(\S+)", result["stdout"])
    if needs_sirius:
        require(
            len(matches) == 1, f"Expected one shared libsirius dependency in {path}"
        )
    for resolved in matches:
        require(
            Path(resolved).resolve() == library.resolve(),
            f"Wrong libsirius resolution for {path}: {resolved}",
        )
    result["resolved_sirius"] = [str(Path(p).resolve()) for p in matches]
    result["stdout"] = re.sub(r"\s+\(0x[0-9a-fA-F]+\)", "", result["stdout"])
    return result


def oracle_policy(record: dict) -> dict:
    policy = {name: record[name] for name in SQL_ORACLE_POLICY if name in record}
    require(
        not policy or policy == SQL_ORACLE_POLICY,
        "Unsupported SQL oracle policy or tolerance",
    )
    return policy


def identify(build: dict, directory: Path) -> dict:
    output = directory / "identify.jsonl"
    fixture = build["artifacts"]["fixture"]["path"]
    checked(
        [fixture, "identify", "--output", str(output)], env=build_environment(build)
    )
    records = jsonl(output)
    require(len(records) == 1, "identify must emit exactly one record")
    record = records[0]
    require(
        record.get("schema_version") == 2 and record.get("mode") == "identify",
        "Unsupported native identify protocol",
    )
    require(
        record.get("revision_role") == build["revision_role"],
        "Fixture revision role mismatch",
    )
    require(
        record.get("identity_source") == "fixture_build_metadata",
        "Missing fixture identity provenance",
    )
    modes = (
        ["identify", "sql"]
        if build["revision_role"] == "baseline"
        else ["identify", "transport", "builder", "sql"]
    )
    require(
        set(record.get("supported_modes", [])) == set(modes), "Unexpected fixture modes"
    )
    require(
        type(record.get("publication_metrics_supported")) is bool,
        "Missing metric availability",
    )
    oracle_policy(record)
    require(
        record.get("linkage") == "static_sirius_core",
        "Unexpected native fixture linkage",
    )
    for name in ("bloom_block_bytes", "cuda_allocation_alignment"):
        require(
            type(record.get(name)) is int and record[name] > 0,
            f"Missing identify {name}",
        )
    return record


def baseline_cmake(original: str) -> str:
    old = "  include(cmake/sirius-s4-experiments.cmake)\n"
    new = "  include(cmake/sirius-s4-baseline-fixture.cmake)\n"
    anchor = "  include(cmake/sirius-tests.cmake)\n"
    require(original.count(anchor) == 1, "Missing baseline test registration anchor")
    return (
        original.replace(old, new)
        if old in original
        else original.replace(anchor, anchor + new)
    )


def source_identity(root: Path, role: str, baseline_kind: str = "prerequisite") -> dict:
    head = checked(["git", "rev-parse", "HEAD"], cwd=root)
    tree = checked(["git", "rev-parse", "HEAD^{tree}"], cwd=root)
    overlay = {}
    if role == "baseline":
        require(
            head == BASELINE_REVISIONS[baseline_kind],
            "Baseline must use the exact frozen revision",
        )
        if baseline_kind == "prerequisite":
            require(tree == BASELINE_TREE, "Prerequisite tree mismatch")
        changed = set(
            checked(["git", "diff", "--name-only", "HEAD"], cwd=root).splitlines()
        )
        require(
            changed <= {*BASELINE_OVERLAY, "CMakeLists.txt"},
            f"Baseline contains non-overlay changes: {sorted(changed - {*BASELINE_OVERLAY, 'CMakeLists.txt'})}",
        )
        original = checked(["git", "show", "HEAD:CMakeLists.txt"], cwd=root) + "\n"
        require(
            (root / "CMakeLists.txt").read_text() == baseline_cmake(original),
            "Baseline CMake changes exceed the test-only include",
        )
        overlay = {name: file_hash(root / name) for name in BASELINE_OVERLAY}
    paths = [
        "CMakeLists.txt",
        "cmake",
        "src",
        "include",
        "test/cpp",
        "sirius-duckdb/CMakeLists.txt",
    ]
    diff = checked(["git", "diff", "--binary", "HEAD", "--", *paths], cwd=root)
    new_paths = checked(
        ["git", "ls-files", "--others", "--exclude-standard", "--", *paths], cwd=root
    )
    untracked = {
        p: file_hash(root / p) for p in new_paths.splitlines() if (root / p).is_file()
    }
    if role == "baseline":
        require(
            set(untracked) <= set(BASELINE_OVERLAY),
            "Untracked baseline production/build source",
        )
    dependencies = []
    for line in checked(
        ["git", "submodule", "status", "--recursive"], cwd=root
    ).splitlines():
        match = re.match(r"\s*([+U-]?)([0-9a-f]{40})\s+(\S+)", line)
        require(
            match is not None and match[1] not in ("+", "U"),
            "Submodule revision differs from its source pin",
        )
        dependencies.append(
            {"path": match[3], "revision": match[2], "initialized": match[1] != "-"}
        )
    return {
        "head": head,
        "tree": tree,
        "baseline_kind": baseline_kind if role == "baseline" else None,
        "baseline_production_unchanged": role == "baseline",
        "fixture_overlay_sha256": overlay,
        "fixture_sources_sha256": {
            name: file_hash(root / name) for name in BASELINE_OVERLAY[:-1]
        },
        "diff": diff,
        "untracked_sha256": untracked,
        "submodules": dependencies,
    }


def cmake_settings(path: Path) -> dict:
    result = {}
    for line in path.read_text().splitlines():
        match = re.match(r"([^:#/][^:]*):([^=]+)=(.*)", line)
        if match and match[2] not in ("INTERNAL", "STATIC"):
            result[match[1]] = match[3]
    return result


def comparable_configuration(
    config: dict, root: Path, build_dir: Path, install: Path, baseline_kind: str | None
) -> dict:
    legacy = "SIRIUS_S4_PUBLICATION_SCHEDULE"
    if legacy in config:
        require(
            baseline_kind == "s4_control" and config[legacy] == "current_direct",
            "A production schedule selector is only valid in the frozen S4 control",
        )
    locations = sorted(
        (
            (str(root.resolve()), "<SOURCE>"),
            (str(build_dir.resolve()), "<BUILD>"),
            (str(install.resolve()), "<INSTALL>"),
        ),
        key=lambda pair: -len(pair[0]),
    )
    result = {}
    for name, value in config.items():
        if name == legacy:
            continue
        for path, token in locations:
            value = re.sub(re.escape(path) + r"(?=/|;|$)", lambda _: token, value)
        result[name] = value
    return result


def manifest_command(args) -> None:
    output = args.output.resolve()
    require(not output.exists(), f"Refusing to replace {output}")
    artifacts = {
        name: artifact(getattr(args, name))
        for name in ("fixture", "library", "extension", "cli")
    }
    cache = (args.cmake_cache or output.parent / "CMakeCache.txt").resolve(strict=True)
    config = cmake_settings(cache)
    root = args.source_root.resolve(strict=True)
    source = source_identity(root, args.revision_role, args.baseline_kind)
    comparable_config = comparable_configuration(
        config,
        root,
        args.build_dir,
        args.library.resolve().parent.parent,
        source["baseline_kind"],
    )
    search = [str(Path(artifacts["library"]["path"]).parent)]
    if os.environ.get("CONDA_PREFIX"):
        search.append(str(Path(os.environ["CONDA_PREFIX"]) / "lib"))
    search += [
        str(Path(p).resolve())
        for p in os.environ.get("LD_LIBRARY_PATH", "").split(":")
        if p
    ]
    build = {
        "schema_version": 2,
        "revision_role": args.revision_role,
        "baseline_kind": source["baseline_kind"],
        "linkage": "static_sirius_core",
        "artifacts": artifacts,
        "library_search_paths": list(dict.fromkeys(search)),
        "cmake_cache": artifact(cache),
        "build_command": (
            artifact(output.parent / "build-command.json")
            if (output.parent / "build-command.json").is_file()
            else None
        ),
        "configuration": config,
        "comparable_configuration": comparable_config,
        "configuration_sha256": digest(comparable_config),
        "source": source,
    }
    build["source_sha256"] = digest(build["source"])
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s4-identify-", dir=output.parent) as tmp:
        build["identify"] = identify(build, Path(tmp))
    env = build_environment(build)
    library = Path(artifacts["library"]["path"])
    build["loader"] = {
        name: loader_identity(
            Path(artifacts[name]["path"]),
            library,
            env,
            needs_sirius=name == "extension",
        )
        for name in ("fixture", "extension", "cli")
    }
    build["cli_version"] = checked([artifacts["cli"]["path"], "--version"], env=env)
    build["build_id"] = digest(build)
    write_json(output, build)
    print(output)


def verify_build(build: dict, *, runtime=True) -> None:
    require(build.get("schema_version") == 2, LEGACY_SCHEMA_MESSAGE)
    require(
        build.get("build_id")
        == digest({k: v for k, v in build.items() if k != "build_id"}),
        "Build manifest identity was modified",
    )
    for item in [*build["artifacts"].values(), build["cmake_cache"]]:
        require(
            artifact(Path(item["path"])) == item, f"Artifact changed: {item['path']}"
        )
    if build.get("build_command"):
        require(
            artifact(Path(build["build_command"]["path"])) == build["build_command"],
            "Build command provenance changed",
        )
    if runtime:
        with tempfile.TemporaryDirectory(prefix="s4-verify-") as tmp:
            current = identify(build, Path(tmp))
        require(current == build["identify"], "Fixture identify record changed")
        for name in ("fixture", "extension", "cli"):
            actual = loader_identity(
                Path(build["artifacts"][name]["path"]),
                Path(build["artifacts"]["library"]["path"]),
                build_environment(build),
                needs_sirius=name == "extension",
            )
            require(
                actual["stdout"] == build["loader"][name]["stdout"],
                f"Runtime library resolution changed for {name}",
            )


def validate_build_pair(builds: dict) -> None:
    baseline, candidate = (builds[name] for name in REVISION_ROLES)
    for role, build in builds.items():
        require(build["revision_role"] == role, "Mislabeled revision role")
    require(
        baseline["source"]["baseline_production_unchanged"],
        "Baseline production was modified",
    )
    require(
        baseline["source"]["head"] == BASELINE_REVISIONS[baseline["baseline_kind"]],
        "Wrong baseline revision",
    )
    require(
        baseline["source"]["submodules"] == candidate["source"]["submodules"],
        "Dependency revisions differ",
    )
    require(
        baseline["source"]["fixture_sources_sha256"]
        == candidate["source"]["fixture_sources_sha256"],
        "SQL fixture overlay differs between revisions",
    )
    require(
        baseline["identify"]["duckdb_version"]
        == candidate["identify"]["duckdb_version"],
        "DuckDB versions differ",
    )
    require(
        oracle_policy(baseline["identify"]) == oracle_policy(candidate["identify"]),
        "SQL oracle policies differ between revisions",
    )
    for field in ("configuration_sha256",):
        require(baseline[field] == candidate[field], f"Paired builds differ in {field}")
    for field in ("bloom_block_bytes", "cuda_allocation_alignment"):
        require(
            baseline["identify"][field] == candidate["identify"][field],
            "Geometry differs across builds",
        )
    for name in ("fixture", "library"):
        require(
            baseline["artifacts"][name]["sha256"]
            != candidate["artifacts"][name]["sha256"],
            f"Identical {name} hashes for different revisions",
        )


def pair_manifests(args) -> None:
    builds = {
        REVISION_ROLES[0]: load_json(args.baseline),
        REVISION_ROLES[1]: load_json(args.final),
    }
    for build in builds.values():
        verify_build(build)
    validate_build_pair(builds)
    write_json(args.output, {"schema_version": 2, "builds": builds})
    print(args.output)


def load_builds(path: Path) -> dict:
    value = load_json(path)
    if "builds" in value:
        builds = value["builds"]
    else:
        builds = {name: load_json(Path(value[name])) for name in REVISION_ROLES}
    require(
        set(builds) == set(REVISION_ROLES),
        "Require baseline and final revision builds",
    )
    validate_build_pair(builds)
    for build in builds.values():
        verify_build(build)
    return builds


def positive(value: str) -> int:
    result = int(value)
    if result < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return result


def comma_integers(value: str) -> list[int]:
    result = [int(part) for part in value.split(",")]
    require(
        result and min(result) >= 0 and len(result) == len(set(result)),
        "Use distinct nonnegative GPU ordinals",
    )
    return result


def workload_set(
    phase: str, profile: str, devices: list[int], alignment=256, block=32, roots=None
) -> list[dict]:
    tail = math.lcm(alignment, block)
    specs = [
        (1, MIB + tail),
        (1, 64 * MIB + tail),
        (1, 256 * MIB - tail),
        (2, 64 * MIB + tail),
    ]
    if phase != "screen":
        specs = [
            (1, 8 * MIB + tail),
            (1, 128 * MIB + tail),
            (2, 8 * MIB + tail),
            (2, 128 * MIB - tail),
        ]
    if profile == "raised2g":
        specs = [(1, round(1.46 * 1024**3 / tail) * tail)]
    selected_roots = roots or [devices[0], devices[-1]]
    workloads = []
    for keys, size in specs:
        require(
            size % tail == 0 and keys * size <= CAPS[profile],
            "Workload exceeds combined cap/alignment",
        )
        sweep = (
            devices
            if phase == "screen" and keys == 1 and size == 64 * MIB + tail
            else selected_roots
        )
        if phase == "screen" and not (keys == 1 and size == 64 * MIB + tail):
            sweep = [devices[-1]]
        for root in dict.fromkeys(sweep):
            workloads.append(
                {
                    "id": f"builder-k{keys}-b{size}-r{root}",
                    "mode": "builder",
                    "root": root,
                    "K": keys,
                    "B": size,
                    "transport": "automatic",
                    "held_out": phase != "screen" and profile == "default256",
                }
            )
    return workloads


def transport_workloads(devices: list[int]) -> list[dict]:
    workloads = []
    for issuer in ("destination_read", "source_write"):
        for source in devices:
            for destination in devices:
                if source != destination:
                    workloads.append(
                        {
                            "mode": "transport",
                            "pattern": "pair",
                            "issuer": issuer,
                            "source": source,
                            "destination": destination,
                            "root": destination,
                        }
                    )
        workloads.append(
            {
                "mode": "transport",
                "pattern": "all_pairs",
                "issuer": issuer,
                "root": devices[0],
            }
        )
        for root in devices:
            for pattern in ("root_inbound", "root_outbound"):
                workloads.append(
                    {
                        "mode": "transport",
                        "pattern": pattern,
                        "issuer": issuer,
                        "root": root,
                    }
                )
    for index, item in enumerate(workloads):
        item.update(id=f"transport-{index:02d}", B=64 * MIB + 256, held_out=False)
    return workloads


def pin_resources():
    path = ROOT / "test/tpch_performance/tpch_pin_columns.py"
    spec = importlib.util.spec_from_file_location("s4_tpch_pins", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def prepare_sql(args, directory: Path) -> tuple[list[dict], dict]:
    pins = pin_resources()
    queries = (
        list(range(1, 23))
        if args.queries == "all22"
        else [int(q) for q in args.queries.split(",")]
    )
    require(
        queries
        and len(queries) == len(set(queries))
        and all(1 <= q <= 22 for q in queries),
        "Queries must be distinct values in 1..22 or all22",
    )
    source = args.data_path.resolve() if args.data_path else None
    available = source is not None and source.exists()
    dataset = {
        "path": str(source) if source else None,
        "scale": args.scale,
        "scale_provenance": "user_declared; not inferred from path",
        "availability": "available" if available else "unavailable",
        "reason": None if available else "dataset_path_missing",
        "identity_method": "sha256" if args.hash_dataset else "path_size_mtime_ns",
        "files": [],
        "row_counts": None,
        "row_counts_availability": "not_measured_by_orchestrator",
    }
    inputs = directory / "sql"
    inputs.mkdir()
    workloads = []
    observed_files = {}
    for query in queries:
        query_source = ROOT / f"test/tpch_performance/tpch_queries/orig/q{query}.sql"
        query_path = inputs / f"q{query}.sql"
        query_path.write_bytes(query_source.read_bytes())
        query_path.chmod(0o444)
        item = {
            "id": f"sql-q{query}",
            "mode": "sql",
            "query_id": f"q{query}",
            "held_out": args.command != "screen",
            "query_file": str(query_path),
            "query_sha256": file_hash(query_path),
            "available": available,
            "skip_reason": dataset["reason"],
            "database": str(source) if available and source.is_file() else ":memory:",
        }
        statements = [
            "SET gpu_execution = false;",
            f"SET max_dynamic_filter_bloom_bytes_per_gpu = {CAPS[args.profile]};",
            "SET dynamic_filter_domain_coverage_threshold = 0.9;",
            "SET enable_dynamic_filter = true;",
            "SET enable_dynamic_filter_multi_partition = true;",
        ]
        if available:
            for table, columns in pins.QUERY_COLUMNS[query].items():
                col_sql = ",".join(literal(c) for c in columns)
                if source.is_dir():
                    try:
                        pattern = pins.detect_pin_glob(str(source), table)
                    except RuntimeError as error:
                        item.update(available=False, skip_reason=str(error))
                        continue
                    paths = sorted(glob.glob(pattern))
                    paths_sql = ",".join(literal(p) for p in paths)
                    statements.append(
                        f"CREATE VIEW {table} AS SELECT * FROM read_parquet([{paths_sql}]);"
                    )
                    statements.append(
                        f"CALL pin_table({literal(pattern)}, name={literal(table)}, tier={literal(args.pin_tier)}, cols=[{col_sql}], compression=false);"
                    )
                else:
                    paths = [str(source)]
                    statements.append(
                        f"CALL pin_table(format='duckdb', name={literal(table)}, tier={literal(args.pin_tier)}, cols=[{col_sql}], compression=false);"
                    )
                for path in paths:
                    p = Path(path)
                    info = {
                        "path": path,
                        "size": p.stat().st_size,
                        "mtime_ns": p.stat().st_mtime_ns,
                    }
                    if args.hash_dataset:
                        info["sha256"] = (
                            file_hash(p)
                            if path not in observed_files
                            else observed_files[path]["sha256"]
                        )
                    observed_files[path] = info
        statements.append("SET gpu_execution = true;")
        prep = inputs / f"q{query}-prepare.sql"
        prep.write_text("\n".join(statements) + "\n")
        prep.chmod(0o444)
        item.update(prepare_file=str(prep), prepare_sha256=file_hash(prep))
        workloads.append(item)
    dataset["files"] = list(observed_files.values())
    dataset["identity_sha256"] = digest(dataset)
    dataset["query_availability"] = {
        w["query_id"]: {"available": w["available"], "reason": w["skip_reason"]}
        for w in workloads
    }
    return workloads, dataset


def machine_info() -> dict:
    commands = {
        "gpus": [
            "nvidia-smi",
            "--query-gpu=index,uuid,name,pci.bus_id,memory.total,driver_version,pstate,clocks.current.sm,clocks.current.memory,power.limit",
            "--format=csv",
        ],
        "topology": ["nvidia-smi", "topo", "-m"],
        "peer_read": ["nvidia-smi", "topo", "-p2p", "r"],
        "peer_write": ["nvidia-smi", "topo", "-p2p", "w"],
        "load": ["nvidia-smi"],
        "cpu": ["lscpu"],
        "numa": ["numactl", "--show"],
        "toolkit": ["nvcc", "--version"],
        "profiler": ["nsys", "--version"],
    }
    return {
        "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "platform": platform.platform(),
        "hostname": platform.node(),
        "affinity": sorted(os.sched_getaffinity(0)),
        "environment": {
            k: v
            for k, v in os.environ.items()
            if k.startswith(("CUDA_", "SIRIUS_", "OMP_", "LIBCUDF_"))
        },
        "observations": {k: capture(v) for k, v in commands.items()},
        "physical_route_counters": {"availability": "not_measured", "value": None},
        "pinned_memory_placement": {"availability": "not_measured", "value": None},
        "peer_matrix_scope": "driver capability only; real-pool evidence is Stage A",
    }


def execution_plan(workloads: list[dict], args) -> list[dict]:
    rng = random.Random(args.seed)
    clusters = 1 if args.command == "screen" else args.process_pairs
    repeats = args.pairs if args.command == "screen" else args.pairs_per_process
    jobs = []
    next_cluster = 0
    for cluster in range(clusters):
        for stage in args.stages.split(","):
            stage_work = [w for w in workloads if w["mode"] == stage]
            rng.shuffle(stage_work)
            for workload in stage_work:
                comparisons = (
                    ["diagnostic"]
                    if stage != "sql"
                    else ["baseline_final", "baseline_baseline"]
                )
                rng.shuffle(comparisons)
                for comparison in comparisons:
                    identity = next_cluster
                    next_cluster += 1
                    seed = rng.randrange(1, 2**31)
                    arms = (
                        ["candidate"]
                        if comparison == "diagnostic"
                        else ["baseline", "candidate"]
                    )
                    rng.shuffle(arms)
                    for order, arm in enumerate(arms):
                        role = (
                            "final"
                            if comparison == "diagnostic"
                            or (comparison == "baseline_final" and arm == "candidate")
                            else "baseline"
                        )
                        jobs.append(
                            {
                                "workload": workload["id"],
                                "process_cluster": identity,
                                "cluster_index": cluster,
                                "comparison": comparison,
                                "arm": arm,
                                "order": order,
                                "pair": cluster,
                                "seed": seed,
                                "revision_role": role,
                                "warmups": args.warmups,
                                "repetitions": repeats,
                            }
                        )
    return jobs


def fixture_command(
    build: dict, workload: dict, job: dict, run: dict, output: Path
) -> list[str]:
    command = [build["artifacts"]["fixture"]["path"], workload["mode"]]
    options = {
        "output": output,
        "run-id": run["run_id"],
        "sample-id": job["sample_prefix"],
        "build-id": build["build_id"],
        "process-cluster": job["process_cluster"],
        "pair": job["pair"],
        "order": job["order"],
        "seed": job["seed"],
        "warmups": job["warmups"],
        "repetitions": job["repetitions"],
        "devices": ",".join(map(str, run["devices"])),
        "root": workload.get("root", run["devices"][-1]),
        "cap-bytes": run["cap_bytes"],
        "coverage": run["coverage"],
        "profile": run["profile"],
    }
    if workload["mode"] == "builder":
        options.update(
            keys=workload["K"], bytes=workload["B"], transport=workload["transport"]
        )
    elif workload["mode"] == "transport":
        options.update(
            bytes=workload["B"], pattern=workload["pattern"], issuer=workload["issuer"]
        )
        for key in ("source", "destination"):
            if key in workload:
                options[key] = workload[key]
    else:
        options.update(
            {
                "database": workload["database"],
                "query-file": workload["query_file"],
                "prepare-file": workload["prepare_file"],
                "query-id": workload["query_id"],
                "dataset": run["dataset"]["identity_sha256"],
                "profile": run["profile"],
            }
        )
    for key, value in options.items():
        command.extend(["--" + key, str(value)])
    return command


def validate_native(
    records: list[dict], build: dict, workload: dict, job: dict, run: dict
) -> None:
    require(
        len(records) == job["warmups"] + job["repetitions"],
        "Native record count does not match request",
    )
    seen = set()
    for record in records:
        for key, expected in {
            "schema_version": 2,
            "run_id": run["run_id"],
            "build_id": build["build_id"],
            "mode": workload["mode"],
            "process_cluster": job["process_cluster"],
            "pair": job["pair"],
            "order": job["order"],
            "seed": job["seed"],
            "revision_role": job["revision_role"],
            "devices": run["devices"],
            "profile": run["profile"],
            "cap_bytes": run["cap_bytes"],
            "coverage": run["coverage"],
        }.items():
            require(
                record.get(key) == expected,
                f"Native {key} mismatch: {record.get(key)!r} != {expected!r}",
            )
        require(
            type(record.get("warmup")) is bool
            and type(record.get("repetition")) is int,
            "Invalid repetition identity",
        )
        key = (record["warmup"], record["repetition"])
        count = job["warmups"] if key[0] else job["repetitions"]
        require(
            0 <= key[1] < count and key not in seen,
            "Duplicate/out-of-range native repetition",
        )
        seen.add(key)
        suffix = "warmup" if key[0] else "sample"
        require(
            record.get("sample_id") == f"{job['sample_prefix']}:{suffix}:{key[1]}",
            "Wrong native sample ID",
        )
        require(
            record.get("status") in ("ok", "unavailable", "skipped", "error"),
            "Unknown native status",
        )
        for name in METRICS:
            require(name in record, f"Missing native field: {name}")
            require(
                record[name] is None
                or (type(record[name]) is int and record[name] >= 0),
                f"Invalid metric: {name}",
            )
        if record["status"] == "ok":
            require(
                record.get("oracle_outcome") == "passed",
                "Successful sample did not pass oracle",
            )
            require(
                record.get("oracle_passed") is True,
                "Successful sample has no oracle success",
            )
            metric = (
                "query_elapsed_ns" if workload["mode"] == "sql" else "host_elapsed_ns"
            )
            require(
                record[metric] is not None and record[metric] > 0,
                f"Missing positive {metric}",
            )
            if workload["mode"] == "sql":
                require(
                    oracle_policy(record) == oracle_policy(build["identify"]),
                    "SQL oracle policy differs from the fixture identity",
                )
                require(
                    type(record.get("gpu_intercepted")) is bool,
                    "Missing SQL interception evidence",
                )
                require(
                    record.get("profile_verified") is True,
                    "SQL profile was not verified",
                )
                require(
                    record.get("publication_metrics_supported")
                    == build["identify"]["publication_metrics_supported"],
                    "SQL metric availability differs from identify",
                )
                for name in COMMON_SQL_OUTCOMES:
                    require(
                        type(record.get(name)) is int and record[name] >= 0,
                        f"Missing/invalid common SQL outcome: {name}",
                    )
                if not record["publication_metrics_supported"]:
                    require(
                        all(
                            record[name] is None
                            for name in (
                                "host_elapsed_ns",
                                "input_bitmap_bytes",
                                "ready_replica_bytes",
                                "published_bitmap_bytes",
                            )
                        ),
                        "Unsupported publication metrics must remain null",
                    )
            if workload["mode"] == "builder":
                require(record.get("root") == workload["root"], "Builder root mismatch")
                require(
                    record["B"] == workload["B"] and record["K"] == workload["K"],
                    "Builder geometry mismatch",
                )
                require(
                    record["ready_replica_bytes"]
                    == len(run["devices"]) * workload["K"] * workload["B"],
                    "Incomplete builder replicas",
                )
                if workload["transport"] == "force_host":
                    require(
                        record.get("actual_transport") == "host",
                        "Forced HOST did not use HOST transport",
                    )
        else:
            require(
                bool(record.get("skip_reason")),
                "Non-success requires an explicit reason",
            )
            require(
                record["status"] != "error", f"Native error: {record['skip_reason']}"
            )


def run_process(command: list[str], env: dict, directory: Path, timeout: int) -> None:
    with (directory / "stdout.log").open("x") as stdout, (
        directory / "stderr.log"
    ).open("x") as stderr:
        with subprocess.Popen(
            command,
            stdout=stdout,
            stderr=stderr,
            env=env,
            cwd=directory,
            start_new_session=True,
        ) as process:
            try:
                code = process.wait(timeout=timeout)
            except BaseException:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
                raise
    require(code == 0, f"Fixture exited {code}; see {directory / 'stderr.log'}")


def cli_smoke(build: dict, config: Path, directory: Path, timeout: int) -> None:
    directory.mkdir(parents=True)
    env = build_environment(build)
    env["SIRIUS_CONFIG_FILE"] = str(config)
    env.pop("SIRIUS_DISABLE", None)
    env["LD_DEBUG"] = "libs"
    sql = f"LOAD {literal(build['artifacts']['extension']['path'])}; SELECT 's4_extension_loaded';"
    command = [
        build["artifacts"]["cli"]["path"],
        "-unsigned",
        "-bail",
        "-no-agent",
        "-batch",
        "-c",
        sql,
    ]
    write_json(
        directory / "request.json",
        {
            "command": command,
            "config": str(config),
            "library_search_paths": build["library_search_paths"],
        },
    )
    run_process(command, env, directory, timeout)
    require(
        "s4_extension_loaded" in (directory / "stdout.log").read_text(),
        "CLI did not finish extension-load smoke",
    )
    library = Path(build["artifacts"]["library"]["path"])
    loaded = re.findall(
        r"calling init:\s*(\S*libsirius\S*)", (directory / "stderr.log").read_text()
    )
    require(
        loaded and all(Path(path).resolve() == library.resolve() for path in loaded),
        "CLI loader did not initialize the frozen shared library",
    )


def verify_inputs(run: dict) -> None:
    for workload in run["workloads"]:
        if workload["mode"] == "sql":
            for prefix in ("query", "prepare"):
                require(
                    file_hash(Path(workload[prefix + "_file"]))
                    == workload[prefix + "_sha256"],
                    f"Frozen SQL input changed: {workload['id']} {prefix}",
                )
    require(
        artifact(Path(run["config"]["path"])) == run["config"],
        "Sirius configuration changed",
    )
    for item in run["dataset"].get("files", []):
        stat = Path(item["path"]).stat()
        require(
            (stat.st_size, stat.st_mtime_ns) == (item["size"], item["mtime_ns"]),
            f"Dataset changed during campaign: {item['path']}",
        )


def campaign(args) -> None:
    devices = comma_integers(args.gpus)
    require(
        len(devices) in (1, 2, 4), "Use one/two GPUs for smoke or four for the handoff"
    )
    stages = args.stages.split(",")
    require(
        stages
        and len(stages) == len(set(stages))
        and set(stages) <= {"transport", "builder", "sql"},
        "Invalid stages",
    )
    require(
        "transport" not in stages or stages[0] == "transport",
        "Stage A transport must precede Stage B",
    )
    require(args.warmups >= 0, "Warmups must be nonnegative")
    require(0 <= args.seed < 2**63, "Seed must be a nonnegative 63-bit integer")
    require(
        args.process_pairs in (5, 10),
        "Confirm/regression use five clusters, or the predeclared extension to ten",
    )
    if args.command == "regression":
        require(
            stages == ["sql"]
            and args.queries == "all22"
            and args.profile == "default256",
            "Regression requires sql, all22, default256",
        )
    output = args.output.resolve()
    require(not output.exists(), f"Refusing to reuse result directory {output}")
    builds = load_builds(args.build_manifest)
    output.mkdir(parents=True)
    identify_record = builds["final"]["identify"]
    roots = comma_integers(args.roots) if args.roots else None
    require(
        roots is None or set(roots) <= set(devices),
        "Roots must be selected GPU ordinals",
    )
    workloads = transport_workloads(devices) if "transport" in stages else []
    if "builder" in stages:
        workloads += workload_set(
            args.command,
            args.profile,
            devices,
            identify_record["cuda_allocation_alignment"],
            identify_record["bloom_block_bytes"],
            roots,
        )
        if args.command == "screen":
            host = dict(workloads[-1])
            host.update(
                id="builder-force-host",
                K=2,
                B=64 * MIB
                + math.lcm(
                    identify_record["cuda_allocation_alignment"],
                    identify_record["bloom_block_bytes"],
                ),
                transport="force_host",
            )
            workloads.append(host)
    dataset = {"availability": "not_requested", "identity_sha256": None}
    if "sql" in stages:
        sql_work, dataset = prepare_sql(args, output)
        workloads += sql_work
    config = output / "sirius.yaml"
    if args.sirius_config:
        config.write_bytes(args.sirius_config.read_bytes())
    else:
        config.write_text(
            "sirius:\n  topology:\n    gpu_ids: ["
            + ", ".join(map(str, devices))
            + "]\n"
            "  memory:\n    gpu:\n      usage_limit_fraction: 0.4\n"
            "    host:\n      capacity_fraction: 0.5\n      initial_number_pools: 2\n"
            "      pool_size: 128\n      block_size: 1048576\n"
        )
    config.chmod(0o444)
    jobs = execution_plan(workloads, args)
    run_id = output.name + "-" + str(time.time_ns())
    for index, job in enumerate(jobs):
        job["sample_prefix"] = f"{run_id}:p{index:05d}"
        job["process_id"] = f"p{index:05d}"
    run = {
        "schema_version": 2,
        "comparison_contract": "revision_sql_v2",
        "baseline_kind": builds["baseline"]["baseline_kind"],
        "run_id": run_id,
        "phase": args.command,
        "devices": devices,
        "four_gpu_handoff": len(devices) == 4,
        "profile": args.profile,
        "cap_bytes": CAPS[args.profile],
        "coverage": 0.9,
        "test_options_enabled": True,
        "seed": args.seed,
        "dataset": dataset,
        "pin_tier": args.pin_tier,
        "stages": stages,
        "builds": builds,
        "harness_sources": {
            name: artifact(ROOT / "scripts" / filename)
            for name, filename in (
                ("runner", "run_s4_experiments.py"),
                ("analyzer", "analyze_s4_experiments.py"),
                ("build_helper", "build_s4_experiments.py"),
            )
        },
        "workloads": workloads,
        "jobs": jobs,
        "config": artifact(config),
        "stage_a_results": (
            str(args.stage_a_results.resolve()) if args.stage_a_results else None
        ),
        "stage_a_experiment_sha256": (
            file_hash(args.stage_a_results / "experiment.json")
            if args.stage_a_results
            else None
        ),
        "thresholds": {
            "query_slowdown_upper": 0.03,
            "interval": "two-sided 95% Student-t on independent process-pair mean log ratios",
            "minimum_clusters": 5,
            "maximum_clusters": 10,
        },
        "cpu_oracle": "native fixture; outside measured query; same pinned dataset",
        "scale": args.scale,
        "argv": sys.argv,
    }
    if args.extend_from:
        require(
            args.command in ("confirm", "regression") and args.process_pairs == 5,
            "An extension adds five clusters to confirm/regression",
        )
        prior = load_json(args.extend_from / "experiment.json")
        completion = load_json(args.extend_from / "completion.json")
        require(
            completion.get("status") == "complete" and not prior.get("extension_of"),
            "Extend one completed original campaign exactly once",
        )
        require(
            max(j["cluster_index"] for j in prior["jobs"]) == 4,
            "Extension requires five original clusters",
        )
        require(prior["seed"] != args.seed, "Extension requires a fresh seed")
        for field in (
            "phase",
            "devices",
            "profile",
            "scale",
            "pin_tier",
            "stages",
            "builds",
            "dataset",
        ):
            require(prior[field] == run[field], f"Extension differs in {field}")
        require(
            prior["config"]["sha256"] == run["config"]["sha256"],
            "Extension config differs",
        )

        def comparable(items):
            return [
                {
                    k: v
                    for k, v in item.items()
                    if k not in ("query_file", "prepare_file")
                }
                for item in items
            ]

        require(
            comparable(prior["workloads"]) == comparable(workloads),
            "Extension workloads differ",
        )
        offset = 1 + max(j["process_cluster"] for j in prior["jobs"])
        for job in jobs:
            job["process_cluster"] += offset
            job["cluster_index"] += 5
        run["extension_of"] = {
            "directory": str(args.extend_from.resolve()),
            "experiment_sha256": file_hash(args.extend_from / "experiment.json"),
            "samples_sha256": file_hash(args.extend_from / "samples.jsonl"),
        }
    write_json(output / "experiment.json", run)
    write_json(output / "machine-before.json", machine_info())
    native_artifact_stats = {
        a["path"]: (Path(a["path"]).stat().st_size, Path(a["path"]).stat().st_mtime_ns)
        for b in builds.values()
        for a in b["artifacts"].values()
    }
    workload_by_id = {w["id"]: w for w in workloads}
    completed = 0
    try:
        for role, build in builds.items():
            cli_smoke(build, config, output / "cli-smoke" / role, args.timeout)
        with (output / "samples.jsonl").open("x") as aggregate:
            for job in jobs:
                workload = workload_by_id[job["workload"]]
                build = builds[job["revision_role"]]
                verify_inputs(run)
                directory = output / "processes" / job["process_id"]
                directory.mkdir(parents=True)
                print(
                    f"{job['process_id']} {workload['id']} {job['comparison']} {job['arm']}",
                    flush=True,
                )
                if workload.get("available") is False:
                    for warmup, count in (
                        (True, job["warmups"]),
                        (False, job["repetitions"]),
                    ):
                        for repetition in range(count):
                            suffix = "warmup" if warmup else "sample"
                            record = {k: None for k in METRICS}
                            record.update(
                                schema_version=2,
                                run_id=run_id,
                                sample_id=f"{job['sample_prefix']}:{suffix}:{repetition}",
                                build_id=build["build_id"],
                                mode=workload["mode"],
                                revision_role=job["revision_role"],
                                process_cluster=job["process_cluster"],
                                pair=job["pair"],
                                order=job["order"],
                                seed=job["seed"],
                                warmup=warmup,
                                repetition=repetition,
                                devices=devices,
                                root=None,
                                status="unavailable",
                                skip_reason=workload["skip_reason"],
                                oracle_outcome="not_measured",
                                oracle_passed=None,
                                gpu_intercepted=None,
                                profile=args.profile,
                                cap_bytes=run["cap_bytes"],
                                coverage=run["coverage"],
                            )
                            record.update(
                                harness={
                                    **job,
                                    "phase": args.command,
                                    "profile": args.profile,
                                    "held_out": workload["held_out"],
                                }
                            )
                            aggregate.write(json.dumps(record, allow_nan=False) + "\n")
                    aggregate.flush()
                    completed += 1
                    continue
                for path, stamp in native_artifact_stats.items():
                    stat = Path(path).stat()
                    require(
                        (stat.st_size, stat.st_mtime_ns) == stamp,
                        f"Artifact mutated during campaign: {path}",
                    )
                raw = directory / "native.jsonl"
                command = fixture_command(build, workload, job, run, raw)
                env = fixture_environment(build, config)
                write_json(
                    directory / "request.json",
                    {
                        "command": command,
                        "job": job,
                        "library_search_paths": build["library_search_paths"],
                        "environment_overrides": {
                            "SIRIUS_CONFIG_FILE": str(config),
                            "SIRIUS_ENABLE_TEST_OPTIONS": "1",
                        },
                    },
                )
                run_process(command, env, directory, args.timeout)
                records = jsonl(raw)
                validate_native(records, build, workload, job, run)
                for record in records:
                    record["harness"] = {
                        **job,
                        "phase": args.command,
                        "profile": args.profile,
                        "held_out": workload["held_out"],
                        "raw_path": str(raw),
                    }
                    aggregate.write(json.dumps(record, allow_nan=False) + "\n")
                aggregate.flush()
                completed += 1
        for build in builds.values():
            verify_build(build)
        verify_inputs(run)
        write_json(
            output / "completion.json",
            {
                "status": "complete",
                "processes": completed,
                "samples_sha256": file_hash(output / "samples.jsonl"),
                "experiment_sha256": file_hash(output / "experiment.json"),
            },
        )
    except BaseException as error:
        write_json(
            output / "failure.json",
            {"status": "failed", "completed_processes": completed, "error": str(error)},
        )
        raise
    finally:
        write_json(output / "machine-after.json", machine_info())
    print(
        f"Saved {output}; analyze with scripts/analyze_s4_experiments.py --results {output}"
    )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    sub = result.add_subparsers(dest="command", required=True)
    manifest = sub.add_parser(
        "manifest",
        help="Hash frozen artifacts and verify fixture revision metadata (CPU only)",
    )
    for name in ("fixture", "library", "extension", "cli", "output"):
        manifest.add_argument("--" + name, type=Path, required=True)
    manifest.add_argument("--revision-role", choices=REVISION_ROLES, required=True)
    manifest.add_argument("--source-root", type=Path, required=True)
    manifest.add_argument("--build-dir", type=Path, required=True)
    manifest.add_argument(
        "--baseline-kind", choices=BASELINE_REVISIONS, default="prerequisite"
    )
    manifest.add_argument("--cmake-cache", type=Path)
    pair = sub.add_parser(
        "pair-manifests", help="Validate and freeze two compatible build manifests"
    )
    for name in ("baseline", "final", "output"):
        pair.add_argument("--" + name, type=Path, required=True)
    for name in ("screen", "confirm", "regression"):
        run = sub.add_parser(
            name, help="Run GPU experiments; processes execute sequentially"
        )
        run.add_argument("--build-manifest", type=Path, required=True)
        run.add_argument(
            "--gpus", required=True, help="Distinct CUDA-visible ordinals, e.g. 0,1,2,3"
        )
        run.add_argument(
            "--output",
            type=Path,
            required=True,
            help="New result directory; never overwritten",
        )
        run.add_argument(
            "--stages",
            default=(
                "transport,builder,sql"
                if name == "screen"
                else "sql" if name == "regression" else "builder,sql"
            ),
        )
        run.add_argument("--profile", choices=CAPS, default="default256")
        run.add_argument(
            "--queries", default="all22" if name == "regression" else "4,21,7"
        )
        run.add_argument(
            "--data-path",
            type=Path,
            help="TPC-H DuckDB database or directory of Parquet tables; absence is recorded",
        )
        run.add_argument(
            "--scale",
            default="100",
            help="User-declared scale; use unknown/tiny for unverified fixture data",
        )
        run.add_argument(
            "--hash-dataset",
            action="store_true",
            help="Hash dataset bytes in addition to path/size/mtime identity",
        )
        run.add_argument("--pin-tier", choices=("gpu", "host"), default="gpu")
        run.add_argument(
            "--sirius-config",
            type=Path,
            help="Copy an explicit machine config; its topology must match --gpus",
        )
        run.add_argument(
            "--roots",
            help="Confirmation representative/adverse roots; default first and last GPU",
        )
        run.add_argument(
            "--stage-a-results",
            type=Path,
            help="Optional prior final-host transport diagnostics directory",
        )
        run.add_argument(
            "--extend-from",
            type=Path,
            help="Add the single allowed five-cluster extension to a completed campaign; use a fresh seed",
        )
        run.add_argument("--warmups", type=int, default=3)
        run.add_argument("--pairs", type=positive, default=3)
        run.add_argument(
            "--process-pairs",
            type=positive,
            default=5,
            help="Five clusters, or one predeclared extension to ten",
        )
        run.add_argument("--pairs-per-process", type=positive, default=6)
        run.add_argument(
            "--seed",
            type=int,
            default={"screen": 1823, "confirm": 1824, "regression": 1825}[name],
        )
        run.add_argument(
            "--timeout",
            type=positive,
            default=3600,
            help="Timeout in seconds per native process",
        )
    return result


def main(argv=None) -> int:
    args = parser().parse_args(argv)
    try:
        if args.command == "manifest":
            manifest_command(args)
        elif args.command == "pair-manifests":
            pair_manifests(args)
        else:
            campaign(args)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"S4: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
