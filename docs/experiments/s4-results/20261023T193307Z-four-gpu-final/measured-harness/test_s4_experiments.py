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

import build_s4_experiments as build_helper
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
    record.update({field: 0 for field in run.COMMON_SQL_OUTCOMES})
    record.update(
        schema_version=2,
        run_id=manifest["run_id"],
        build_id=build["build_id"],
        sample_id=f"{job['sample_prefix']}:{'warmup' if warmup else 'sample'}:{repetition}",
        process_cluster=job["process_cluster"],
        pair=job["pair"],
        order=job["order"],
        seed=job["seed"],
        revision_role=job["revision_role"],
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
        profile_verified=True,
        publication_metrics_supported=True,
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
        args = arguments("confirm", "--stages", "sql")
        workloads = [{"id": "sql-q4", "mode": "sql", "held_out": False}]
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
                self.assertEqual({a["revision_role"] for a in arms}, {"baseline"})
        self.assertEqual(
            {j["arm"] for j in jobs if j["order"] == 0}, {"baseline", "candidate"}
        )


class StatisticsTests(unittest.TestCase):
    def sql_pair(self):
        left = {name: 0 for name in run.COMMON_SQL_OUTCOMES}
        left.update({name: None for name in run.METRICS})
        left.update(
            mode="sql",
            status="ok",
            gpu_intercepted=True,
            process_cluster=0,
            seed=1,
            repetition=0,
            sample_id="baseline",
            query_elapsed_ns=1000,
            publication_count=2,
            publication_metrics_supported=False,
            harness={"arm": "baseline"},
        )
        right = dict(
            left,
            sample_id="final",
            query_elapsed_ns=1010,
            publication_metrics_supported=True,
            host_elapsed_ns=100,
            published_bitmap_bytes=1024,
            harness={"arm": "candidate"},
        )
        return left, right

    def test_absent_baseline_publication_metrics_keep_query_comparison(self):
        samples = self.sql_pair()
        query = analyze.summarize_pair_group(samples, "query_elapsed_ns")
        publication = analyze.summarize_pair_group(samples, "host_elapsed_ns")
        self.assertEqual(query["statistics"]["measured_pairs"], 1)
        self.assertIsNone(publication["statistics"])
        self.assertEqual(
            publication["excluded_pairs"],
            {"publication_work_not_equivalent_or_not_published": 1},
        )

    def test_changed_publication_or_contributions_exclude_overhead_claim(self):
        for name in (
            "publication_count",
            "accumulation_completed_contributions",
            "filters_pushed",
        ):
            left, right = self.sql_pair()
            right[name] += 1
            with self.subTest(name=name):
                result = analyze.summarize_pair_group([left, right], "query_elapsed_ns")
                self.assertIsNone(result["statistics"])
                self.assertFalse(result["complete_comparison"])
                self.assertEqual(
                    result["changed_work"][0]["outcomes"][name],
                    [left[name], right[name]],
                )

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


class ProvenanceTests(unittest.TestCase):
    def test_baseline_overlay_preserves_production(self):
        original = (
            "if(SIRIUS_BUILD_TESTS)\n  include(cmake/sirius-tests.cmake)\nendif()\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "CMakeLists.txt").write_text(run.baseline_cmake(original))
            for name in run.BASELINE_OVERLAY:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("test-only overlay\n")
            changed = ["CMakeLists.txt"]

            def git(command, **kwargs):
                if command[1:3] == ["rev-parse", "HEAD"]:
                    return run.BASELINE_REVISIONS["prerequisite"]
                if command[1:3] == ["rev-parse", "HEAD^{tree}"]:
                    return run.BASELINE_TREE
                if command[1] == "show":
                    return original.rstrip()
                if command[1:3] == ["diff", "--name-only"]:
                    return "\n".join(changed)
                if command[1] == "ls-files":
                    return "\n".join(run.BASELINE_OVERLAY)
                return ""

            with patch.object(run, "checked", side_effect=git):
                identity = run.source_identity(root, "baseline")
                self.assertTrue(identity["baseline_production_unchanged"])
                self.assertEqual(len(identity["fixture_overlay_sha256"]), 5)
                self.assertIn(
                    "test/cpp/operator/s4_experiment_sql_oracle.hpp",
                    identity["fixture_sources_sha256"],
                )
                changed.append("src/cuda/sirius_dynamic_bloom_filter.cu")
                with self.assertRaisesRegex(ValueError, "non-overlay"):
                    run.source_identity(root, "baseline")
                changed.pop()
                (root / "CMakeLists.txt").write_text(
                    run.baseline_cmake(original) + "add_compile_definitions(ALTERED)\n"
                )
                with self.assertRaisesRegex(ValueError, "test-only include"):
                    run.source_identity(root, "baseline")

    def test_control_overlay_replaces_existing_fixture_include(self):
        original = "  include(cmake/sirius-tests.cmake)\n  include(cmake/sirius-s4-experiments.cmake)\n"
        result = run.baseline_cmake(original)
        self.assertEqual(result.count("sirius-s4-baseline-fixture.cmake"), 1)
        self.assertNotIn("sirius-s4-experiments.cmake", result)

    def test_only_known_configuration_paths_are_normalized(self):
        outputs = []
        for root in (Path("/frozen/baseline"), Path("/frozen/final")):
            settings = {
                "SOURCE": str(root / "duckdb"),
                "BUILD": str(root / "build/release/lib"),
                "INSTALL": str(root / "install/lib"),
                "CMAKE_CUDA_ARCHITECTURES": "120a-real",
                "SIRIUS_TPCH_HAVOC": "OFF",
                "FETCHCONTENT_SOURCE_DIR_CUCO": "/reuse/cuco",
            }
            outputs.append(
                run.comparable_configuration(
                    settings, root, root / "build/release", root / "install", None
                )
            )
        self.assertEqual(*outputs)
        self.assertEqual(outputs[0]["FETCHCONTENT_SOURCE_DIR_CUCO"], "/reuse/cuco")
        self.assertEqual(outputs[0]["CMAKE_CUDA_ARCHITECTURES"], "120a-real")
        with self.assertRaises(ValueError):
            run.comparable_configuration(
                {"SIRIUS_S4_PUBLICATION_SCHEDULE": "concurrent_ingress"},
                root,
                root / "build",
                root / "install",
                "s4_control",
            )

    def test_revision_pair_accepts_different_source_but_rejects_dependencies(self):
        builds = {}
        for role in run.REVISION_ROLES:
            builds[role] = {
                "revision_role": role,
                "baseline_kind": "prerequisite" if role == "baseline" else None,
                "source_sha256": role,
                "configuration_sha256": "matched",
                "source": {
                    "head": (
                        run.BASELINE_REVISIONS["prerequisite"]
                        if role == "baseline"
                        else "final-head"
                    ),
                    "baseline_production_unchanged": role == "baseline",
                    "submodules": [],
                    "fixture_sources_sha256": {"sql": "matched"},
                },
                "identify": {
                    "duckdb_version": "matched",
                    "bloom_block_bytes": 32,
                    "cuda_allocation_alignment": 256,
                },
                "artifacts": {
                    name: {"sha256": role} for name in ("fixture", "library")
                },
            }
        run.validate_build_pair(builds)
        builds["final"]["identify"].update(run.SQL_ORACLE_POLICY)
        with self.assertRaisesRegex(ValueError, "oracle policies differ"):
            run.validate_build_pair(builds)
        builds["baseline"]["identify"].update(run.SQL_ORACLE_POLICY)
        run.validate_build_pair(builds)
        builds["final"]["source"]["fixture_sources_sha256"] = {"sql": "different"}
        with self.assertRaisesRegex(ValueError, "fixture overlay differs"):
            run.validate_build_pair(builds)
        builds["final"]["source"]["fixture_sources_sha256"] = {"sql": "matched"}
        builds["final"]["source"]["submodules"] = [{"revision": "different"}]
        with self.assertRaisesRegex(ValueError, "Dependency"):
            run.validate_build_pair(builds)

    def test_gpu_diagnostics_never_request_baseline_modes(self):
        work = run.workload_set(
            "confirm", "default256", [0, 1]
        ) + run.transport_workloads([0, 1])
        jobs = run.execution_plan(
            work, arguments("confirm", "--stages", "transport,builder")
        )
        self.assertTrue(jobs)
        self.assertEqual({j["revision_role"] for j in jobs}, {"final"})
        self.assertEqual({j["comparison"] for j in jobs}, {"diagnostic"})

    def test_legacy_manifest_has_explicit_frozen_analyzer_guidance(self):
        with self.assertRaisesRegex(ValueError, "9b2e8201"):
            run.verify_build({"schema_version": 1})


class ProtocolTests(unittest.TestCase):
    def test_fixed_oracle_metadata_accepts_legacy_and_binds_new_samples(self):
        self.workload = {"id": "sql-q1", "mode": "sql", "query_id": "q1"}
        record = native_sample(self.workload, self.job, self.manifest, self.build)
        self.validate(record)
        self.build["identify"].update(run.SQL_ORACLE_POLICY)
        with self.assertRaisesRegex(ValueError, "oracle policy differs"):
            self.validate(record)
        record.update(run.SQL_ORACLE_POLICY)
        record.update(oracle_digest="123", result_digest="456")
        self.validate(record)

    def test_oracle_metadata_rejects_changed_missing_and_unbound_policy(self):
        self.workload = {"id": "sql-q1", "mode": "sql", "query_id": "q1"}
        record = native_sample(self.workload, self.job, self.manifest, self.build)
        record.update(run.SQL_ORACLE_POLICY)
        with self.assertRaisesRegex(ValueError, "oracle policy differs"):
            self.validate(record)
        self.build["identify"].update(run.SQL_ORACLE_POLICY)
        for field, value in (
            ("oracle_policy", "any_float"),
            ("oracle_rel_tol", 1e-6),
            ("oracle_abs_tol", None),
        ):
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.validate(dict(record, **{field: value}))
        del record["oracle_abs_tol"]
        with self.assertRaises(ValueError):
            self.validate(record)

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
        self.build = {
            "build_id": "immutable",
            "identify": {"publication_metrics_supported": True},
        }
        self.sample = native_sample(self.workload, self.job, self.manifest, self.build)

    def validate(self, record):
        run.validate_native(
            [record], self.build, self.workload, self.job, self.manifest
        )

    def test_valid_record(self):
        self.validate(self.sample)

    def test_wrong_role_missing_bytes_false_oracle_and_nonfinite_rejected(self):
        for key, bad in (
            ("revision_role", "not_the_build"),
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
        args = arguments("screen", "--warmups", "0", "--pairs", "1", "--stages", "sql")
        self.workload = {"id": "sql-q4", "mode": "sql", "held_out": False}
        jobs = run.execution_plan([self.workload], args)
        manifest = {
            **self.manifest,
            "schema_version": 2,
            "baseline_kind": "prerequisite",
            "phase": "screen",
            "scale": "unknown",
            "four_gpu_handoff": False,
            "stage_a_results": None,
            "dataset": {"availability": "not_requested"},
            "builds": {name: self.build for name in run.REVISION_ROLES},
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
                    if result["outcomes"][0]["comparison"] == "baseline_final"
                    else "noise_control"
                ),
            )
            samples.write_text(samples.read_text().splitlines()[0] + "\n")
            with self.assertRaises(ValueError):
                analyze.validated_bundle(directory)

    def test_manifest_tampering_rejected_before_subprocess(self):
        with self.assertRaises(ValueError):
            run.verify_build({"schema_version": 2, "build_id": "forged"})

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
                role: {
                    "build_id": role,
                    "baseline_kind": "prerequisite" if role == "baseline" else None,
                    "library_search_paths": [],
                    "identify": {
                        "cuda_allocation_alignment": 256,
                        "bloom_block_bytes": 32,
                        "publication_metrics_supported": True,
                    },
                    "artifacts": {
                        name: run.artifact(fake_binary)
                        for name in ("fixture", "library", "extension", "cli")
                    },
                }
                for role in run.REVISION_ROLES
            }
            query = base / "q4.sql"
            query.write_text("SELECT 1")
            workload = {
                "id": "sql-q4",
                "mode": "sql",
                "held_out": False,
                "query_id": "4",
                "query_file": str(query),
                "prepare_file": str(query),
                "query_sha256": run.file_hash(query),
                "prepare_sha256": run.file_hash(query),
                "database": ":memory:",
            }
            dataset = {
                "availability": "available",
                "identity_sha256": "cpu-test",
                "files": [],
            }

            def fake_process(command, env, directory, timeout):
                self.assertEqual(env["SIRIUS_ENABLE_TEST_OPTIONS"], "1")
                manifest = run.load_json(directory.parents[1] / "experiment.json")
                job = next(
                    j for j in manifest["jobs"] if j["process_id"] == directory.name
                )
                build = builds[job["revision_role"]]
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
                run, "prepare_sql", return_value=([workload], dataset)
            ), patch.object(
                run, "run_process", side_effect=fake_process
            ), contextlib.redirect_stdout(
                io.StringIO()
            ):
                args = arguments(
                    "confirm", "--stages", "sql", "--output", str(base / "original")
                )
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
                    "--stages",
                    "sql",
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
