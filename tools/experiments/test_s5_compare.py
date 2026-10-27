"""CPU-only checks; no native DuckDB, CUDA, GPU observation, or build is invoked."""

from decimal import Decimal
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import s5_compare as s5


class OracleChecks(unittest.TestCase):
    def test_integer_decimal_null_and_row_multiplicity_are_exact(self):
        schema = [{"column_name": "n", "column_type": "BIGINT"}]
        self.assertFalse(s5.same_rows([(10**16,)], [(10**16 + 1,)], schema, 4))
        self.assertFalse(
            s5.same_rows([(Decimal("1.00000000001"),)], [(Decimal("1.0"),)], schema, 4)
        )
        self.assertFalse(s5.same_rows([(None,)], [("",)], schema, 4))
        self.assertTrue(s5.same_rows([(2,), (1,), (1,)], [(1,), (2,), (1,)], schema, 4))
        self.assertFalse(s5.same_rows([(2,), (1,), (1,)], [(1,), (2,)], schema, 4))

    def test_only_recognized_q1_avg_disc_allows_rounding(self):
        schema = [
            {"column_name": n, "column_type": "VARCHAR" if i < 2 else "DOUBLE"}
            for i, n in enumerate(s5.Q1_COLUMNS)
        ]
        row = ("A", "F", 1, 2, 3, 4, 5, 6, Decimal("0.05000000000000001"), 10**16)
        changed = (*row[:8], Decimal("0.05"), row[9])
        self.assertTrue(s5.same_rows([row], [changed], schema, 1))
        self.assertFalse(s5.same_rows([row], [changed], schema, 7))
        self.assertFalse(s5.same_rows([row], [(*changed[:9], row[9] + 1)], schema, 1))
        self.assertFalse(s5.same_rows([row, row], [changed, changed], schema, 1))
        schema[8]["column_type"] = "DECIMAL(18,16)"
        self.assertFalse(s5.same_rows([row], [changed], schema, 1))

    def test_json_duplicates_and_nonfinite_fail(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "data.json"
            for content in ('[{"n":1,"n":2}]', '[{"n":NaN}]'):
                path.write_text(content)
                with self.assertRaises(ValueError):
                    s5.load(path, typed=True)
            path.write_text('[{"n":10000000000000001,"s":"","null":null}]')
            self.assertEqual(
                s5.rows(path, ["n", "s", "null"]), [(10000000000000001, "", None)]
            )


class PlanningChecks(unittest.TestCase):
    def test_all_arms_are_fresh_and_randomization_reproducible(self):
        jobs = s5.plan([4, 7], s5.ARMS, 2, 2, 3, 61)
        self.assertEqual(jobs, s5.plan([4, 7], s5.ARMS, 2, 2, 3, 61))
        self.assertNotEqual(jobs, s5.plan([4, 7], s5.ARMS, 2, 2, 3, 62))
        self.assertEqual(len({j["id"] for j in jobs}), len(jobs))
        for query in (4, 7):
            for block in range(2):
                current = [
                    j for j in jobs if j["query"] == query and j["block"] == block
                ]
                self.assertEqual({j["arm"] for j in current}, set(s5.ARMS))
                self.assertTrue(
                    all(
                        j["hints"] and j["warmups"] == 2 and j["reps"] == 3
                        for j in current
                    )
                )

    def test_missing_and_duplicate_arms_rejected(self):
        for arms in (("0.9", "2.0"), ("0.9", "0.9", "2.0")):
            with self.assertRaises(ValueError):
                s5.plan([4], arms, 1, 1, 1, 1)

    def test_summary_rejects_missing_duplicate_and_truncated_records(self):
        jobs = s5.plan([4], s5.ARMS, 1, 1, 1, 1)
        manifest = dict(
            jobs=jobs, queries=[4], blocks=1, hardware={"selected": [{}, {}]}
        )
        records = [
            dict(
                job=j,
                valid=True,
                times_seconds=[1.0] * (j["warmups"] + j["reps"]),
                domain_evidence=[],
            )
            for j in jobs
        ]
        report = s5.summarize(manifest, records)
        self.assertTrue(
            all(
                row["ci95"] is None and row["within_3pct"] is None
                for row in report["comparisons"]
            )
        )
        for bad in (
            records[:-1],
            records[:-1] + records[:1],
            [dict(records[0], times_seconds=[])] + records[1:],
        ):
            with self.assertRaises(ValueError):
                s5.summarize(manifest, bad)

    def test_ci_counts_blocks_not_repetitions(self):
        result = s5.interval([0.0] * 5)
        self.assertEqual(result["independent_blocks"], 5)
        self.assertEqual(result["ci95"], [1.0, 1.0])
        self.assertTrue(result["within_3pct"])
        self.assertIsNone(s5.interval([0.0] * 2)["ci95"])

    def test_interrupted_archive_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(FileNotFoundError):
                s5.analyze(Path(temp))

    def test_confirmation_has_five_fresh_pairs_and_five_controls(self):
        jobs = s5.confirmation_plan([4, 21], "0.45", "0.9", 82)
        for query in (4, 21):
            for block in range(5):
                for phase in ("confirmation", "control"):
                    pair = [
                        j
                        for j in jobs
                        if j["query"] == query
                        and j["block"] == block
                        and j["phase"] == phase
                    ]
                    self.assertEqual(len(pair), 2)
                    self.assertEqual(
                        {j["side"] for j in pair}, {"candidate", "reference"}
                    )
                    self.assertTrue(
                        all(j["warmups"] == 2 and j["reps"] == 6 for j in pair)
                    )
                    if phase == "control":
                        self.assertEqual({j["arm"] for j in pair}, {"0.9"})
        records = [
            dict(
                job=j,
                valid=True,
                times_seconds=[1.0] * (j["warmups"] + j["reps"]),
                domain_evidence=[],
            )
            for j in jobs
        ]
        manifest = dict(
            jobs=jobs,
            queries=[4, 21],
            blocks=5,
            confirmation={"candidate": "0.45"},
            hardware={"selected": [{}, {}]},
        )
        summary = s5.summarize(manifest, records)
        self.assertEqual(len(summary["comparisons"]), 4)
        self.assertTrue(
            all(r["independent_blocks"] == 5 for r in summary["comparisons"])
        )
        stable_summary = summary
        self.assertTrue(
            all(
                r["verdict"] == "nonregression_observed"
                and r["baseline_drift_within_3pct"]
                for r in summary["comparisons"]
            )
        )
        for record in records:
            job = record["job"]
            if (
                job["phase"] == "control"
                and job["query"] == 4
                and job["side"] == "candidate"
            ):
                record["times_seconds"] = [1.01] * 8
        summary = s5.summarize(manifest, records)
        control = next(
            r
            for r in summary["comparisons"]
            if r["query"] == 4 and r["phase"] == "control"
        )
        self.assertFalse(control["baseline_drift_within_3pct"])
        self.assertAlmostEqual(control["ratio"], 1.01)
        for ratio in (1.0, 0.94, 1.04):
            with self.subTest(ratio=ratio):
                for record in records:
                    job = record["job"]
                    if (
                        job["phase"] == "confirmation"
                        and job["query"] == 4
                        and job["side"] == "candidate"
                    ):
                        record["times_seconds"] = [ratio] * 8
                summary = s5.summarize(manifest, records)
                confirmation = summary["comparisons"][0]
                self.assertEqual(confirmation["verdict"], "inconclusive")
                self.assertIsNone(confirmation["within_3pct"])
                self.assertIsNone(confirmation["gain_at_least_5pct"])
                self.assertFalse(confirmation["baseline_drift_within_3pct"])
                self.assertEqual(
                    confirmation["attribution_reason"],
                    "same_query_baseline_control_failed",
                )
                self.assertAlmostEqual(confirmation["ratio"], ratio)
                for value in confirmation["ci95"] + confirmation["block_ratios"]:
                    self.assertAlmostEqual(value, ratio)
                self.assertEqual(summary["comparisons"][1], control)
                self.assertEqual(
                    summary["comparisons"][2:], stable_summary["comparisons"][2:]
                )
                self.assertEqual(
                    summary["diagnostic_gate_counts"],
                    stable_summary["diagnostic_gate_counts"],
                )

    def test_correctness_mode_covers_all_thresholds_and_df_off(self):
        jobs = s5.plan(list(range(1, 23)), s5.ARMS, 1, 1, 1, 82, correctness_only=True)
        self.assertEqual(len(jobs), 88)
        self.assertEqual(sum(j["phase"] == "df_off" for j in jobs), 22)
        self.assertEqual(sum(j["phase"] == "correctness" for j in jobs), 66)
        self.assertFalse(any(j["phase"] == "timing" for j in jobs))


class ProtocolChecks(unittest.TestCase):
    def test_markers_and_timings_must_be_exact(self):
        text = "S5_BEGIN_0\nRun Time (s): real 0.123456789 user 0.01 sys 0.00\nS5_END_0\nS5_COMPLETE\n"
        self.assertEqual(s5.parse_times(text, 1), [0.123456789])
        for bad in (
            text.replace("S5_END_0", ""),
            text + text,
            text.replace("real 0.123456789", "real 0"),
        ):
            with self.assertRaises(ValueError):
                s5.parse_times(bad, 1)

    def test_unique_hint_allowlist_never_asserts_composite_columns(self):
        files = {t: [f"/data/{t}/0.parquet"] for t in s5.TABLES}
        patterns = {t: f"/data/{t}/*.parquet" for t in s5.TABLES}
        columns = dict(
            orders=["o_orderkey"],
            lineitem=["l_orderkey"],
            partsupp=["ps_partkey", "ps_suppkey"],
        )
        sql = "\n".join(s5.preparation(files, columns, True, patterns))
        self.assertEqual(sql.count("unique_cols"), 1)
        self.assertIn("unique_cols=>['o_orderkey']", sql)
        self.assertNotIn(
            "unique_cols", "\n".join(s5.preparation(files, columns, False, patterns))
        )

    def test_sql_fail_fast_settings_and_every_execution(self):
        sql = s5.make_script(
            "SELECT 1 AS n", [], Path("/out"), "/extension", "0.45", False, 3
        )
        self.assertTrue(sql.startswith(".bail on\n"))
        self.assertIn("SET enable_duckdb_fallback=false", sql)
        self.assertIn("SET enable_dynamic_filter=false", sql)
        self.assertEqual(sql.count(".timer on 9"), 3)
        self.assertEqual(sql.count("SELECT 1 AS n;"), 4)

    def test_gate_grammar_and_visibility_preserved(self):
        line = "[debug] [dynamic_filter_domain] path=accumulated condition=2 rows=90 domain=100 provenance=pinned_parquet_exact eligible=1 threshold=0.9 decision=skip"
        self.assertEqual(s5.gate_records(line)[0]["decision"], "skip")
        with self.assertRaises(ValueError):
            s5.gate_records(line.replace("condition=2", "key=2"))
        with patch.dict(
            os.environ,
            {"CUDA_VISIBLE_DEVICES": "GPU-a,GPU-b", "SIRIUS_DISABLE": "1"},
            clear=True,
        ):
            env = s5.environment(Path("config"), Path("logs"))
            self.assertEqual(env["CUDA_VISIBLE_DEVICES"], "GPU-a,GPU-b")
            self.assertNotIn("SIRIUS_DISABLE", env)

    def test_settings_query_casts_native_string_cap(self):
        captured = '[{"fallback":false,"enabled":false,"multipartition":true,"cap":"268435456","coverage":0.9}]'
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "settings.json"
            path.write_text(captured)
            settings = s5.load(path, typed=True)
        self.assertEqual(settings[0]["cap"], "268435456")
        self.assertNotEqual(settings[0]["cap"], 268435456)
        sql = s5.make_script(
            "SELECT 1 AS n", [], Path("/out"), "/extension", "0.9", False, 1
        )
        self.assertIn(
            "current_setting('max_dynamic_filter_bloom_bytes_per_gpu')::BIGINT AS cap",
            sql,
        )

    def test_full_process_validation_checks_warmups_settings_and_fallback(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            (directory / "sirius-log").mkdir()
            schema = [{"column_name": "n", "column_type": "BIGINT"}]
            job = dict(
                id=0,
                query=4,
                block=0,
                phase="timing",
                arm="0.9",
                hints=True,
                warmups=1,
                reps=1,
            )
            files = {
                "schema.json": json.dumps(schema),
                "settings.json": '[{"fallback":false,"enabled":true,"multipartition":true,"cap":268435456,"coverage":0.9}]',
                "stdout.log": "".join(
                    f"S5_BEGIN_{i}\nRun Time (s): real 0.100000000 user 0.0 sys 0.0\nS5_END_{i}\n"
                    for i in range(2)
                )
                + "S5_COMPLETE\n",
                "stderr.log": "",
                "result-0.json": '[{"n":10000000000000001}]',
                "result-1.json": '[{"n":10000000000000001}]',
                "sirius-log/run.log": "[info] topology summary: 2 GPU(s)\n"
                + (s5.GPU_PLAN + "\n") * 2,
            }
            for name, content in files.items():
                (directory / name).write_text(content)
            self.assertEqual(
                s5.validate_process(directory, job, [(10000000000000001,)], schema, 2)[
                    "oracle_checks"
                ],
                2,
            )
            changes = {
                "result-0.json": '[{"n":10000000000000002}]',
                "settings.json": files["settings.json"].replace(
                    '"coverage":0.9', '"coverage":0.45'
                ),
                "sirius-log/run.log": files["sirius-log/run.log"]
                + "Transparent execution fallback: unsupported\n",
            }
            (directory / "settings.json").write_text(
                files["settings.json"].replace('"cap":268435456', '"cap":"268435456"')
            )
            with self.assertRaisesRegex(ValueError, "Effective settings mismatch"):
                s5.validate_process(directory, job, [(10000000000000001,)], schema, 2)
            (directory / "settings.json").write_text(files["settings.json"])
            for name, bad in changes.items():
                with self.subTest(name=name):
                    (directory / name).write_text(bad)
                    with self.assertRaises(ValueError):
                        s5.validate_process(
                            directory, job, [(10000000000000001,)], schema, 2
                        )
                    (directory / name).write_text(files[name])

    def test_idle_check_filters_selected_gpus(self):
        with patch.object(
            s5, "capture", return_value="GPU-other, 111\nGPU-selected, 222\n"
        ):
            self.assertEqual(
                s5.gpu_apps({"GPU-selected"}, {222}),
                [{"uuid": "GPU-selected", "pid": 222}],
            )
            with self.assertRaises(ValueError):
                s5.gpu_apps({"GPU-selected"})

    def test_process_timeout_and_contamination_preserve_logs(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary = root / "fake-cli"
            binary.write_text(f"#!{sys.executable}\nimport time\ntime.sleep(10)\n")
            binary.chmod(0o700)
            with patch.object(s5, "gpu_apps", return_value=[]):
                with self.assertRaisesRegex(ValueError, "timeout"):
                    s5.execute(
                        binary,
                        "SELECT 1;",
                        root / "timeout",
                        dict(os.environ),
                        0.05,
                        set(),
                    )
            self.assertTrue((root / "timeout/stdout.log").exists())
            with patch.object(
                s5, "gpu_apps", side_effect=[[], ValueError("Selected GPUs busy")]
            ):
                with self.assertRaisesRegex(ValueError, "busy"):
                    s5.execute(
                        binary, "SELECT 1;", root / "busy", dict(os.environ), 10, set()
                    )
            self.assertTrue((root / "busy/stderr.log").exists())


if __name__ == "__main__":
    unittest.main()
