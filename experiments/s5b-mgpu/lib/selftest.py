#!/usr/bin/env python3
# Copyright 2026, Sirius Contributors.
#
# Licensed under the Apache License, Version 2.0 (the "License").
# See the LICENSE file at the repo root for the full text.
"""Host-only checks of the kit's parsing, schemas, sessions, summaries and cleanup guard.

No GPU, no Sirius, no subprocess. Run in S0 before any GPU stage:
    pixi run python -I -B -m unittest discover -s experiments/s5b-mgpu/lib -p selftest.py

The sample log lines live in `formats.py` beside their patterns, written from the format strings
at the cited source lines; a failure here means a pattern no longer matches what Sirius prints,
a table no longer fits its schema, a summary gives the wrong verdict, or RUNBOOK.md's generated
appendices are stale.
"""

from __future__ import annotations

import decimal
import json
import os
import pathlib
import sys
import tempfile
import unittest
from typing import Any, Callable
from unittest import mock

import pyarrow as pa
import pyarrow.parquet as pq

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import bench  # noqa: E402
import common  # noqa: E402
import duck  # noqa: E402
import formats  # noqa: E402
import manifest  # noqa: E402
import parity_stage  # noqa: E402
import schemas  # noqa: E402
import session  # noqa: E402

RUNBOOK = pathlib.Path(__file__).resolve().parents[1] / "RUNBOOK.md"


def parse(*texts: str, operators: bool = False) -> common.LogEvents:
    events = common.LogEvents()
    for text in texts:
        common.parse_line(text, events, operators)
    return events


class Formats(unittest.TestCase):
    def test_every_pattern_matches_its_sample_and_yields_its_fields(self):
        for fmt in formats.ALL_FORMATS:
            with self.subTest(fmt=fmt.name):
                parsed = fmt.parse(fmt.sample)
                self.assertIsNotNone(parsed, fmt.sample)
                if fmt.fields:
                    self.assertEqual(sorted(parsed), sorted(f.name for f in fmt.fields))

    def test_the_key_line_parses_to_the_keys_table(self):
        row = parity_stage.key_row(formats.KEY_LINE_SAMPLE)
        self.assertEqual(
            (row["kind"], row["size"], row["zone_map"], row["pushed"]),
            ("bloom", 4096, True, 2),
        )
        event_columns = schemas.TABLES["C.keys"].names[6:]
        self.assertEqual(tuple(row), event_columns)
        with self.assertRaises(formats.FormatError):
            formats.parse_key_tokens(formats.KEY_LINE_SAMPLE.replace("v=1", "v=2"))

    def test_runtime_fallback_names_the_gpu_error(self):
        self.assertEqual(
            formats.runtime_fallback_error(formats.RUNTIME_FALLBACK.sample),
            "validity row_offset (3) not byte-aligned",
        )
        self.assertIsNone(formats.runtime_fallback_error(formats.GPU_COMPLETED.sample))


class LogParsing(unittest.TestCase):
    def test_build_fold(self):
        fold = parse(formats.BUILD_FOLD.sample).build_folds[0]
        self.assertEqual(
            (fold["fold_policy"], fold["rows"], fold["fold"], fold["bytes_per_row"]),
            ("bounded", 1000, True, 64.0),
        )

    def test_publication_and_bloom(self):
        events = parse(
            formats.BLOOM_FINISH.sample,
            formats.PUBLICATION.sample,
            formats.CONSTRUCTION_STARTED.sample,
        )
        self.assertEqual(events.bloom_transfers[0]["transport"], "host_staging")
        self.assertEqual(
            events.publications[0],
            {
                "host_staged": True,
                "filters_pushed": 5,
                "pushed_to": 7,
                "latency_ms": 1.5,
            },
        )
        self.assertEqual(events.constructions_started, 1)

    def test_join_strategies(self):
        join = parse(formats.JOIN_STRATEGY.sample).join_strategies[0]
        self.assertEqual(
            (join["mode"], join["broadcast"], join["build_rows"], join["join_gpus"]),
            ("MIXED_JOIN", True, None, 4),
        )

    def test_memory(self):
        events = parse(
            formats.RESCHEDULE.sample,
            formats.DOWNGRADE_MONITOR.sample,
            formats.DOWNGRADE_RESERVE.sample,
            formats.GPU_POOL.sample,
        )
        self.assertEqual(events.oom_retries, 1)
        self.assertEqual(events.downgrade_bytes, 1300)
        self.assertEqual(
            (events.reservation_shortfalls, events.reservation_failures), (1, 1)
        )
        self.assertEqual(events.max_gpu_peak_bytes, 999)
        self.assertEqual(parse(formats.DOWNGRADE_DEBUG.sample).downgrade_bytes, 2048)

    def test_operators_window_pins_and_fallbacks(self):
        begin = formats.WINDOW.sample.replace("[window] end", "[window] begin").replace(
            "10:00:02.000", "10:00:00.000"
        )
        events = parse(
            begin,
            formats.OPERATOR_PRODUCED.sample,
            formats.WINDOW.sample,
            formats.GPU_COMPLETED.sample,
            formats.PINNED_SCAN.sample,
            formats.PIN_COMPRESSING.sample,
            operators=True,
        )
        summary = events.operator_summary()[0]
        self.assertEqual(
            (summary["operator"], summary["tasks"], summary["total_ms"]),
            ("HASH_JOIN", 1, 2.5),
        )
        self.assertAlmostEqual(summary["first_output_s"], 0.5, places=3)
        self.assertEqual(
            (events.windows_ok, events.gpu_completed, events.pinned_scans), (1, 1, 1)
        )
        self.assertEqual(events.compressed_pins, ["lineitem"])
        self.assertFalse(events.fallback)
        self.assertTrue(parse(formats.RUNTIME_FALLBACK.sample).fallback)
        self.assertTrue(parse(formats.PLAN_FALLBACK.sample).fallback)

    def test_timer(self):
        run = duck.CliRun(
            0, f"{formats.TIMER_SAMPLE}\nx\nRun Time (s): real 0.5 user 0 sys 0\n", ""
        )
        self.assertEqual(run.timer_seconds, [1.25, 0.5])


class SettingsProblems(unittest.TestCase):
    """Finding L1: a run whose logged settings disagree with what it SET is flagged."""

    def test_agreeing_settings_pass(self):
        settings = common.settings_dict(
            ["build_fold_policy='bounded'", "concat_batch_bytes=1024"]
        )
        self.assertEqual(
            common.settings_problems(settings, parse(formats.BUILD_FOLD.sample)), []
        )

    def test_disagreeing_settings_are_reported(self):
        settings = common.settings_dict(
            ["build_fold_policy='never'", "concat_batch_bytes=7", "max_build_hash_table_bytes=0",
             "enable_dynamic_filter=false"]
        )  # fmt: skip
        probe = formats.JOIN_STRATEGY.sample.replace("MIXED_JOIN", "BUILD_PROBE")
        problems = common.settings_problems(
            settings,
            parse(
                formats.BUILD_FOLD.sample, probe, formats.CONSTRUCTION_STARTED.sample
            ),
        )
        self.assertEqual(len(problems), 4, problems)


class Schemas(unittest.TestCase):
    """Finding B1 and L7: no column can overwrite another, and RUNBOOK documents the schemas."""

    def test_merge_row_refuses_a_column_supplied_twice(self):
        with self.assertRaises(schemas.SchemaError):
            schemas.merge_row({"filters": "on"}, {"filters": 3})

    def test_write_table_refuses_an_undeclared_column(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(schemas.SchemaError):
                schemas.write_table(
                    "S0.settings", [{"nme": "x"}], pathlib.Path(tmp) / "t.parquet"
                )

    def test_cell_and_publication_columns_are_distinct(self):
        names = schemas.TABLES["P.publications"].names
        self.assertIn("filters", names)
        self.assertIn("filters_pushed", names)

    def test_runbook_appendices_are_current(self):
        text = RUNBOOK.read_text()
        self.assertEqual(
            schemas.render_runbook(text), text, "run lib/schemas.py --write-runbook"
        )


class Results(unittest.TestCase):
    """Result comparison, including finding L3 (order-sensitive) and the one-ulp hash case."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self.tmp.name)
        table = pa.table(
            {
                "name": pa.array(['a, quoted "x"', "b"]),
                "day": pa.array([19000, 19001], pa.date32()),
                "price": pa.array(
                    [decimal.Decimal("1234567890123.45"), decimal.Decimal("2.50")],
                    pa.decimal128(38, 2),
                ),
                "ratio": pa.array([0.1 + 0.2, 1.0 / 3.0]),
                "n": pa.array([1, 2], pa.int64()),
            }
        )
        self.ref = self.dir / "ref.parquet"
        pq.write_table(table, self.ref)

    def tearDown(self):
        self.tmp.cleanup()

    def write_csv(self, body: str, name: str = "gpu.csv") -> pathlib.Path:
        path = self.dir / name
        path.write_text("name,day,price,ratio,n\n" + body)
        return path

    def test_in_order_with_float_tolerance(self):
        csv = self.write_csv(
            '"a, quoted ""x""",2022-01-08,1234567890123.45,0.30000000000000004,1\n'
            "b,2022-01-09,2.50,0.33333333333333326,2\n"
        )
        outcome = common.compare_to_reference(csv, self.ref)
        self.assertEqual(
            (outcome["ref_match"], outcome["ref_order"]), (True, "ordered"), outcome
        )
        self.assertEqual(
            outcome["result_hash"], common.result_hash(common.read_reference(self.ref))
        )

    def test_reordered_rows_are_reported_as_a_multiset_match(self):
        csv = self.write_csv(
            "b,2022-01-09,2.50,0.3333333333333333,2\n"
            '"a, quoted ""x""",2022-01-08,1234567890123.45,0.30000000000000004,1\n'
        )
        outcome = common.compare_to_reference(csv, self.ref)
        self.assertEqual(
            (outcome["ref_match"], outcome["ref_order"]), (True, "multiset")
        )

    def test_mismatch(self):
        csv = self.write_csv(
            "b,2022-01-09,2.51,0.3333333333333333,2\n"
            '"a, quoted ""x""",2022-01-08,1234567890123.45,0.30000000000000004,1\n'
        )
        outcome = common.compare_to_reference(csv, self.ref)
        self.assertEqual((outcome["ref_match"], outcome["ref_order"]), (False, None))

    def test_values_one_ulp_apart_match_although_their_hashes_may_differ(self):
        left = pa.table({"x": [0.12345678905]})
        right = pa.table({"x": [0.12345678904999999]})
        self.assertEqual(common.tables_match(left, right)[0], "ordered")
        a = self.write_csv("", "a.csv")
        a.write_text("x\n0.12345678905\n")
        b = self.write_csv("", "b.csv")
        b.write_text("x\n0.12345678904999999\n")
        self.assertTrue(common.csv_results_match(a, b, None))


class GpuState(unittest.TestCase):
    def test_throttle_reasons_are_decoded(self):
        self.assertEqual(common.decode_throttle("0x0000000000000001"), "gpu_idle")
        self.assertEqual(
            common.decode_throttle("0x24"), "sw_power_cap,sw_thermal_slowdown"
        )
        self.assertEqual(common.decode_throttle("0x0"), "none")
        self.assertEqual(common.decode_throttle("[N/A]"), "unknown")

    def test_nvidia_smi_rows(self):
        rows = common.parse_gpu_state(
            "0, 1980, 2619, 1980, 41, 85.50, P0, 0x0000000000000004\n"
        )
        self.assertEqual(
            (rows[0]["gpu"], rows[0]["sm_clock_mhz"], rows[0]["throttle_reasons"]),
            (0, 1980, "sw_power_cap"),
        )
        self.assertEqual(rows[0].keys(), set(schemas.TABLES["P.gpu_state"].names[3:]))


class Sessions(unittest.TestCase):
    def test_every_pass_is_a_block_of_every_cell_in_a_seeded_order(self):
        cells = [f"c{i}" for i in range(8)]
        queries = ["q1", "q2", "q3", "q4"]
        steps = session.schedule(cells, queries, 6, seed=7)
        self.assertEqual(len(steps), 6 * 8 * 4)
        self.assertEqual(steps, session.schedule(cells, queries, 6, seed=7))
        self.assertNotEqual(steps, session.schedule(cells, queries, 6, seed=8))
        orders = []
        for pass_ in range(6):
            block = [s for s in steps if s.pass_ == pass_]
            self.assertEqual(sorted({s.cell for s in block}), cells)
            order = list(dict.fromkeys(s.cell for s in block))
            self.assertEqual(
                len(order), len(cells)
            )  # each cell visited once, contiguously
            orders.append(order)
        self.assertGreater(len({tuple(o) for o in orders}), 1)
        firsts = {next(s.query for s in steps if s.pass_ == p) for p in range(6)}
        self.assertGreater(len(firsts), 1)  # the query order rotates between passes
        self.assertEqual([s.index for s in steps], list(range(len(steps))))

    def test_timers_are_attributed_by_step_marker(self):
        stdout = "\n".join(
            [
                "@@S5B 0",
                formats.TIMER_SAMPLE,
                "Run Time (s): real 9.000000 user 0 sys 0",  # a later SET; not a step
                "@@S5B 1",
                "@@S5B 2",
                "Run Time (s): real 0.500000 user 0 sys 0",
            ]
        )
        self.assertEqual(session.step_latencies(stdout), {0: 1250.0, 2: 500.0})

    def test_script_pins_then_sets_each_cell_before_its_queries(self):
        spec = spec_of(
            "P2",
            "matrix_1gpu",
            "matrix",
            "tip",
            1,
            [
                session.Cell("a", (("enable_dynamic_filter", "true"),)),
                session.Cell("b"),
            ],
            ("q1",),
            passes=2,
        )
        steps = session.schedule(["a", "b"], ["q1"], 2, 0)
        text = session.script(
            spec,
            steps,
            pathlib.Path("/w"),
            {"q1": "SELECT 1;"},
            ["LOAD x;"],
            ["CALL pin_table('p', tier => 'host', name => 't');"],
        )
        lines = text.splitlines()
        self.assertLess(
            lines.index(".bail on"),
            lines.index("CALL pin_table('p', tier => 'host', name => 't');"),
        )
        self.assertLess(
            lines.index("CALL pin_table('p', tier => 'host', name => 't');"),
            lines.index(".bail off"),
        )
        self.assertEqual(text.count("SELECT 1;"), 4)
        self.assertEqual(text.count("@@S5B "), 4)
        self.assertEqual(text.count(".once "), 2)  # warm pass only
        # `.once` redirects only the next shell command, so the query must follow it directly.
        for at, line in enumerate(lines):
            if line.startswith(".once "):
                self.assertEqual(lines[at + 1], "SELECT 1;")
        self.assertIn("SET enable_dynamic_filter = true;", text)

    def test_pin_glob_matches_the_view_file_list(self):
        single = [pathlib.Path("/d/nation.parquet")]
        parts = [
            pathlib.Path("/d/lineitem/a.parquet"),
            pathlib.Path("/d/lineitem/b.parquet"),
        ]
        self.assertEqual(duck.pin_glob("nation", single), "/d/nation.parquet")
        self.assertEqual(duck.pin_glob("lineitem", parts), "/d/lineitem/*.parquet")
        statement = duck.pin_statements({"nation": single})[0]
        self.assertEqual(
            statement,
            "CALL pin_table('/d/nation.parquet', tier => 'host', name => 'nation');",
        )

    def test_run_status(self):
        ok = parse(formats.WINDOW.sample, formats.PINNED_SCAN.sample)
        self.assertEqual(session.run_status(10.0, ok, True), "ok")
        self.assertEqual(session.run_status(None, ok, True), "error")
        self.assertEqual(session.run_status(10.0, ok, False), "not_run")
        self.assertEqual(
            session.run_status(10.0, parse(formats.WINDOW.sample), True), "unpinned"
        )
        fell_back = parse(
            formats.WINDOW.sample,
            formats.PINNED_SCAN.sample,
            formats.RUNTIME_FALLBACK.sample,
        )
        self.assertEqual(session.run_status(10.0, fell_back, True), "fallback")

    def test_build_record_from_a_session_directory(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.dict(
            os.environ, {"S5B_WORK": tmp, "S5B_WT_tip": tmp}
        ):
            directory = pathlib.Path(tmp) / "P1" / "tip_4gpu"
            spec = spec_of(
                "P1",
                "tip_4gpu",
                "tip",
                "tip",
                4,
                [session.Cell("default")],
                ("q1", "q2"),
                passes=2,
            )
            steps = session.schedule(["default"], ["q1", "q2"], 2, 0)
            (directory / "setup").mkdir(parents=True)
            (directory / "setup" / "s.log").write_text(formats.GPU_POOL.sample + "\n")
            (directory / "end").mkdir()
            for step in steps:
                run_dir = session.run_dir(directory, step)
                run_dir.mkdir(parents=True)
                lines = [formats.WINDOW.sample, formats.PUBLICATION.sample]
                if step.index != 3:
                    lines.append(formats.PINNED_SCAN.sample)
                (run_dir / "sirius.log").write_text("\n".join(lines) + "\n")
            stdout = (
                "\n".join(f"@@S5B {i}\n{formats.TIMER_SAMPLE}" for i in (0, 2, 3))
                + "\n@@S5B 1\n"
            )
            cli = duck.CliRun(0, stdout, "")
            with mock.patch.object(common, "git_head", return_value="c" * 40):
                record = session.build_record(spec, steps, directory, cli, [], [], 8)
        statuses = [r["status"] for r in record["runs"]]
        self.assertEqual(statuses, ["ok", "error", "ok", "unpinned"])
        self.assertEqual(record["gpu_peak_bytes"], 999)
        self.assertEqual(len(record["warm"]), 2)
        self.assertEqual(record["runs"][0]["publications"][0]["filters_pushed"], 5)


# ---------------------------------------------------------------------------------------------
# Synthetic session records for the summaries
# ---------------------------------------------------------------------------------------------


def spec_of(
    stage, name, role, build, gpus, cells, queries, passes=bench.PASSES, seed=0
) -> session.Spec:
    return session.Spec(
        stage=stage,
        name=name,
        role=role,
        build=build,
        gpus=gpus,
        sf=100,
        cells=tuple(cells),
        queries=tuple(queries),
        passes=passes,
        seed=seed,
        warm_log_level="debug",
        timeout_s=1.0,
        stall_s=1.0,
    )


def make_record(
    spec: session.Spec, latency: Callable[[session.Cell, str], float], oom=None
) -> dict[str, Any]:
    oom = oom or (lambda cell, query: 0)
    runs, warm = [], []
    for step in session.schedule(
        [c.label for c in spec.cells], list(spec.queries), spec.passes, spec.seed
    ):
        cell = spec.cell(step.cell)
        filters_on = cell.filters != "off"
        runs.append(
            {
                "pass": step.pass_,
                "kind": step.kind,
                "cell": step.cell,
                "query": step.query,
                "latency_ms": latency(cell, step.query),
                "status": "ok",
                "oom_retries": oom(cell, step.query) if step.pass_ == 1 else 0,
                "reschedules": 0,
                "downgrade_bytes": 0,
                "reservation_shortfalls": 0,
                "reservation_failures": 0,
                "pinned_scans": 2,
                "plan_fallback": False,
                "runtime_fallback": False,
                "severe_lines": 0,
                "publications": (
                    [
                        {
                            "host_staged": False,
                            "filters_pushed": 3,
                            "pushed_to": 4,
                            "latency_ms": 2.0,
                        }
                    ]
                    if filters_on
                    else []
                ),
            }
        )
        if step.pass_ == 0:
            warm.append(
                {
                    "cell": step.cell,
                    "query": step.query,
                    "result_csv": "/nonexistent.csv",
                    "result_hash": "h",
                    "result_rows": 1,
                    "ref_match": True,
                    "ref_order": "ordered",
                    "ref_detail": "equal",
                    "settings_problems": [],
                    "build_folds": [
                        {
                            "sig": "s",
                            "join_id": "1",
                            "fold_policy": cell.policy or "legacy_filter_coupled",
                            "mode": "STANDARD",
                            "rows": 10,
                            "resident_bytes": 400,
                            "concat_batch_bytes": 1,
                            "publishes_filters": filters_on,
                            "fold": cell.policy == "bounded",
                            "bytes_per_row": 40.0,
                        }
                    ],
                    "bloom_transfers": [],
                    "join_strategies": [],
                    "operators": [
                        {
                            "operator": "HASH_JOIN",
                            "operator_id": 3,
                            "tasks": 4,
                            "total_ms": 9.0,
                            "first_output_s": 0.2,
                        }
                    ],
                }
            )
    return {
        "spec": spec.meta(),
        "commit": "c" * 40,
        "status": "ok",
        "returncode": 0,
        "wall_s": 100.0,
        "setup_s": 10.0,
        "gpu_peak_bytes": 2**30,
        "host_rss_peak_bytes": 2**31,
        "mem_available_min_bytes": 2**33,
        "disk_peak_bytes": 2**20,
        "pinned_tables": 8,
        "compressed_tables": 0,
        "fallback_banner": False,
        "gpu_state": {
            "before": common.parse_gpu_state(
                "0, 1980, 2619, 1980, 41, 85.50, P0, 0x1\n"
            ),
            "after": common.parse_gpu_state(
                "0, 1980, 2619, 1980, 55, 85.50, P0, 0x4\n"
            ),
        },
        "error": None,
        "runs": runs,
        "warm": warm,
        "excerpt": [],
    }


class SummaryCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = pathlib.Path(self.tmp.name)
        self.env = mock.patch.dict(
            os.environ,
            {"S5B_WORK": str(root / "work"), "S5B_RESULTS": str(root / "results")},
        )
        self.env.start()
        self.settings = mock.patch.object(
            common,
            "resolved_settings",
            return_value={
                "hash_partition_bytes": "800",
                "max_build_hash_table_bytes": "5000",
            },
        )
        self.settings.start()

    def tearDown(self):
        self.settings.stop()
        self.env.stop()
        self.tmp.cleanup()

    def store(self, record: dict) -> None:
        spec = record["spec"]
        common.write_json_atomic(
            common.env_path("S5B_WORK") / spec["stage"] / spec["name"] / "record.json",
            record,
        )

    def summary(self, stage: str) -> str:
        return (common.env_path("S5B_RESULTS") / stage / "summary.md").read_text()


class P1Summaries(SummaryCase):
    """No-regression verdicts: A/A noise, confirmation of slow queries, INCOMPLETE (M3, M5)."""

    def sessions(
        self, gpus, tip, roles=bench.P1_ROLES, queries=common.TPCH_QUERIES, base=100.0
    ):
        for role, build in roles:
            spec = spec_of(
                "P1",
                f"{role}_{gpus}gpu",
                role,
                build,
                gpus,
                [session.Cell("default")],
                queries,
            )
            if build == "tip":
                self.store(make_record(spec, tip))
            else:
                self.store(
                    make_record(
                        spec,
                        lambda c, q, b=base, r=role: b
                        * (1.02 if r == "base_b" else 1.0),
                    )
                )

    def args(self):
        return bench.parse_args(["summarize", "--stage", "P1", "--sf", "100"])

    def test_pass_with_noise_and_the_memory_comparison(self):
        for gpus in (4, 1):
            self.sessions(gpus, lambda c, q: 101.0)
        self.assertEqual(bench.summarize("P1", self.args()), 0)
        compare = pq.read_table(
            common.env_path("S5B_RESULTS") / "P1" / "compare.parquet"
        ).to_pylist()
        self.assertEqual(len(compare), 44)
        row = compare[0]
        self.assertAlmostEqual(row["aa_ratio"], 1.02)
        self.assertAlmostEqual(row["base_ms"], 100.0 * 1.02**0.5)
        summary = self.summary("P1")
        self.assertIn("**Overall: PASS**", summary)
        self.assertIn("A/A geomean base_b/base_a 1.0200", summary)
        self.assertIn("information only; not a pass bar", summary)
        self.assertIn("sw_power_cap", summary)
        self.assertIn(
            "Publication Sessions logged: 132", summary
        )  # 22 queries x 6 runs, 4-GPU tip

    def test_a_missing_gpu_count_is_incomplete(self):
        self.sessions(4, lambda c, q: 100.0)
        self.assertEqual(bench.summarize("P1", self.args()), common.EXIT_INCOMPLETE)
        self.assertIn("**Overall: INCOMPLETE**", self.summary("P1"))

    def test_a_slow_query_needs_confirmation_and_the_verdict_uses_it(self):
        slow = lambda c, q: 130.0 if q == "q5" else 100.0  # noqa: E731
        for gpus in (4, 1):
            self.sessions(gpus, slow)
        self.assertEqual(bench.summarize("P1", self.args()), common.EXIT_INCOMPLETE)
        records = bench.load_records("P1")
        specs = bench.confirm_specs("P1", self.args(), 4, records)
        self.assertEqual(
            [s.name for s in specs], ["base_confirm_4gpu", "tip_confirm_4gpu"]
        )
        self.assertEqual(specs[0].queries, ("q5",))
        for gpus in (4, 1):  # the confirming run shows the slowdown was noise
            self.sessions(gpus, lambda c, q: 100.0, bench.P1_CONFIRM_ROLES, ("q5",))
        self.assertEqual(bench.summarize("P1", self.args()), 0)
        self.assertIn("q5 1.287 -> 1.000", self.summary("P1"))

    def test_a_confirmed_slow_query_fails(self):
        slow = lambda c, q: 130.0 if q == "q5" else 100.0  # noqa: E731
        for gpus in (4, 1):
            self.sessions(gpus, slow)
            self.sessions(gpus, slow, bench.P1_CONFIRM_ROLES, ("q5",))
        self.assertEqual(bench.summarize("P1", self.args()), 1)
        self.assertIn("queries over 1.1 after confirmation: ['q5']", self.summary("P1"))


class FoldSummaries(SummaryCase):
    """P2 decisions, including the B1 path: filters on, publications logged."""

    def args(self):
        return bench.parse_args(["summarize", "--stage", "P2", "--queries", "q1,q2"])

    def matrix(
        self,
        latency,
        oom=None,
        cells=None,
        name="matrix_1gpu",
        queries=("q1", "q2"),
        seed=5,
    ):
        cells = cells if cells is not None else bench.fold_cells(self.args(), 1)
        spec = spec_of(
            "P2",
            name,
            "matrix" if name.startswith("matrix") else "confirm",
            "tip",
            1,
            cells,
            queries,
            seed=seed,
        )
        self.store(make_record(spec, latency, oom))

    @staticmethod
    def by_policy(never=100.0, bounded=90.0, legacy=99.0):
        return lambda c, q: {
            "never": never,
            "bounded": bounded,
            "legacy_filter_coupled": legacy,
        }[c.policy]

    def test_the_faster_policy_wins_with_filters_on(self):
        self.matrix(self.by_policy())
        self.assertEqual(bench.summarize("P2", self.args()), 0)
        self.assertIn("**Decision (criteria 1-5): `bounded`**", self.summary("P2"))
        out = common.env_path("S5B_RESULTS") / "P2"
        publications = pq.read_table(out / "publications.parquet").to_pylist()
        self.assertTrue(publications)
        self.assertEqual(
            {(p["filters"], p["filters_pushed"]) for p in publications}, {("on", 3)}
        )
        self.assertEqual(pq.read_table(out / "compare.parquet").num_rows, 2 * (30 - 12))

    def test_missing_cells_are_incomplete(self):
        cells = [c for c in bench.fold_cells(self.args(), 1) if c.experiment == "E1"]
        self.matrix(self.by_policy(), cells=cells)
        self.assertEqual(bench.summarize("P2", self.args()), common.EXIT_INCOMPLETE)
        self.assertIn("**Decision (criteria 1-5): INCOMPLETE**", self.summary("P2"))

    def test_more_oom_retries_need_confirmation_then_disqualify(self):
        oom = lambda c, q: (
            1 if c.policy == "bounded" and q == "q1" and c.experiment == "E1" else 0
        )  # noqa: E731
        self.matrix(self.by_policy(), oom)
        self.assertEqual(bench.summarize("P2", self.args()), common.EXIT_INCOMPLETE)
        records = bench.load_records("P2")
        specs = bench.confirm_specs("P2", self.args(), 1, records)
        self.assertEqual(len(specs), 1)
        labels = {c.label for c in specs[0].cells}
        self.assertIn("E1_c2_bounded_on", labels)
        self.assertIn("E1_c2_never_on", labels)
        self.assertEqual(specs[0].queries, ("q1",))
        self.assertEqual(specs[0].seed, 6)
        self.matrix(
            self.by_policy(),
            oom,
            cells=list(specs[0].cells),
            name="confirm_1gpu",
            queries=("q1",),
        )
        self.assertEqual(bench.summarize("P2", self.args()), 0)
        self.assertIn("**Decision (criteria 1-5): `never`**", self.summary("P2"))

    def test_a_retry_that_does_not_recur_is_cleared_by_confirmation(self):
        oom = lambda c, q: (
            1 if c.policy == "bounded" and q == "q1" and c.experiment == "E1" else 0
        )  # noqa: E731
        self.matrix(self.by_policy(), oom)
        specs = bench.confirm_specs("P2", self.args(), 1, bench.load_records("P2"))
        self.matrix(
            self.by_policy(),
            None,
            cells=list(specs[0].cells),
            name="confirm_1gpu",
            queries=("q1",),
        )
        self.assertEqual(bench.summarize("P2", self.args()), 0)
        self.assertIn("**Decision (criteria 1-5): `bounded`**", self.summary("P2"))


class ParitySummaries(SummaryCase):
    """C2: the strict Key Outcome bar and INCOMPLETE labels."""

    def label(self, name: str, lines_for: Callable[[str], list[str]]) -> None:
        directory = common.env_path("S5B_WORK") / "C2" / name
        directory.mkdir(parents=True)
        build = name.split("_")[0]
        common.write_json_atomic(
            directory / "meta.json",
            {
                "stage": "C2",
                "label": name,
                "build": build,
                "gpus": 4,
                "sf": 1,
                "settings": [],
                "commit": "c" * 40,
            },
        )
        for query in common.TPCH_QUERIES:
            qdir = directory / query
            qdir.mkdir()
            (qdir / "sirius.log").write_text("\n".join(lines_for(query)) + "\n")
            common.write_json_atomic(
                qdir / "result.json",
                {
                    "returncode": 0,
                    "result_match": True,
                    "result_order": "ordered",
                    "gpu_fallback": False,
                },
            )

    def args(self, labels: str) -> Any:
        return mock.Mock(
            stage="C2",
            labels=labels,
            diff=["key_4gpu:tip_4gpu"],
            info_labels="key_4gpu",
            resource_baseline="",
        )

    def test_a_declared_delta_still_fails_the_strict_bar(self):
        key = formats.KEY_LINE_SAMPLE
        self.label("key_4gpu", lambda q: [key])
        weaker = key.replace("kind=bloom", "kind=none").replace(
            "reason=-", "reason=bloom_cap"
        )
        self.label("tip_4gpu", lambda q: [weaker if q == "q3" else key])
        self.assertEqual(parity_stage.summarize(self.args("key_4gpu,tip_4gpu")), 1)
        rows = pq.read_table(
            common.env_path("S5B_RESULTS") / "C2" / "key_diff.parquet"
        ).to_pylist()
        failing = [r for r in rows if r["strict_failed"]]
        self.assertEqual(len(failing), 1)
        self.assertTrue(failing[0]["declared_delta"])
        self.assertFalse(failing[0]["failed"])  # the parity script alone would pass it

    def test_a_lost_zone_map_fails_the_strict_bar(self):
        key = formats.KEY_LINE_SAMPLE
        self.label("key_4gpu", lambda q: [key])
        lost = key.replace("zone_map=1", "zone_map=0").replace(
            "reason=-", "reason=declined_pipeline_barrier"
        )
        self.label("tip_4gpu", lambda q: [lost if q == "q7" else key])
        self.assertEqual(parity_stage.summarize(self.args("key_4gpu,tip_4gpu")), 1)

    def test_identical_outcomes_pass_and_a_missing_label_is_incomplete(self):
        key = formats.KEY_LINE_SAMPLE
        self.label("key_4gpu", lambda q: [key])
        self.label("tip_4gpu", lambda q: [key])
        self.assertEqual(parity_stage.summarize(self.args("key_4gpu,tip_4gpu")), 0)
        self.assertEqual(
            parity_stage.summarize(self.args("key_4gpu,tip_4gpu,tip_df_off_4gpu")),
            common.EXIT_INCOMPLETE,
        )


class CleanupGuard(unittest.TestCase):
    """Finding M4: cleanup deletes only directories the kit recorded as created."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(os.path.realpath(self.tmp.name))
        self.work = self.root / "home" / "s5b" / "work"
        self.work.mkdir(parents=True)
        (self.work / manifest.WORK_SENTINEL).write_text("")
        self.data = self.root / "home" / "s5b" / "data"
        self.data.mkdir()
        self.environ = {
            "HOME": str(self.root / "home"),
            "S5B_HOME": str(self.root / "home" / "s5b"),
            "S5B_DATA": str(self.data),
            "S5B_TPCH_SF1000": str(self.root / "raid" / "sf1000"),
            "S5B_WT_tip": str(self.root / "home" / "s5b" / "wt-tip"),
        }
        self.recorded = {("work", self.work)}

    def tearDown(self):
        self.tmp.cleanup()

    def problems(self, kind, target, recorded=None):
        return manifest.delete_problems(
            kind,
            target,
            self.recorded if recorded is None else recorded,
            manifest.protected_paths(self.environ, kind),
        )

    def test_a_recorded_work_directory_may_be_deleted(self):
        self.assertEqual(self.problems("work", self.work), [])

    def test_an_unrecorded_directory_is_refused(self):
        self.assertTrue(self.problems("work", self.work, recorded=set()))

    def test_a_directory_holding_the_data_is_refused(self):
        parent = self.root / "home" / "s5b"
        (parent / manifest.WORK_SENTINEL).write_text("")
        problems = self.problems("work", parent, recorded={("work", parent)})
        self.assertTrue(any("protected" in p for p in problems), problems)
        self.environ["S5B_TPCH_SF1000"] = str(self.work / "sf1000")
        self.assertTrue(any("protected" in p for p in self.problems("work", self.work)))

    def test_root_home_a_symlink_and_a_missing_sentinel_are_refused(self):
        self.assertTrue(
            self.problems(
                "work", pathlib.Path("/"), recorded={("work", pathlib.Path("/"))}
            )
        )
        home = self.root / "home"
        (home / manifest.WORK_SENTINEL).write_text("")
        self.assertTrue(self.problems("work", home, recorded={("work", home)}))
        link = self.root / "link"
        link.symlink_to(self.work)
        self.assertTrue(self.problems("work", link, recorded={("work", link)}))
        (self.work / manifest.WORK_SENTINEL).unlink()
        self.assertTrue(any("sentinel" in p for p in self.problems("work", self.work)))

    def test_the_manifest_round_trips(self):
        path = self.root / "manifest"
        path.write_text(f"work\t{self.work}\t2026-11-13T10:00:00\nbogus line\n")
        self.assertEqual(manifest.read_manifest(path), {("work", self.work)})


if __name__ == "__main__":
    unittest.main()
