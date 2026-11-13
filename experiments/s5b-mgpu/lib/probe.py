#!/usr/bin/env python3
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
"""Reads back the settings a loaded Sirius resolves from each generated config, and smoke-tests it.

For each (build, GPU count) it loads the extension with test options on, records the sizing
settings the stages derive from (H = hash_partition_bytes, and others), runs
`SELECT count(*) FROM lineitem` on TPC-H SF1 from disk and records which GPUs the `gpu_pool` lines
name, then pins `nation` on the host tier as the P stages pin every table and checks that a scan of
it is served from the pin (`pinned_scans`). Writes `S5B_WORK/settings/<build>_<n>gpu.json` and
`results/S0/settings.parquet`.
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import common  # noqa: E402
import duck  # noqa: E402
import schemas  # noqa: E402

SETTINGS = (
    "hash_partition_bytes",
    "concat_batch_bytes",
    "max_build_hash_table_bytes",
    "max_build_probe_resident_bytes",
    "max_broadcast_join_size",
    "scan_task_batch_size",
    "build_fold_policy",
    "enable_dynamic_filter",
    "enable_dynamic_zone_map_filter",
    "max_dynamic_filter_bloom_bytes_per_gpu",
)


def probe(build: str, gpus: int) -> dict[str, str]:
    directory = common.work_dir("S0", "probe", f"{build}_{gpus}gpu")
    shutil.rmtree(directory)  # a rerun must not count an earlier run's log lines
    for sub in ("log", "pin", "end"):
        (directory / sub).mkdir(parents=True)
    names = ", ".join(duck.sql_string(name) for name in SETTINGS)
    nation = {"nation": duck.tables(1)["nation"]}
    script = (
        "\n".join(
            [
                *duck.gpu_preamble(build, 1),
                "SET gpu_execution = false;",
                ".mode csv",
                ".headers on",
                f'.once "{directory / "settings.csv"}"',
                f"SELECT name, value FROM duckdb_settings() WHERE name IN ({names}) ORDER BY name;",
                "RESET gpu_execution;",
                f'.once "{directory / "count.csv"}"',
                "SELECT count(*) AS n FROM lineitem;",
                ".mode trash",
                f"SET sirius_log_dir = {duck.sql_string(str(directory / 'pin'))};",
                *duck.pin_statements(nation),
                "SELECT sum(n_nationkey) FROM nation;",
                f"SET sirius_log_dir = {duck.sql_string(str(directory / 'end'))};",
            ]
        )
        + "\n"
    )
    common.wait_for_idle_gpus()
    run = duck.run(
        build,
        script,
        directory,
        config=common.config_file(gpus),
        log_dir=directory / "log",
        log_level="info",
        test_options=True,
        timeout_s=900,
    )
    if run.returncode != 0:
        sys.exit(
            f"error: probe {build}/{gpus} GPU(s) failed; see {directory}/stderr.txt"
        )
    settings = {
        row["name"]: row["value"]
        for row in common.read_csv_dicts(directory / "settings.csv")
    }
    events = common.parse_log_dir(directory / "log")
    pinned = common.parse_log_dir(directory / "pin")
    settings["gpus_seen"] = ",".join(str(g) for g in sorted(events.gpu_peak_bytes))
    settings["lineitem_rows_sf1"] = common.read_csv_dicts(directory / "count.csv")[0][
        "n"
    ]
    settings["gpu_completed"] = str(events.gpu_completed)
    settings["fallback"] = str(
        bool(events.fallback or pinned.fallback or run.fallback_banner)
    )
    settings["pinned_scans"] = str(pinned.pinned_scans)
    settings["pin_compressed"] = str(bool(pinned.compressed_pins))
    return settings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--cells",
        default="tip:1,tip:2,tip:4,base:1,base:4,key:2,key:4",
        help="comma list of build:gpus",
    )
    args = parser.parse_args()
    rows = []
    out = common.results_dir("S0") / "settings.parquet"
    for cell in args.cells.split(","):
        build, gpus = cell.split(":")
        settings = probe(build, int(gpus))
        common.write_json_atomic(
            common.work_dir("settings") / f"{build}_{gpus}gpu.json", settings
        )
        commit = common.git_head(common.worktree(build))
        rows += [
            {"build": build, "commit": commit, "gpus": int(gpus), "name": k, "value": v}
            for k, v in sorted(settings.items())
        ]
        common.log(
            f"{build} {gpus} GPU(s): H={settings.get('hash_partition_bytes')} "
            f"concat={settings.get('concat_batch_bytes')} gpus_seen={settings['gpus_seen']} "
            f"fallback={settings['fallback']} pinned_scans={settings['pinned_scans']}"
        )
    schemas.write_table("S0.settings", rows, out)
    common.log(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
