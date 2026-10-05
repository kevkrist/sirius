#!/usr/bin/env python3
"""Compare fused dense count-join with eager aggregation on identical pinned inputs.

Run through pixi. The parent serializes GPU use; separate child processes prepare
data, compute CPU references, confirm paths with logging, and measure without
logging. No engine changes or rebuild are needed.
"""

import argparse
import csv
import fcntl
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
VARIANTS = ("dense", "eager")
INNER = """SELECT c.c_custkey, count(o.o_orderkey) AS c_count
FROM customer c LEFT JOIN orders o ON c.c_custkey = o.o_custkey
GROUP BY c.c_custkey"""
QUERIES = {
    "core": f"SELECT count(*) AS groups, sum(c_count) AS total_count FROM ({INNER}) counts",
    "q13": f"""SELECT c_count, count(*) AS custdist FROM ({INNER}) counts
GROUP BY c_count ORDER BY custdist DESC, c_count DESC""",
}


def literal(value):
    return "'" + str(value).replace("'", "''") + "'"


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(8 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_output(argv):
    result = subprocess.run(argv, text=True, capture_output=True, check=False)
    return result.stdout.strip() or result.stderr.strip()


def parquet_glob(directory, table):
    if (directory / f"{table}.parquet").is_file():
        return str(directory / f"{table}.parquet")
    if (directory / table).is_dir():
        return str(directory / table / "**" / "*.parquet")
    raise ValueError(f"Missing {table}.parquet or {table}/ below {directory}")


def worker(job):
    import duckdb

    data = Path(job["data"])
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute("SET threads = 8")
    con.execute("SET memory_limit = '16GB'")
    if job["stage"] == "prepare":
        data.mkdir(parents=True, exist_ok=False)
        if job.get("tpch_dir") or job.get("tpch_db"):
            if job.get("tpch_db"):
                con.execute(f"ATTACH {literal(job['tpch_db'])} AS tpch (READ_ONLY)")
                customer_source, orders_source = "tpch.customer", "tpch.orders"
            else:
                source = Path(job["tpch_dir"])
                customer_source = (
                    f"read_parquet({literal(parquet_glob(source, 'customer'))})"
                )
                orders_source = (
                    f"read_parquet({literal(parquet_glob(source, 'orders'))})"
                )
            customer = f"SELECT c_custkey FROM {customer_source}"
            orders = f"""SELECT o_custkey, o_orderkey FROM {orders_source}
WHERE o_comment NOT LIKE '%special%requests%'"""
            kind = "TPC-H q13 projected inputs; comment filter applied before timing"
        else:
            customers = round(150_000 * job["sf"])
            orders_count = round(1_500_000 * job["sf"])
            matched_keys = max(1, customers * 4 // 5)
            customer = (
                f"SELECT (range + 1)::INTEGER AS c_custkey FROM range({customers})"
            )
            # 20% of customers have no matches; NULL join keys and NULL COUNT
            # inputs exercise the outer-join semantics while retaining dense keys.
            orders = f"""SELECT
CASE WHEN range % 101 = 0 THEN NULL ELSE (range % {matched_keys} + 1)::INTEGER END AS o_custkey,
CASE WHEN range % 17 = 0 THEN NULL ELSE (range + 1)::BIGINT END AS o_orderkey
FROM range({orders_count})"""
            kind = "synthetic SF-sized rows, not reference TPC-H data"
        for name, sql in (("customer", customer), ("orders", orders)):
            con.execute(
                f"COPY ({sql}) TO {literal(data / (name + '.parquet'))} "
                "(FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 122880)"
            )
        counts = {}
        for name in ("customer", "orders"):
            counts[name] = con.execute(
                f"SELECT count(*) FROM read_parquet({literal(data / (name + '.parquet'))})"
            ).fetchone()[0]
        save_json(
            Path(job["result"]),
            {
                "kind": kind,
                "rows": counts,
                "sf": job["sf"],
                "source": job.get("tpch_db")
                or job.get("tpch_dir")
                or "synthetic range generator",
            },
        )
        con.close()
        return

    for name in ("customer", "orders"):
        con.execute(
            f"CREATE VIEW {name} AS SELECT * FROM read_parquet({literal(data / (name + '.parquet'))})"
        )
    if job["stage"] == "cpu":
        result = {
            name: con.execute(QUERIES[name]).fetchall() for name in job["workloads"]
        }
        save_json(Path(job["result"]), result)
        con.close()
        return

    con.execute(f"LOAD {literal(job['extension'])}")
    con.execute("SET enable_duckdb_fallback = false")
    con.execute("SET gpu_execution = true")
    con.execute("SET eager_agg_pushdown_force = false")
    # Pin once and share the exact same cached columns between both variants.
    for name, columns in (
        ("customer", "['c_custkey']"),
        ("orders", "['o_custkey', 'o_orderkey']"),
    ):
        con.execute(
            f"CALL pin_table({literal(data / (name + '.parquet'))}, "
            f"name={literal(name)}, tier='gpu', cols={columns}, compression=false)"
        )

    reference = json.loads(Path(job["reference"]).read_text())

    def execute(name, variant):
        con.execute(
            f"SET enable_eager_agg_pushdown = {str(variant == 'eager').lower()}"
        )
        con.execute(f"SET enable_dense_count_join = {str(variant == 'dense').lower()}")
        start = time.perf_counter_ns()
        rows = con.execute(QUERIES[name]).fetchall()
        elapsed_ms = (time.perf_counter_ns() - start) / 1e6
        if [list(row) for row in rows] != reference[name]:
            raise RuntimeError(f"{name}/{variant}: result differs from CPU reference")
        return elapsed_ms

    timings = []
    for name in job["workloads"]:
        if job["stage"] == "validate":
            execute(name, job["variant"])
            continue
        for variant in VARIANTS:
            timings.append((name, variant, "first", 0, execute(name, variant)))
        for _ in range(job["warmups"]):
            for variant in VARIANTS:
                execute(name, variant)
        # AB, BA, AB, BA ...: the A/B pairs form ABBA blocks. Exactly one
        # sample per algorithm in each pair; settings and result checks excluded.
        for pair in range(job["pairs"]):
            order = VARIANTS if pair % 2 == 0 else tuple(reversed(VARIANTS))
            for variant in order:
                elapsed = execute(name, variant)
                timings.append((name, variant, "hot", pair + 1, elapsed))
    for name in ("orders", "customer"):
        con.execute(f"CALL unpin_table({literal(name)})")
    con.close()
    save_json(Path(job["result"]), {"timings": timings, "duckdb": duckdb.__version__})


def child(job, run, config):
    name = job["stage"] + ("_" + job["variant"] if job.get("variant") else "")
    job["result"] = str(run / f"{name}.json")
    job_file = run / f"{name}_job.json"
    save_json(job_file, job)
    env = dict(os.environ)
    env.update(
        SIRIUS_CONFIG_FILE=str(config),
        SIRIUS_ENABLE_TEST_OPTIONS="1",
        SIRIUS_DISABLE="1" if name in ("prepare", "cpu") else "0",
        SIRIUS_LOG_BACKEND="spdlog",
        SIRIUS_LOG_LEVEL="info" if job["stage"] == "validate" else "off",
        SIRIUS_LOG_DIR=str(run / name / "logs"),
    )
    (run / name / "logs").mkdir(parents=True)
    print(f"Running {name} ...", flush=True)
    with (run / f"{name}.stdout").open("w") as stdout:
        result = subprocess.run(
            [sys.executable, str(Path(__file__).resolve()), "--worker", str(job_file)],
            cwd=ROOT,
            env=env,
            stdout=stdout,
            stderr=subprocess.STDOUT,
            timeout=job["timeout"],
            check=False,
        )
    if result.returncode:
        raise RuntimeError(
            f"{name} exited {result.returncode}; see {run / (name + '.stdout')}"
        )
    return json.loads(Path(job["result"]).read_text())


def confirm_path(run, variant, count, allow_sparse):
    log_files = sorted((run / f"validate_{variant}" / "logs").glob("*.log"))
    text = "\n".join(path.read_text(errors="replace") for path in log_files)
    evidence = {
        "eager_rewrites": text.count("Eager aggregation pushdown applied"),
        "dense_fusions": text.count("Fusing COUNT-join into DENSE_COUNT_JOIN"),
        "dense_tasks": text.count("dense strategy"),
        "sparse_tasks": text.count("sparse strategy"),
        "pinned_customer_scans": text.count("assigned pinned entry 'customer'"),
        "pinned_orders_scans": text.count("assigned pinned entry 'orders'"),
        "logs": [str(path) for path in log_files],
    }
    if "retrying with the original plan" in text or "fallback to DuckDB" in text:
        raise RuntimeError(
            f"{variant}: planning retry or CPU fallback in validation log"
        )
    if variant == "dense":
        valid = evidence["dense_fusions"] == count and evidence["eager_rewrites"] == 0
        valid = valid and evidence["dense_tasks"] + evidence["sparse_tasks"] > 0
        if not allow_sparse:
            valid = (
                valid and evidence["sparse_tasks"] == 0 and evidence["dense_tasks"] > 0
            )
    else:
        valid = evidence["eager_rewrites"] == count and evidence["dense_fusions"] == 0
    valid = (
        valid
        and evidence["pinned_customer_scans"] == count
        and evidence["pinned_orders_scans"] == count
    )
    if not valid:
        save_json(run / f"{variant}_path_failure.json", evidence)
        raise RuntimeError(f"{variant}: intended algorithm not confirmed: {evidence}")
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--sf", type=float, default=50, help="synthetic SF row counts (default: 50)"
    )
    parser.add_argument(
        "--tpch-dir",
        type=Path,
        help="reference TPC-H parquet directory to project/filter",
    )
    parser.add_argument(
        "--tpch-db",
        type=Path,
        help="TPC-H DuckDB database, opened read-only during preparation",
    )
    parser.add_argument(
        "--prepared-data", type=Path, help="reuse projected data from an earlier run"
    )
    parser.add_argument("--output", type=Path, required=True, help="new run directory")
    parser.add_argument(
        "--config", type=Path, help="base Sirius YAML; telemetry will be disabled"
    )
    parser.add_argument(
        "--extension",
        type=Path,
        default=ROOT / "build/release/extension/sirius/sirius.duckdb_extension",
    )
    parser.add_argument("--workload", choices=("core", "q13", "both"), default="both")
    parser.add_argument(
        "--pairs",
        type=int,
        default=10,
        help="hot samples per algorithm, even for balanced ABBA",
    )
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument(
        "--allow-sparse",
        action="store_true",
        help="compare the fused operator even if its sparse strategy runs",
    )
    parser.add_argument(
        "--timeout", type=int, default=1800, help="seconds per child process"
    )
    parser.add_argument("--worker", type=Path, help=argparse.SUPPRESS)
    if "--worker" in sys.argv:
        index = sys.argv.index("--worker")
        worker(json.loads(Path(sys.argv[index + 1]).read_text()))
        return
    args = parser.parse_args()
    if args.sf <= 0 or args.sf * 150_000 > 2_147_483_647:
        parser.error("SF must produce a positive customer count fitting INTEGER")
    if args.pairs < 4 or args.pairs % 2 or args.warmups < 1:
        parser.error("Use an even --pairs >= 4 and --warmups >= 1")
    if (
        sum(bool(value) for value in (args.tpch_dir, args.tpch_db, args.prepared_data))
        > 1
    ):
        parser.error("Choose one of --tpch-dir, --tpch-db or --prepared-data")
    run = args.output.resolve()
    run.mkdir(parents=True, exist_ok=False)
    # Use the same lock naming convention as the existing GPU benchmark harness.
    gpu_uuid = command_output(
        ["nvidia-smi", "--query-gpu=uuid", "--format=csv,noheader"]
    ).splitlines()[0]
    if not gpu_uuid.startswith("GPU-"):
        raise RuntimeError(f"Cannot identify an accessible GPU: {gpu_uuid}")
    lock_path = Path("/tmp") / f"sirius-benchmark-{gpu_uuid}.lock"
    # Linux permits flock(LOCK_EX) on a read-only descriptor. Reuse a lock
    # created by another user without requiring write access to its contents.
    try:
        descriptor = os.open(lock_path, os.O_RDONLY)
    except FileNotFoundError:
        descriptor = os.open(lock_path, os.O_RDONLY | os.O_CREAT, 0o644)
    with os.fdopen(descriptor, "r") as lock:
        print(f"Waiting for GPU lock {lock_path} ...", flush=True)
        fcntl.flock(lock, fcntl.LOCK_EX)
        run_locked(args, run, gpu_uuid)


def run_locked(args, run, gpu_uuid):
    import yaml

    if args.config:
        config_doc = yaml.safe_load(args.config.read_text())
    else:
        config_doc = {
            "sirius": {
                "topology": {"num_gpus": 1},
                "memory": {
                    "gpu": {"usage_limit_fraction": 0.5},
                    "host": {"capacity_bytes": 4 << 30},
                    "disk": {
                        "capacity_bytes": 8 << 30,
                        "downgrade_root_dirs": str(run / "spill"),
                    },
                },
                "executor": {
                    "pipeline": {"num_threads": 4},
                    "downgrade": {"num_threads": 1},
                },
            }
        }
    config_doc["sirius"].setdefault("telemetry", {})["enable_quent"] = False
    config = run / "sirius_config.yaml"
    config.write_text(yaml.safe_dump(config_doc, sort_keys=False))
    workloads = list(QUERIES) if args.workload == "both" else [args.workload]
    data = args.prepared_data.resolve() if args.prepared_data else run / "data"
    common = {
        "data": str(data),
        "extension": str(args.extension.resolve()),
        "workloads": workloads,
        "timeout": args.timeout,
        "warmups": args.warmups,
        "pairs": args.pairs,
        "reference": str(run / "cpu.json"),
    }
    manifest = {
        "commit": command_output(["git", "rev-parse", "HEAD"]),
        "dirty": command_output(["git", "status", "--short"]),
        "extension": str(args.extension.resolve()),
        "extension_sha256": sha256(args.extension),
        "script_sha256": sha256(Path(__file__)),
        "config_sha256": sha256(config),
        "gpu_uuid": gpu_uuid,
        "gpu": command_output(
            [
                "nvidia-smi",
                "--query-gpu=name,driver_version,temperature.gpu",
                "--format=csv",
            ]
        ),
        "command": sys.argv,
        "python": sys.version,
        "protocol": "GPU-pinned inputs; 1 first sample, warmups, alternating AB/BA pairs; logging/telemetry off for timings",
        "timing": "wall milliseconds including planning, GPU execution and fetching compact results; excludes settings and correctness comparison",
        "sf": args.sf,
        "workloads": workloads,
    }
    save_json(run / "manifest.json", manifest)
    for name in workloads:
        (run / f"{name}.sql").write_text(QUERIES[name] + ";\n")
    if not args.prepared_data:
        child(
            {
                **common,
                "stage": "prepare",
                "sf": args.sf,
                "tpch_dir": str(args.tpch_dir.resolve()) if args.tpch_dir else None,
                "tpch_db": str(args.tpch_db.resolve()) if args.tpch_db else None,
            },
            run,
            config,
        )
    else:
        provenance = data.parent / "prepare.json"
        save_json(
            run / "prepare.json",
            (
                json.loads(provenance.read_text())
                if provenance.exists()
                else {
                    "kind": "reused projected data; original provenance unavailable",
                    "source": str(data),
                }
            ),
        )
    manifest["data"] = {
        name: {
            "path": str(data / f"{name}.parquet"),
            "sha256": sha256(data / f"{name}.parquet"),
        }
        for name in ("customer", "orders")
    }
    save_json(run / "manifest.json", manifest)
    child({**common, "stage": "cpu"}, run, config)
    evidence = {}
    for variant in VARIANTS:
        child({**common, "stage": "validate", "variant": variant}, run, config)
        evidence[variant] = confirm_path(
            run, variant, len(workloads), args.allow_sparse
        )
    save_json(run / "path_evidence.json", evidence)
    result = child({**common, "stage": "timing"}, run, config)
    with (run / "timings.csv").open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(("workload", "algorithm", "regime", "pair", "wall_ms"))
        writer.writerows(result["timings"])
    provenance = json.loads((run / "prepare.json").read_text())
    lines = [
        "# Count-join comparison",
        "",
        f"Commit: `{manifest['commit']}`.",
        f"Hardware: {manifest['gpu'].splitlines()[-1]}. DuckDB: {result['duckdb']}.",
        f"Dataset: {provenance['kind']}. Source: `{provenance['source']}`. Rows: {provenance.get('rows', 'not recorded')}.",
        f"Samples: {args.pairs} hot samples per algorithm per workload, {args.warmups} warmups each, balanced ABBA order.",
        "GPU-pinned projected inputs. All results match CPU exactly. Both paths confirmed in separate logged runs.",
        "Timing runs have logging and telemetry disabled. First samples are excluded from hot summaries.",
        "",
        "| Workload | Fused median ms | Eager median ms | Eager / fused | Fused CV | Eager CV |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for name in workloads:
        samples = {
            variant: [
                row[4] for row in result["timings"] if row[:3] == [name, variant, "hot"]
            ]
            for variant in VARIANTS
        }
        medians = {v: statistics.median(samples[v]) for v in VARIANTS}
        cvs = {
            v: statistics.stdev(samples[v]) / statistics.mean(samples[v]) * 100
            for v in VARIANTS
        }
        lines.append(
            f"| {name} | {medians['dense']:.3f} | {medians['eager']:.3f} | "
            f"{medians['eager'] / medians['dense']:.3f}x | {cvs['dense']:.1f}% | {cvs['eager']:.1f}% |"
        )
    lines += [
        "",
        "`core` adds a two-scalar checksum above the matching COUNT-over-LEFT-JOIN fragment, avoiding transfer of millions of groups.",
        "`q13` adds the customer-count distribution and sort from q13. The comment filter, when using reference TPC-H data, is applied during preparation.",
        "This compares the two engine execution paths, including partitioning/scheduling, rather than isolated kernels.",
        "CV is sample standard deviation divided by mean. See prepare.json for provenance and row counts, and path_evidence.json for dense/sparse strategy counts.",
        "Raw samples: timings.csv. Algorithm evidence: path_evidence.json and validate_*/logs/. Configuration and hashes: manifest.json.",
        "",
    ]
    (run / "REPORT.md").write_text("\n".join(lines))
    print("\n".join(lines), flush=True)


if __name__ == "__main__":
    main()
