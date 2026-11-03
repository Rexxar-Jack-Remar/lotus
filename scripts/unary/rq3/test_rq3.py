#!/usr/bin/env python3
"""Regression tests for the RQ3 runner and aggregator."""

from __future__ import annotations

import csv
import json
import signal
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import aggregate_rq3
import config
import run_rq3


def json_cell(value: object) -> str:
  return json.dumps(value, separators=(",", ":"))


class RQ3Test(unittest.TestCase):
  def make_row(
      self,
      experiment: dict[str, object],
      files: list[Path],
      *,
      statuses: list[str] | None = None,
      configured: int = 1,
  ) -> dict[str, str]:
    statuses = statuses or ["success"]
    successes = sum(status == "success" for status in statuses)
    stored = [str(path) for path in files]
    hashes = {str(path): run_rq3.sha256(path) for path in files}
    row = {column: "" for column in run_rq3.CSV_COLUMNS}
    row.update(
        {
            "dataset": "test",
            "analysis": "value-flow",
            "dot_name": "input.dot",
            "dot_path": "input.dot",
            "input_sha256": "1" * 64,
            "binary_sha256": "2" * 64,
            "extra_environment": "{}",
            "experiment": str(experiment["name"]),
            "tool": str(experiment["tool"]),
            "configuration": json_cell(
                {key: value for key, value in experiment.items() if key != "enabled"}
            ),
            "warmup_repetitions": str(config.WARMUP_REPETITIONS),
            "configured_repetitions": str(configured),
            "completed_repetitions": str(len(statuses)),
            "successful_repetitions": str(successes),
            "statuses": json_cell(statuses),
            "exit_codes": json_cell([0 if value == "success" else -9 for value in statuses]),
            "time_ms": json_cell([1 if value == "success" else None for value in statuses]),
            "average_time_ms": "1",
            "median_time_ms": "1",
            "peak_rss_bytes": json_cell([100 if value == "success" else None for value in statuses]),
            "average_peak_rss_bytes": "100",
            "median_peak_rss_bytes": "100",
            "wall_time_seconds": json_cell([0.1 if value == "success" else None for value in statuses]),
            "average_wall_time_seconds": "0.1",
            "median_wall_time_seconds": "0.1",
            "stats_per_run": json_cell([
                {"Time (ms)": "1", "process peak RSS (bytes)": "100"}
                for _ in statuses
            ]),
            "timeout_seconds": str(config.TIMEOUT_SECONDS),
            "memory_limit_bytes": str(int(config.MEMORY_LIMIT_GIB * 1024**3)),
            "relation_files": json_cell(stored),
            "relation_sha256": json_cell(hashes),
            "log_files": "[]",
            "capture_command": "[]",
            "capture_status": "success" if files else "not-run",
        }
    )
    return row

  def test_success_requires_every_configured_repetition(self) -> None:
    row = self.make_row(
        {"name": "union-dyck", "tool": "staged", "artifact_kind": "union"},
        [],
        statuses=["success", "timeout"],
        configured=2,
    )
    with mock.patch.object(config, "MEASURED_REPETITIONS", 2):
      self.assertFalse(aggregate_rq3.successful(row))
      self.assertEqual(aggregate_rq3.method_status(row), "partial")

  def test_resume_retries_required_failure_but_retains_optional_timeout(self) -> None:
    required = {
        "name": "union-dyck",
        "tool": "staged",
        "artifact_kind": "union",
        "enabled": True,
    }
    optional = {
        "name": "mcfl-plus-d3",
        "tool": "mcfl",
        "artifact_kind": "mcfl",
        "family": "plus",
        "dimension": 3,
        "required": False,
        "enabled": True,
    }
    with tempfile.TemporaryDirectory() as directory:
      csv_path = Path(directory) / "runs.csv"
      rows = [
          self.make_row(required, [], statuses=["timeout"]),
          self.make_row(optional, [], statuses=["timeout"]),
      ]
      with csv_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=run_rq3.CSV_COLUMNS)
        writer.writeheader()
        writer.writerows(rows)
      with mock.patch.object(config, "EXPERIMENTS", [required, optional]):
        with mock.patch.object(config, "MEASURED_REPETITIONS", 1):
          completed = run_rq3.prepare_csv(csv_path, resume=True)
      self.assertEqual(completed, {("test", "input.dot", "mcfl-plus-d3")})
      with csv_path.open("r", encoding="utf-8", newline="") as stream:
        retained = list(csv.DictReader(stream))
      self.assertEqual([row["experiment"] for row in retained], ["mcfl-plus-d3"])

  def test_missing_acf_makes_aggregation_incomplete(self) -> None:
    experiments: list[dict[str, object]] = [
        {"name": "regularization", "tool": "staged", "artifact_kind": "upper"},
        {"name": "mutual-refinement", "tool": "staged", "artifact_kind": "upper"},
        {"name": "acf", "tool": "unary", "artifact_kind": "components"},
    ]
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      files: dict[str, list[Path]] = {}
      for name, names in {
          "regularization": ["upper.pairs"],
          "mutual-refinement": ["upper.pairs"],
      }.items():
        base = root / name
        base.mkdir()
        files[name] = []
        for filename in names:
          path = base / filename
          path.write_text("", encoding="utf-8")
          files[name].append(path)
      rows = [
          self.make_row(item, files[str(item["name"])])
          for item in experiments[:2]
      ]
      runs = root / "runs.csv"
      with runs.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=run_rq3.CSV_COLUMNS)
        writer.writeheader()
        writer.writerows(rows)
      arguments = [
          "aggregate_rq3.py",
          "--runs",
          str(runs),
          "--bounds",
          str(root / "bounds.csv"),
          "--methods",
          str(root / "methods.csv"),
      ]
      with (
          mock.patch.object(config, "EXPERIMENTS", experiments),
          mock.patch.object(config, "MEASURED_REPETITIONS", 1),
          mock.patch.object(sys, "argv", arguments),
      ):
        self.assertEqual(aggregate_rq3.main(), 1)
      with (root / "bounds.csv").open("r", encoding="utf-8", newline="") as stream:
        bounds = list(csv.DictReader(stream))
      self.assertEqual(bounds[0]["status"], "incomplete")
      self.assertEqual(len(bounds), 2)
      self.assertEqual(bounds[0]["intersection_size"], "")

  def test_both_upper_bounds_measure_acf_tightening(self) -> None:
    experiments: list[dict[str, object]] = [
        {"name": "regularization", "tool": "staged", "artifact_kind": "upper"},
        {"name": "mutual-refinement", "tool": "staged", "artifact_kind": "upper"},
        {"name": "acf", "tool": "unary", "artifact_kind": "components"},
    ]
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      contents = {
          "regularization": {"upper.pairs": "1 2\n2 3\n3 4\n"},
          "mutual-refinement": {"upper.pairs": "1 2\n2 3\n"},
          "acf": {"components.map": "1 7\n2 7\n3 7\n4 8\n"},
      }
      files: dict[str, list[Path]] = {}
      for experiment_name, artifacts in contents.items():
        base = root / experiment_name
        base.mkdir()
        files[experiment_name] = []
        for filename, text in artifacts.items():
          path = base / filename
          path.write_text(text, encoding="utf-8")
          files[experiment_name].append(path)
      rows = [
          self.make_row(experiment, files[str(experiment["name"])])
          for experiment in experiments
      ]
      runs = root / "runs.csv"
      with runs.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=run_rq3.CSV_COLUMNS)
        writer.writeheader()
        writer.writerows(rows)
      arguments = [
          "aggregate_rq3.py",
          "--runs",
          str(runs),
          "--bounds",
          str(root / "bounds.csv"),
          "--methods",
          str(root / "methods.csv"),
      ]
      with (
          mock.patch.object(config, "EXPERIMENTS", experiments),
          mock.patch.object(config, "MEASURED_REPETITIONS", 1),
          mock.patch.object(sys, "argv", arguments),
      ):
        self.assertEqual(aggregate_rq3.main(), 0)
      with (root / "bounds.csv").open("r", encoding="utf-8", newline="") as stream:
        bounds = list(csv.DictReader(stream))
      self.assertEqual(len(bounds), 2)
      row = bounds[0]
      self.assertEqual(row["experiment"], "regularization")
      self.assertEqual(row["status"], "success")
      self.assertEqual(row["typed_upper_size"], "3")
      self.assertEqual(row["intersection_size"], "2")
      self.assertEqual(row["newly_excluded_size"], "1")
      self.assertAlmostEqual(float(row["upper_reduction"]), 1.0 / 3.0)
      self.assertEqual(bounds[1]["experiment"], "mutual-refinement")
      self.assertEqual(bounds[1]["typed_upper_size"], "2")
      self.assertEqual(bounds[1]["intersection_size"], "2")
      self.assertEqual(bounds[1]["newly_excluded_size"], "0")

  def test_capture_printed_upper_bound_and_reject_truncation(self) -> None:
    with tempfile.TemporaryDirectory() as directory:
      path = Path(directory) / "upper.pairs"
      output = (
          "mutual-refinement upper bound: 2\nTime (ms): 1\n"
          "1 2\n3 4\nprocess peak RSS (bytes): 1024\n"
      )
      run_rq3.capture_upper_pairs(output.splitlines(), path)
      self.assertEqual(aggregate_rq3.load_pairs(path), {(1, 2), (3, 4)})
      with self.assertRaisesRegex(ValueError, "captured 1"):
        run_rq3.capture_upper_pairs(output.replace("3 4\n", "").splitlines(), path)
      run_rq3.capture_upper_pairs(["regularization: 0", "Time (ms): 0"], path)
      self.assertEqual(aggregate_rq3.load_pairs(path), set())

  def test_measurement_summary_rejects_missing_and_changed_samples(self) -> None:
    row = self.make_row({"name": "regularization", "tool": "staged"}, [])
    self.assertTrue(run_rq3.valid_measurements(row))
    for field, value in (("time_ms", "[null]"), ("median_time_ms", "2"),
                         ("peak_rss_bytes", "[]"), ("stats_per_run", "[]")):
      with self.subTest(field=field):
        self.assertFalse(run_rq3.valid_measurements({**row, field: value}))

  def test_exit_zero_without_required_statistics_is_not_success(self) -> None:
    with tempfile.TemporaryDirectory() as directory:
      result = run_rq3.run_once(
          [sys.executable, "-c", "print('regularization: 0')"],
          Path(directory) / "run.log",
      )
      self.assertEqual(result.status, "stats-error")
      self.assertIn("Status: stats-error", result.log_path.read_text())

  def test_sigkill_alone_does_not_prove_memory_exhaustion(self) -> None:
    self.assertFalse(run_rq3.is_memory_failure("", -signal.SIGKILL))
    self.assertTrue(run_rq3.is_memory_failure("std::bad_alloc", 1))

  def test_failed_attempt_keeps_wall_time_without_partial_medians(self) -> None:
    experiment = next(item for item in config.EXPERIMENTS if item["name"] == "acf")
    benchmark = run_rq3.Benchmark(
        "test", "taint", config.REPOSITORY_ROOT, config.REPOSITORY_ROOT / "input.dot"
    )
    results = [
        run_rq3.RunResult("success", 0, 0.1, "", "",
                         {"Time (ms)": "1", "process peak RSS (bytes)": "100"},
                         Path("/tmp/success.log")),
        run_rq3.RunResult("timeout", -9, 30.0, "", "", {}, Path("/tmp/timeout.log")),
    ]
    with mock.patch.object(config, "MEASURED_REPETITIONS", 2):
      row = run_rq3.create_row(benchmark, experiment, results, [], None, {})
    self.assertEqual(json.loads(row["wall_time_seconds"]), [0.1, 30.0])
    self.assertEqual(json.loads(row["time_ms"]), [1, None])
    self.assertEqual(json.loads(row["peak_rss_bytes"]), [100, None])
    self.assertIsNone(row["median_wall_time_seconds"])
    self.assertIsNone(row["median_time_ms"])
    self.assertEqual(row["capture_status"], "not-run")

  def test_capture_is_separate_and_artifact_errors_are_logged(self) -> None:
    experiment = next(item for item in config.EXPERIMENTS if item["name"] == "regularization")
    benchmark = run_rq3.Benchmark(
        "test", "taint", config.REPOSITORY_ROOT, config.REPOSITORY_ROOT / "input.dot"
    )
    for truncated in (False, True):
      with self.subTest(truncated=truncated), tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        calls = []

        def fake_run(command, log_path, **kwargs):
          calls.append(command)
          stdout = "regularization: 1\nTime (ms): 1\nprocess peak RSS (bytes): 100\n"
          if "--print-result" in command:
            output = stdout.replace("Time (ms)", "1 2\nTime (ms)")
            if truncated:
              output = output.replace("regularization: 1", "regularization: 2")
            Path(command[command.index("-o") + 1]).write_text(output)
            stdout = ""
          return run_rq3.RunResult(
              "success", 0, 0.1, stdout, "", run_rq3.parse_stats(stdout), log_path
          )

        with (
            mock.patch.object(config, "MEASURED_REPETITIONS", 2),
            mock.patch.object(config, "ARTIFACT_DIRECTORY", root / "artifacts"),
            mock.patch.object(config, "LOG_DIRECTORY", root / "log"),
            mock.patch.object(run_rq3, "run_once", side_effect=fake_run),
        ):
          results, files, capture = run_rq3.run_experiment(benchmark, experiment)
          row = run_rq3.create_row(benchmark, experiment, results, files, capture, {})
        self.assertEqual(len(results), 2)
        self.assertEqual(len(calls), 3)
        self.assertTrue(all("--print-result" not in command for command in calls[:2]))
        self.assertIn("--print-result", calls[2])
        expected = "artifact-error" if truncated else "success"
        self.assertEqual(capture.status, expected)
        self.assertEqual(row["capture_status"], expected)
        self.assertIn(f"Status: {expected}", capture.log_path.read_text())
        self.assertEqual(len(files), 0 if truncated else 1)

  def test_capture_failure_does_not_invalidate_performance(self) -> None:
    row = self.make_row({"name": "regularization", "tool": "staged"}, [])
    row["capture_status"] = "timeout"
    with mock.patch.object(config, "MEASURED_REPETITIONS", 1):
      self.assertTrue(aggregate_rq3.measurement_successful(row))
      self.assertEqual(aggregate_rq3.method_status(row), "success")
      self.assertFalse(aggregate_rq3.successful(row))
      with mock.patch.object(config, "TIMEOUT_SECONDS", config.TIMEOUT_SECONDS + 1):
        self.assertFalse(aggregate_rq3.measurement_successful(row))
        self.assertEqual(aggregate_rq3.method_status(row), "protocol-mismatch")

  def test_selected_resume_preserves_unselected_failed_records(self) -> None:
    experiment = {"name": "regularization", "tool": "staged", "artifact_kind": "upper"}
    selected = self.make_row(experiment, [], statuses=["timeout"])
    other = {**selected, "dot_path": "other.dot", "dot_name": "other.dot"}
    expected = {("test", "input.dot", "regularization"): {"input_sha256": selected["input_sha256"]}}
    with tempfile.TemporaryDirectory() as directory:
      path = Path(directory) / "runs.csv"
      aggregate_rq3.write_rows(path, run_rq3.CSV_COLUMNS, [selected, other])
      with mock.patch.object(config, "EXPERIMENTS", [experiment]):
        self.assertEqual(run_rq3.prepare_csv(path, True, expected), set())
      with path.open(newline="") as stream:
        retained = list(csv.DictReader(stream))
      self.assertEqual(retained, [other])

  def test_resume_rejects_changed_execution_identity(self) -> None:
    row = self.make_row({"name": "regularization", "tool": "staged"}, [])
    key = ("test", "input.dot", "regularization")
    with tempfile.TemporaryDirectory() as directory:
      path = Path(directory) / "runs.csv"
      aggregate_rq3.write_rows(path, run_rq3.CSV_COLUMNS, [row])
      for field in ("input_sha256", "binary_sha256", "analysis", "command", "extra_environment"):
        with self.subTest(field=field), self.assertRaisesRegex(RuntimeError, "changed"):
          run_rq3.prepare_csv(path, True, {key: {field: "changed"}})

  def test_aggregation_rejects_mixed_inputs_binaries_and_clients(self) -> None:
    row = self.make_row({"name": "regularization", "tool": "staged"}, [])
    other = {**row, "experiment": "acf"}
    aggregate_rq3.validate_run_identities([row, other])
    for field, value in (("input_sha256", "3" * 64), ("binary_sha256", "3" * 64),
                         ("analysis", "taint"), ("extra_environment", '{"X":"1"}')):
      with self.subTest(field=field), self.assertRaisesRegex(ValueError, "mix"):
        aggregate_rq3.validate_run_identities([row, {**other, field: value}])

  def test_timeout_budget_includes_process_launch(self) -> None:
    process = mock.Mock()
    process.communicate.return_value = ("Time (ms): 1\nprocess peak RSS (bytes): 100\n", "")
    process.returncode = 0
    with (
        tempfile.TemporaryDirectory() as directory,
        mock.patch.object(config, "TIMEOUT_SECONDS", 1),
        mock.patch.object(run_rq3.subprocess, "Popen", return_value=process),
        mock.patch.object(run_rq3.time, "perf_counter", side_effect=[10.0, 10.4, 10.8]),
    ):
      result = run_rq3.run_once(["solver"], Path(directory) / "run.log")
    self.assertAlmostEqual(process.communicate.call_args.kwargs["timeout"], 0.6)
    self.assertAlmostEqual(result.wall_time_seconds, 0.8)
    self.assertEqual(result.status, "success")

  def test_timeout_remains_timeout_if_process_exits_before_kill(self) -> None:
    process = mock.Mock()
    process.communicate.side_effect = [subprocess.TimeoutExpired("solver", 1), ("", "")]
    process.returncode = -9
    with (
        tempfile.TemporaryDirectory() as directory,
        mock.patch.object(run_rq3.subprocess, "Popen", return_value=process),
        mock.patch.object(run_rq3.os, "killpg", side_effect=ProcessLookupError),
    ):
      result = run_rq3.run_once(["solver"], Path(directory) / "run.log")
    self.assertEqual(result.status, "timeout")

  def test_relation_hash_is_verified(self) -> None:
    experiment = {
        "name": "union-dyck",
        "tool": "staged",
        "artifact_kind": "union",
    }
    with tempfile.TemporaryDirectory() as directory:
      path = Path(directory) / "union.pairs"
      path.write_text("1 2\n", encoding="utf-8")
      row = self.make_row(experiment, [path])
      path.write_text("2 3\n", encoding="utf-8")
      with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
        aggregate_rq3.relation_path(row, "union.pairs")


if __name__ == "__main__":
  unittest.main()
