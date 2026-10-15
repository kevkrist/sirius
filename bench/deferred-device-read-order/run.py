#!/usr/bin/env python3
"""Run paired policy measurements, retain raw samples/traces, and summarize them."""
import argparse
import csv
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import time


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path("build/release"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--iterations", type=int, default=10)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--gate-us", type=int, default=20000)
    parser.add_argument("--service-us", type=int, nargs="+", default=[0, 2000])
    args = parser.parse_args()
    if args.rounds < 1 or args.iterations < 1 or args.warmups < 0:
        parser.error("rounds/iterations must be positive and warmups nonnegative")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    build = args.build.resolve()
    binaries = {
        policy: build / "test/io" / ("deferred_order_" + policy)
        for policy in ["siblings_first", "own_first"]
    }
    root = Path(__file__).resolve().parents[2]
    manifest = json.loads((build / "deferred-order/manifest.json").read_text())
    source = root / "src/io/deferred_device_batch.cpp"
    if sha256(source) != manifest["baseline_sha256"]:
        raise RuntimeError(
            "Sources changed after benchmark generation; rebuild both targets"
        )
    shutil.copytree(build / "deferred-order", output / "generated")
    metadata = {
        "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "platform": platform.platform(),
        "cpu": platform.processor(),
        "cuda_visible_devices": os.environ.get("CUDA_VISIBLE_DEVICES", "unset"),
        "arguments": {
            key: str(value) if isinstance(value, Path) else value
            for key, value in vars(args).items()
        },
        "binaries": {
            key: {"path": str(value), "sha256": sha256(value)}
            for key, value in binaries.items()
        },
        "source_sha256": {
            str(path.relative_to(root)): sha256(path)
            for path in [
                source,
                root / "test/io/deferred_device_order_benchmark.cpp",
                root / "src/io/sirius_datasource.cpp",
                root / "src/io/cache/prefetching_cache.cpp",
            ]
        },
        "backend": "bounded pread workers; pinned staging; actual CUDA H2D",
        "file_cache": "page-cache warm: deterministic files freshly written before samples",
        "cache": "new Sirius cache each iteration; only selected source has active prefetch",
        "queue_capacity": 8,
        "slices_per_source": 4,
        "completion_observation": "stream host callback after each cudf memcpy batch",
    }
    metadata["gpu"] = subprocess.check_output(
        ["nvidia-smi", "--query-gpu=name,driver_version,uuid", "--format=csv,noheader"],
        text=True,
    )
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    samples = []
    commands = []
    for scenario, workers, size, service in itertools.product(
        ["none", "blocked_a", "blocked_b"], [1, 2], [65536, 4194304], args.service_us
    ):
        for round_index in range(args.rounds):
            policies = list(binaries)
            if round_index % 2:
                policies.reverse()
            for policy in policies:
                label = (
                    f"{scenario}-w{workers}-b{size}-s{service}-r{round_index}-{policy}"
                )
                directory = output / label
                command = [
                    str(binaries[policy]),
                    str(directory),
                    scenario,
                    str(workers),
                    str(size),
                    str(args.iterations),
                    str(args.warmups),
                    str(args.gate_us),
                    str(service),
                ]
                commands.append(command)
                (output / "commands.json").write_text(
                    json.dumps(commands, indent=2) + "\n"
                )
                print(label, flush=True)
                with (output / (label + ".log")).open("w") as log:
                    subprocess.run(
                        command,
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        check=True,
                        timeout=max(90, 25 * (args.iterations + args.warmups)),
                    )
                with (directory / "samples.csv").open() as source_file:
                    for row in csv.DictReader(source_file):
                        samples.append(
                            dict(
                                scenario=scenario,
                                workers=workers,
                                bytes=size,
                                service_us=service,
                                policy=policy,
                                round=round_index,
                                **row,
                            )
                        )
                # The fixture is regenerated before every process and is not evidence.
                for name in ["A", "B", "C"]:
                    (directory / name).unlink()
    with (output / "samples.csv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=samples[0].keys())
        writer.writeheader()
        writer.writerows(samples)
    grouping = ["scenario", "workers", "bytes", "service_us", "policy"]
    metrics = ["a_ready_us", "a_get_us", "all_get_us", "b_ready_us", "c_ready_us"]
    groups = {}
    for row in samples:
        key = tuple(row[field] for field in grouping)
        groups.setdefault(key, []).append(row)
    summary = []
    for key, rows in groups.items():
        row = dict(zip(grouping, key))
        row["samples"] = len(rows)
        for metric in metrics:
            values = sorted(float(sample[metric]) for sample in rows)
            row[metric + "_median"] = statistics.median(values)
            row[metric + "_p95"] = values[math.ceil(len(values) * 0.95) - 1]
        summary.append(row)
    with (output / "summary.csv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=summary[0].keys())
        writer.writeheader()
        writer.writerows(summary)
    print(
        f"Wrote {len(samples)} verified samples and {len(summary)} summary rows to {output}"
    )


if __name__ == "__main__":
    main()
