#!/usr/bin/env python3
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
"""Build and freeze matched S4 artifacts; run through the repository's Pixi environment."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def run(command: list[str], cwd: Path, env: dict[str, str]) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, env=env, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help="New directory for immutable artifacts",
    )
    parser.add_argument("--build-dir", type=Path, default=Path("build/release"))
    parser.add_argument(
        "--cuda-architectures",
        required=True,
        help="CMake CUDA architectures for this host",
    )
    parser.add_argument("--jobs", type=int, default=24)
    parser.add_argument(
        "--cmake-option",
        action="append",
        default=[],
        help="Additional -D option; use --cmake-option=-DNAME=VALUE",
    )
    parser.add_argument(
        "--schedule", action="append", choices=["current_direct", "concurrent_ingress"]
    )
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    build = args.build_dir.resolve()
    if output.exists():
        parser.error(
            "--output must not exist; existing artifacts are never overwritten"
        )
    if not os.environ.get("CONDA_PREFIX"):
        parser.error("Run with pixi run python scripts/build_s4_experiments.py ...")
    for name in [
        "duckdb",
        "cucascade",
        "substrait",
        "sirius-duckdb/duckdb",
        "sirius-duckdb/extension-ci-tools",
    ]:
        if not (root / name / ".git").exists():
            parser.error(f"Initialize submodule {name} before building")
    schedules = args.schedule or ["current_direct", "concurrent_ingress"]
    if len(set(schedules)) != len(schedules):
        parser.error("--schedule values must be distinct")
    output.mkdir(parents=True)
    env = dict(os.environ, CMAKE_BUILD_PARALLEL_LEVEL=str(args.jobs))
    manifests: dict[str, str] = {}
    for schedule in schedules:
        dest = output / schedule
        dest.mkdir()
        install = dest / "install"
        run(
            [
                "cmake",
                "--preset",
                "release",
                "-B",
                str(build),
                f'-DSIRIUS_DUCKDB_SOURCE_DIR={root / "duckdb"}',
                f"-DCMAKE_CUDA_ARCHITECTURES={args.cuda_architectures}",
                f"-DSIRIUS_S4_PUBLICATION_SCHEDULE={schedule}",
                *args.cmake_option,
            ],
            root,
            env,
        )
        run(
            [
                "cmake",
                "--build",
                str(build),
                "--parallel",
                str(args.jobs),
                "--target",
                "sirius_library",
                "sirius_s4_experiment",
                "sirius_unittest",
            ],
            root,
            env,
        )
        run(
            [
                "cmake",
                "--install",
                str(build),
                "--prefix",
                str(install),
                "--component",
                "sirius_library",
            ],
            root,
            env,
        )
        run(
            [
                "make",
                "-C",
                "sirius-duckdb",
                "release",
                "SIRIUS_DUCKDB_LINKAGE=shared",
                f"SIRIUS_INSTALL_DIR={install}",
                "EXT_FLAGS=-DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache -Usirius_DIR",
            ],
            root,
            env,
        )
        artifacts = {
            "fixture": build / "test/cpp/sirius_s4_experiment",
            "unit_tests": build / "test/cpp/sirius_unittest",
            "fault_tests": build / "test/cpp/sirius_cuda_fault_test",
            "cli": root / "sirius-duckdb/build/release/duckdb",
            "extension": root
            / "sirius-duckdb/build/release/extension/sirius/sirius.duckdb_extension",
        }
        for name, source in artifacts.items():
            target = dest / source.name
            shutil.copy2(source, target)
            artifacts[name] = target
        shutil.copy2(build / "CMakeCache.txt", dest / "CMakeCache.txt")
        (dest / "build-command.json").write_text(
            json.dumps(
                {
                    "argv": sys.argv,
                    "schedule": schedule,
                    "source": str(root),
                    "build_dir": str(build),
                },
                indent=2,
            )
            + "\n"
        )
        manifest = dest / "manifest.json"
        # Manifest generation verifies the linked production schedule and hashes the frozen artifacts.
        run(
            [
                sys.executable,
                str(root / "scripts/run_s4_experiments.py"),
                "manifest",
                "--fixture",
                str(artifacts["fixture"]),
                "--library",
                str(install / "lib/libsirius.so"),
                "--extension",
                str(artifacts["extension"]),
                "--cli",
                str(artifacts["cli"]),
                "--schedule",
                schedule,
                "--cmake-cache",
                str(dest / "CMakeCache.txt"),
                "--baseline-tree",
                "9f59d68e4cdc7ca47999c3579407f6f5168ea652",
                "--output",
                str(manifest),
            ],
            root,
            env,
        )
        manifests[schedule] = str(manifest)
    (output / "builds.json").write_text(json.dumps(manifests, indent=2) + "\n")
    if len(manifests) == 2:
        run(
            [
                sys.executable,
                str(root / "scripts/run_s4_experiments.py"),
                "pair-manifests",
                "--baseline",
                manifests["current_direct"],
                "--candidate",
                manifests["concurrent_ingress"],
                "--output",
                str(output / "build-manifest.json"),
            ],
            root,
            env,
        )
    print(f"Matched artifacts: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
