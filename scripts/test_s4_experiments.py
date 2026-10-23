#!/usr/bin/env python3
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
"""CPU-only checks; native executables and GPU introspection are never invoked."""

from __future__ import annotations

from collections import defaultdict
import contextlib
import io
import json
import math
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import analyze_s4_experiments as analyze
import run_s4_experiments as run


def arguments(phase="confirm", *extra):
    return run.parser().parse_args(
        [
            phase,
            "--build-manifest",
            "unused.json",
            "--gpus",
            "0,1,2,3",
            "--output",
            "unused-results",
            "--stages",
            "builder",
            *extra,
        ]
    )


def native_sample(workload, job, manifest, build, repetition=0, warmup=False):
    record = {field: 0 for field in run.METRICS}
    record.update(
        schema_version=1,
        run_id=manifest["run_id"],
        build_id=build["build_id"],
        sample_id=f"{job['sample_prefix']}:{'warmup' if warmup else 'sample'}:{repetition}",
        process_cluster=job["process_cluster"],
        pair=job["pair"],
        order=job["order"],
        seed=job["seed"],
        compiled_schedule=job["schedule"],
        devices=manifest["devices"],
        root=workload.get("root"),
        mode=workload["mode"],
        warmup=warmup,
        repetition=repetition,
        profile=manifest["profile"],
        cap_bytes=manifest["cap_bytes"],
        coverage=manifest["coverage"],
        status="ok",
        skip_reason=None,
        oracle_outcome="passed",
        oracle_passed=True,
        gpu_intercepted=True,
        host_elapsed_ns=100,
        query_elapsed_ns=1000,
        N=len(manifest["devices"]),
        K=workload.get("K"),
        B=workload.get("B"),
        C=1024,
        ready_replica_bytes=len(manifest["devices"])
        * workload.get("K", 1)
        * workload.get("B", 1),
        actual_transport=(
            "host" if workload.get("transport") == "force_host" else "direct"
        ),
    )
    return record


class WorkloadTests(unittest.TestCase):
    def test_screen_and_heldout_are_disjoint_and_bounded(self):
        screen = run.workload_set("screen", "default256", [3, 0, 2, 1])
        heldout = run.workload_set("confirm", "default256", [3, 0, 2, 1])
        self.assertFalse(
            {(w["K"], w["B"]) for w in screen} & {(w["K"], w["B"]) for w in heldout}
        )
        self.assertEqual(
            {w["root"] for w in screen if w["B"] == 64 * run.MIB + 256 and w["K"] == 1},
            {0, 1, 2, 3},
        )
        for workload in screen + heldout:
            self.assertEqual(workload["B"] % 256, 0)
            self.assertLessEqual(workload["K"] * workload["B"], 256 * run.MIB)
        self.assertTrue(all(w["held_out"] for w in heldout))

    def test_raised_profile_does_not_replace_default_cases(self):
        raised = run.workload_set("screen", "raised2g", [0, 1, 2, 3])
        self.assertTrue(all(w["B"] > 256 * run.MIB for w in raised))
        self.assertTrue(all(w["B"] <= 2 * 1024**3 for w in raised))

    def test_all_ordered_pair_directions_and_roots(self):
        workloads = run.transport_workloads([0, 1, 2, 3])
        for issuer in ("destination_read", "source_write"):
            pairs = {
                (w["source"], w["destination"])
                for w in workloads
                if w["pattern"] == "pair" and w["issuer"] == issuer
            }
            self.assertEqual(len(pairs), 12)
            self.assertFalse(any(a == b for a, b in pairs))
        self.assertEqual(len(workloads), 42)

    def test_confirmation_pairing_and_baseline_controls(self):
        args = arguments()
        workloads = run.workload_set("confirm", "default256", [0, 1, 2, 3])
        jobs = run.execution_plan(workloads, args)
        self.assertEqual(jobs, run.execution_plan(workloads, args))
        groups = defaultdict(list)
        for job in jobs:
            groups[job["process_cluster"]].append(job)
        self.assertEqual(len(groups), len(workloads) * 5 * 2)
        for arms in groups.values():
            self.assertEqual(len(arms), 2)
            self.assertEqual({a["arm"] for a in arms}, {"baseline", "candidate"})
            self.assertEqual(arms[0]["seed"], arms[1]["seed"])
            self.assertEqual(arms[0]["workload"], arms[1]["workload"])
            self.assertEqual(arms[0]["repetitions"], 6)
            if arms[0]["comparison"] == "baseline_baseline":
                self.assertEqual({a["schedule"] for a in arms}, {"current_direct"})
        self.assertEqual(
            {j["arm"] for j in jobs if j["order"] == 0}, {"baseline", "candidate"}
        )


class StatisticsTests(unittest.TestCase):
    def test_thirty_pairs_are_five_clusters(self):
        values = {str(i): [math.log(0.80 + i * 0.05)] * 6 for i in range(5)}
        result = analyze.cluster_interval(values)
        self.assertEqual(result["independent_process_pairs"], 5)
        self.assertEqual(result["measured_pairs"], 30)
        duplicated = analyze.cluster_interval({k: v * 100 for k, v in values.items()})
        self.assertEqual(result["ratio_ci95"], duplicated["ratio_ci95"])

    def test_three_pair_screen_has_no_interval_or_winner(self):
        result = analyze.cluster_interval({"only_process_pair": [math.log(0.5)] * 3})
        self.assertIsNone(result["ratio_ci95"])
        self.assertEqual(
            analyze.assess(
                {"statistics": result}, None, "screen", "host_elapsed_ns", True
            ),
            "exploratory_screen_no_selection",
        )

    def test_baseline_drift_blocks_apparent_improvement(self):
        improved = {
            "statistics": analyze.cluster_interval(
                {str(i): [math.log(0.8)] * 6 for i in range(5)}
            ),
            "complete_comparison": True,
        }
        drifting = {
            "statistics": analyze.cluster_interval(
                {str(i): [math.log(0.9)] * 6 for i in range(5)}
            ),
            "complete_comparison": True,
        }
        self.assertEqual(
            analyze.assess(improved, drifting, "confirm", "host_elapsed_ns", True),
            "inconclusive_baseline_control_drift",
        )

    def test_skipped_sql_is_not_publication_improvement(self):
        left = {"mode": "sql", "published_bitmap_bytes": 0}
        self.assertFalse(analyze.equivalent_publication(left, left))


class ProtocolTests(unittest.TestCase):
    def test_duplicate_json_keys_are_rejected(self):
        with self.assertRaises(ValueError):
            run.decode_json('{"schema_version":1,"schema_version":2}')

    def test_fixture_exposes_existing_internal_test_options(self):
        with patch.dict(run.os.environ, {"SIRIUS_DISABLE": "1"}):
            env = run.fixture_environment(
                {"library_search_paths": ["/frozen"]}, Path("/config.yaml")
            )
        self.assertEqual(env["SIRIUS_ENABLE_TEST_OPTIONS"], "1")
        self.assertEqual(env["SIRIUS_CONFIG_FILE"], "/config.yaml")
        self.assertNotIn("SIRIUS_DISABLE", env)

    def setUp(self):
        self.workload = run.workload_set("screen", "default256", [0, 1])[0]
        self.job = run.execution_plan(
            [self.workload], arguments("screen", "--warmups", "0", "--pairs", "1")
        )[0]
        self.job["sample_prefix"] = "r:p1"
        self.manifest = {
            "run_id": "r",
            "devices": [0, 1],
            "profile": "default256",
            "cap_bytes": 256 * run.MIB,
            "coverage": 0.9,
        }
        self.build = {"build_id": "immutable"}
        self.sample = native_sample(self.workload, self.job, self.manifest, self.build)

    def validate(self, record):
        run.validate_native(
            [record], self.build, self.workload, self.job, self.manifest
        )

    def test_valid_record(self):
        self.validate(self.sample)

    def test_wrong_schedule_missing_bytes_false_oracle_and_nonfinite_rejected(self):
        for key, bad in (
            ("compiled_schedule", "not_the_build"),
            ("ready_replica_bytes", 0),
            ("oracle_passed", False),
            ("host_elapsed_ns", float("nan")),
            ("seed", 0),
            ("cap_bytes", 2**31),
            ("profile", "raised2g"),
        ):
            record = dict(self.sample, **{key: bad})
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.validate(record)
        record = dict(self.sample)
        del record["host_staging_physical_bytes"]
        with self.assertRaises(ValueError):
            self.validate(record)

    def test_duplicate_repetitions_rejected(self):
        self.job["repetitions"] = 2
        with self.assertRaises(ValueError):
            run.validate_native(
                [self.sample, self.sample],
                self.build,
                self.workload,
                self.job,
                self.manifest,
            )

    def test_loader_aslr_is_not_binary_identity(self):
        library = Path("/tmp/libsirius.so")
        first = {
            "returncode": 0,
            "stdout": "libsirius.so => /tmp/libsirius.so (0xabc)\n",
            "stderr": "",
        }
        second = dict(first, stdout=first["stdout"].replace("0xabc", "0xdef"))
        with patch.object(run, "capture", side_effect=[first, second]):
            one = run.loader_identity(
                Path("/tmp/ext.so"), library, {}, needs_sirius=True
            )
            two = run.loader_identity(
                Path("/tmp/ext.so"), library, {}, needs_sirius=True
            )
        self.assertEqual(one["stdout"], two["stdout"])

    def test_missing_dataset_is_explicit(self):
        args = arguments("screen", "--queries", "4", "--scale", "unknown/tiny")
        with tempfile.TemporaryDirectory() as tmp:
            workloads, dataset = run.prepare_sql(args, Path(tmp))
        self.assertFalse(workloads[0]["available"])
        self.assertEqual(dataset["availability"], "unavailable")
        self.assertEqual(dataset["scale"], "unknown/tiny")

    def test_parquet_sql_quotes_paths_and_pins_matching_files(self):
        with tempfile.TemporaryDirectory(prefix="s4-'data-") as tmp:
            directory = Path(tmp)
            for table in ("orders", "lineitem"):
                (directory / f"{table}.parquet").write_bytes(b"CPU test metadata only")
            output = directory / "output"
            output.mkdir()
            args = arguments(
                "screen",
                "--queries",
                "4",
                "--data-path",
                str(directory),
                "--hash-dataset",
                "--scale",
                "unknown/tiny",
            )
            workloads, dataset = run.prepare_sql(args, output)
            sql = Path(workloads[0]["prepare_file"]).read_text()
            self.assertIn("''data-", sql)
            self.assertEqual(sql.count("CALL pin_table"), 2)
            self.assertEqual(len(dataset["files"]), 2)
            self.assertTrue(all("sha256" in f for f in dataset["files"]))

    def test_bundle_roundtrip_and_missing_process_rejection(self):
        args = arguments("screen", "--warmups", "0", "--pairs", "1")
        jobs = run.execution_plan([self.workload], args)
        manifest = {
            **self.manifest,
            "schema_version": 1,
            "phase": "screen",
            "scale": "unknown",
            "four_gpu_handoff": False,
            "stage_a_results": None,
            "dataset": {"availability": "not_requested"},
            "builds": {name: self.build for name in run.SCHEDULES},
            "workloads": [self.workload],
            "jobs": jobs,
        }
        records = []
        for index, job in enumerate(jobs):
            job.update(sample_prefix=f"r:p{index}", process_id=f"p{index}")
            sample = native_sample(self.workload, job, manifest, self.build)
            sample["harness"] = {
                **job,
                "profile": manifest["profile"],
                "phase": "screen",
                "held_out": False,
            }
            records.append(sample)
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            run.write_json(directory / "experiment.json", manifest)
            samples = directory / "samples.jsonl"
            samples.write_text("".join(json.dumps(r) + "\n" for r in records))
            run.write_json(
                directory / "completion.json",
                {
                    "status": "complete",
                    "processes": len(jobs),
                    "samples_sha256": run.file_hash(samples),
                    "experiment_sha256": run.file_hash(directory / "experiment.json"),
                },
            )
            result = analyze.analyze(directory)
            self.assertEqual(
                result["outcomes"][0]["assessment"],
                (
                    "exploratory_screen_no_selection"
                    if result["outcomes"][0]["comparison"] == "baseline_candidate"
                    else "noise_control"
                ),
            )
            samples.write_text(samples.read_text().splitlines()[0] + "\n")
            with self.assertRaises(ValueError):
                analyze.validated_bundle(directory)

    def test_manifest_tampering_rejected_before_subprocess(self):
        with self.assertRaises(ValueError):
            run.verify_build({"schema_version": 1, "build_id": "forged"})

    def test_cli_load_smoke_is_bail_and_configured(self):
        build = {
            "artifacts": {
                "cli": {"path": "/frozen/duckdb"},
                "extension": {"path": "/frozen/sirius.duckdb_extension"},
                "library": {"path": "/frozen/libsirius.so"},
            },
            "library_search_paths": ["/frozen"],
        }
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp) / "smoke"

            def fake_process(command, env, cwd, timeout):
                self.assertIn("-bail", command)
                self.assertIn("-no-agent", command)
                self.assertEqual(env["SIRIUS_CONFIG_FILE"], "/config.yaml")
                (cwd / "stdout.log").write_text("s4_extension_loaded\n")
                (cwd / "stderr.log").write_text("calling init: /frozen/libsirius.so\n")

            with patch.object(run, "run_process", side_effect=fake_process):
                run.cli_smoke(build, Path("/config.yaml"), directory, 30)

    def test_campaign_and_single_extension_with_cpu_fake_fixture(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            fake_binary = base / "fake-binary"
            fake_binary.write_text("CPU test artifact; never executed")
            builds = {
                schedule: {
                    "build_id": schedule,
                    "library_search_paths": [],
                    "identify": {
                        "cuda_allocation_alignment": 256,
                        "bloom_block_bytes": 32,
                    },
                    "artifacts": {
                        name: run.artifact(fake_binary)
                        for name in ("fixture", "library", "extension", "cli")
                    },
                }
                for schedule in run.SCHEDULES
            }
            workload = run.workload_set("confirm", "default256", [0, 1, 2, 3])[0]

            def fake_process(command, env, directory, timeout):
                self.assertEqual(env["SIRIUS_ENABLE_TEST_OPTIONS"], "1")
                manifest = run.load_json(directory.parents[1] / "experiment.json")
                job = next(
                    j for j in manifest["jobs"] if j["process_id"] == directory.name
                )
                build = builds[job["schedule"]]
                records = [
                    native_sample(workload, job, manifest, build, i, warmup)
                    for warmup, count in (
                        (True, job["warmups"]),
                        (False, job["repetitions"]),
                    )
                    for i in range(count)
                ]
                (directory / "native.jsonl").write_text(
                    "".join(json.dumps(r) + "\n" for r in records)
                )

            with patch.object(run, "load_builds", return_value=builds), patch.object(
                run, "verify_build"
            ), patch.object(run, "cli_smoke"), patch.object(
                run, "machine_info", return_value={"hostname": "cpu-test"}
            ), patch.object(
                run, "workload_set", return_value=[workload]
            ), patch.object(
                run, "run_process", side_effect=fake_process
            ), contextlib.redirect_stdout(
                io.StringIO()
            ):
                args = arguments("confirm", "--output", str(base / "original"))
                run.campaign(args)
                sources = run.load_json(base / "original" / "experiment.json")[
                    "harness_sources"
                ]
                self.assertEqual(set(sources), {"runner", "analyzer", "build_helper"})
                for name, filename in (
                    ("runner", "run_s4_experiments.py"),
                    ("analyzer", "analyze_s4_experiments.py"),
                    ("build_helper", "build_s4_experiments.py"),
                ):
                    self.assertEqual(
                        sources[name], run.artifact(run.ROOT / "scripts" / filename)
                    )
                result = analyze.analyze(base / "original")
                self.assertEqual(
                    result["outcomes"][0]["statistics"]["independent_process_pairs"], 5
                )
                args = arguments(
                    "confirm",
                    "--output",
                    str(base / "extended"),
                    "--extend-from",
                    str(base / "original"),
                    "--seed",
                    "1924",
                )
                run.campaign(args)
                result = analyze.analyze(base / "extended")
                self.assertEqual(
                    result["outcomes"][0]["statistics"]["independent_process_pairs"], 10
                )
                self.assertEqual(
                    result["outcomes"][0]["statistics"]["measured_pairs"], 60
                )


if __name__ == "__main__":
    unittest.main()
