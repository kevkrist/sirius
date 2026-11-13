# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
"""Runs one DuckDB CLI process of a build worktree, with or without the Sirius extension.

The process reads its script from a file and writes stdout and stderr to files in its own
directory, which is its working directory too, so files Sirius writes relative to the working
directory stay with the run. While it runs, `run` samples the process's peak resident memory, the
host's lowest available memory and the size of a watched directory, and kills the process at its
hard timeout or when its progress counter stops advancing. Callers check that the GPUs are idle
first.
"""

from __future__ import annotations

import dataclasses
import os
import pathlib
import re
import signal
import subprocess
import time
from typing import Callable

import common
import formats


@dataclasses.dataclass
class CliRun:
    returncode: int | None
    stdout: str
    stderr: str
    timed_out: bool = False
    stalled: bool = False
    wall_s: float = 0.0
    host_rss_peak_bytes: int | None = None
    mem_available_min_bytes: int | None = None
    disk_peak_bytes: int | None = None

    @property
    def timer_seconds(self) -> list[float]:
        """The `.timer` readings, in statement order."""
        return [
            float(m["real"])
            for m in map(formats.TIMER.match, self.stdout.splitlines())
            if m
        ]

    @property
    def fallback_banner(self) -> bool:
        return formats.FALLBACK_BANNER in self.stdout

    @property
    def status(self) -> str:
        if self.timed_out:
            return "timeout"
        if self.stalled:
            return "stalled"
        return "ok" if self.returncode == 0 else "error"


def sql_string(text: str) -> str:
    return "'" + text.replace("'", "''") + "'"


def tables(sf: int) -> dict[str, list[pathlib.Path]]:
    """Each TPC-H table's Parquet files, as the parity script lists them."""
    parity = common.load_parity_module()
    return parity.parquet_tables(common.tpch_dir(sf))


def view_statements(sf: int) -> list[str]:
    """One view per TPC-H table over read_parquet, as the parity script creates them."""
    parity = common.load_parity_module()
    return parity.view_statements(tables(sf))


def pin_glob(name: str, files: list[pathlib.Path]) -> str:
    """A glob whose expansion is exactly @p files, the view's file list.

    Sirius serves a scan from a pinned table only when the pin's file list equals the scan's
    (`sirius_scan_manager.cpp`). `parquet_tables` lists either one `<table>.parquet` file or every
    `*.parquet` file of a `<table>/` directory.
    """
    if len(files) == 1 and files[0].name == f"{name}.parquet":
        return str(files[0])
    return str(files[0].parent / "*.parquet")


def pin_statements(table_files: dict[str, list[pathlib.Path]]) -> list[str]:
    """`CALL pin_table` for every table on the host tier; compression follows the config."""
    return [
        f"CALL pin_table({sql_string(pin_glob(name, files))}, tier => 'host', "
        f"name => {sql_string(name)});"
        for name, files in table_files.items()
    ]


def gpu_preamble(build: str, sf: int) -> list[str]:
    """LOAD plus the views; views bind with GPU execution off, as the parity script does."""
    return [
        f"LOAD {sql_string(str(common.extension(build)))};",
        "SET gpu_execution = false;",
        *view_statements(sf),
        "RESET gpu_execution;",
    ]


def _proc_bytes(path: str, key: str) -> int | None:
    try:
        text = pathlib.Path(path).read_text()
    except OSError:
        return None
    match = re.search(rf"^{key}:\s+(\d+) kB", text, re.M)
    return int(match.group(1)) * 1024 if match else None


def dir_bytes(path: pathlib.Path) -> int:
    total = 0
    for root, _, files in os.walk(path):
        for name in files:
            try:
                total += os.lstat(os.path.join(root, name)).st_size
            except OSError:
                pass
    return total


def run(
    build: str,
    script: str,
    directory: pathlib.Path,
    *,
    config: pathlib.Path | None,
    log_dir: pathlib.Path | None = None,
    log_level: str = "info",
    test_options: bool = False,
    sirius_disabled: bool = False,
    bail: bool = True,
    timeout_s: float | None = None,
    stall_s: float | None = None,
    progress: Callable[[], int] | None = None,
    progress_total: int | None = None,
    watch: pathlib.Path | None = None,
    poll_s: float = 10.0,
) -> CliRun:
    """Runs @p script through the build's DuckDB CLI in @p directory and waits for it.

    @p progress counts finished units of work (for example query runs). The process is killed as
    stalled when the count does not advance for @p stall_s seconds; it is logged every 5 minutes.
    """
    directory.mkdir(parents=True, exist_ok=True)
    script_path = directory / "script.sql"
    script_path.write_text(script)
    intended: dict[str, str] = {}
    if config is not None:
        intended["SIRIUS_CONFIG_FILE"] = str(config)
    if sirius_disabled:
        intended["SIRIUS_DISABLE"] = "1"
    if test_options:
        intended["SIRIUS_ENABLE_TEST_OPTIONS"] = "1"
    if log_dir is not None:
        log_dir.mkdir(parents=True, exist_ok=True)
        intended["SIRIUS_LOG_DIR"] = str(log_dir)
        intended["SIRIUS_LOG_LEVEL"] = log_level
    env = common.clean_env(**intended)
    # -no-agent keeps the plain output format when an AI agent environment is detected.
    command = [str(common.duckdb_cli(build)), "-no-agent", "-unsigned"]
    if bail:
        command.append("-bail")
    stdout_path, stderr_path = directory / "stdout.txt", directory / "stderr.txt"
    result = CliRun(None, "", "")
    started = time.monotonic()
    with (
        script_path.open() as stdin,
        stdout_path.open("w") as stdout,
        stderr_path.open("w") as stderr,
    ):
        process = subprocess.Popen(
            command, stdin=stdin, stdout=stdout, stderr=stderr, env=env, cwd=directory
        )
        status_file = f"/proc/{process.pid}/status"
        last_count, last_change, last_report, polls = -1, started, started, 0
        while process.poll() is None:
            time.sleep(poll_s)
            polls += 1
            now = time.monotonic()
            rss = _proc_bytes(status_file, "VmHWM")
            if rss is not None:
                result.host_rss_peak_bytes = max(result.host_rss_peak_bytes or 0, rss)
            available = _proc_bytes("/proc/meminfo", "MemAvailable")
            if available is not None:
                result.mem_available_min_bytes = min(
                    result.mem_available_min_bytes or available, available
                )
            if watch is not None and polls % 6 == 0:
                result.disk_peak_bytes = max(
                    result.disk_peak_bytes or 0, dir_bytes(watch)
                )
            if progress is not None:
                count = progress()
                if count != last_count:
                    last_count, last_change = count, now
                if now - last_report >= 300:
                    total = f"/{progress_total}" if progress_total else ""
                    common.log(
                        f"{directory.name}: {count}{total} done, {(now - started) / 60:.0f} min"
                    )
                    last_report = now
            if timeout_s is not None and now - started > timeout_s:
                result.timed_out = True
            elif stall_s is not None and now - last_change > stall_s:
                result.stalled = True
            if result.timed_out or result.stalled:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=60)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                break
        result.returncode = process.returncode
    result.wall_s = time.monotonic() - started
    if watch is not None:
        result.disk_peak_bytes = max(result.disk_peak_bytes or 0, dir_bytes(watch))
    result.stdout = stdout_path.read_text(errors="replace")
    result.stderr = stderr_path.read_text(errors="replace")
    if result.timed_out:
        result.stderr += f"\ntimed out after {timeout_s} s"
    if result.stalled:
        result.stderr += f"\nkilled: no progress for {stall_s} s"
    return result
