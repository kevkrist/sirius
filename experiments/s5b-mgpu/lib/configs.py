#!/usr/bin/env python3
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
"""Checks the Sirius configs of S0 'configs' and records them.

The kit chooses no memory, cache, compression or telemetry setting: every stage uses the machine's
own 4-GPU Sirius config (`S5B_SIRIUS_CONFIG`), copied unchanged as `sirius_4gpu.yaml`. The 1- and
2-GPU configs are copies changed by `scripts/patch_config.py --opt sirius.topology.num_gpus=<n>`
and nothing else. This script proves that: it refuses a machine config that does not select 4 GPUs
(or selects them by `gpu_ids`) and a derived config that differs in any other key. It copies the
configs to `results/S0/configs/` and prints the facts the S0 report needs.
"""

from __future__ import annotations

import argparse
import copy
import pathlib
import shutil
import sys
from typing import Any

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import common  # noqa: E402

MACHINE_GPUS = 4


def _get(config: dict, path: str) -> Any:
    node: Any = config
    for part in path.split("."):
        if not isinstance(node, dict) or part not in node:
            return None
        node = node[part]
    return node


def without_num_gpus(config: dict) -> dict:
    stripped = copy.deepcopy(config)
    topology = _get(stripped, "sirius.topology")
    if isinstance(topology, dict):
        topology.pop("num_gpus", None)
        if not topology:
            stripped["sirius"].pop("topology")
    return stripped


def machine_problems(config: dict, visible_gpus: int) -> list[str]:
    """Why @p config cannot serve as the 4-GPU config, if it cannot."""
    if not isinstance(config.get("sirius"), dict):
        return ["the config has no top-level `sirius:` section"]
    problems = []
    if _get(config, "sirius.topology.gpu_ids") is not None:
        problems.append(
            "it selects GPUs with topology.gpu_ids; the kit varies only topology.num_gpus"
        )
    num_gpus = _get(config, "sirius.topology.num_gpus")
    if num_gpus in (None, 0):
        if visible_gpus != MACHINE_GPUS:
            problems.append(
                f"num_gpus is automatic and {visible_gpus} GPUs are visible, not 4"
            )
    elif num_gpus != MACHINE_GPUS:
        problems.append(f"topology.num_gpus is {num_gpus}, not {MACHINE_GPUS}")
    return problems


def derived_problems(machine: dict, derived: dict, gpus: int) -> list[str]:
    """Differences between @p derived and @p machine other than topology.num_gpus = @p gpus."""
    problems = []
    if _get(derived, "sirius.topology.num_gpus") != gpus:
        problems.append(
            f"topology.num_gpus is {_get(derived, 'sirius.topology.num_gpus')}, not {gpus}"
        )
    if without_num_gpus(derived) != without_num_gpus(machine):
        problems.append(
            "it differs from the machine config in more than topology.num_gpus"
        )
    return problems


def facts(config: dict) -> list[str]:
    """The settings of @p config the S0 report names (absent means the Sirius default)."""
    keys = (
        "sirius.topology.num_gpus",
        "sirius.memory.gpu.usage_limit_fraction",
        "sirius.memory.gpu.usage_limit_bytes",
        "sirius.memory.host.capacity_fraction",
        "sirius.memory.host.capacity_bytes",
        "sirius.memory.disk.downgrade_root_dirs",
        "sirius.executor.scan_manager.cache.mode",
        "sirius.compression.enable_pin_table_compression",
        "sirius.compression.input_plan_dir",
        "sirius.telemetry.enable_quent",
        "sirius.telemetry.output_directory",
    )
    return [
        f"{key}: {_get(config, key) if _get(config, key) is not None else '(default)'}"
        for key in keys
    ]


def main() -> int:
    import yaml  # noqa: PLC0415 - pyyaml is in the pixi environment, not needed by selftest

    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--machine", type=pathlib.Path, required=True)
    parser.add_argument("--visible-gpus", type=int, required=True)
    parser.add_argument("--gpus", default="1,2", help="derived GPU counts")
    args = parser.parse_args()
    configs = common.env_path("S5B_CONFIGS")
    machine = yaml.safe_load(args.machine.read_text()) or {}
    problems = [
        f"machine config {args.machine}: {p}"
        for p in machine_problems(machine, args.visible_gpus)
    ]
    for gpus in (int(g) for g in args.gpus.split(",")):
        path = configs / f"sirius_{gpus}gpu.yaml"
        derived = yaml.safe_load(path.read_text()) or {}
        problems += [f"{path}: {p}" for p in derived_problems(machine, derived, gpus)]
    four = (
        yaml.safe_load((configs / f"sirius_{MACHINE_GPUS}gpu.yaml").read_text()) or {}
    )
    if four != machine:
        problems.append(
            f"sirius_{MACHINE_GPUS}gpu.yaml is not an unchanged copy of the machine config"
        )
    copies = common.results_dir("S0") / "configs"
    copies.mkdir(parents=True, exist_ok=True)
    for path in sorted(configs.glob("sirius_*gpu.yaml")):
        shutil.copy(path, copies / path.name)
    shutil.copy(args.machine, copies / "machine_config.yaml")
    print("\n".join(f"  {line}" for line in facts(machine)))
    if problems:
        print("\n".join(f"error: {p}" for p in problems), file=sys.stderr)
        return 1
    common.log(f"configs ok; copies in {copies}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
