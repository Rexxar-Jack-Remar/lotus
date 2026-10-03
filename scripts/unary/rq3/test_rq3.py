#!/usr/bin/env python3
"""Regression tests for the RQ3 runner and aggregator."""

from __future__ import annotations

import csv
import json
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
            "analysis": "taint",
            "dot_name": "input.dot",
            "dot_path": "input.dot",
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
            "peak_rss_bytes": json_cell([100 if value == "success" else None for value in statuses]),
            "wall_time_seconds": json_cell([0.1 if value == "success" else None for value in statuses]),
            "timeout_seconds": str(config.TIMEOUT_SECONDS),
            "memory_limit_bytes": str(int(config.MEMORY_LIMIT_GIB * 1024**3)),
            "relation_files": json_cell(stored),
            "relation_sha256": json_cell(hashes),
            "log_files": "[]",
            "capture_command": "[]",
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

  def test_missing_circ_makes_aggregation_incomplete(self) -> None:
    experiments: list[dict[str, object]] = [
        {"name": "union-dyck", "tool": "staged", "artifact_kind": "union"},
        {"name": "staged-on-demand", "tool": "staged", "artifact_kind": "staged"},
        {"name": "acf", "tool": "unary", "artifact_kind": "components"},
        {
            "name": "mcfl-plus-d1",
            "tool": "mcfl",
            "artifact_kind": "mcfl",
            "family": "plus",
            "dimension": 1,
        },
        {
            "name": "mcfl-circ-d1",
            "tool": "mcfl",
            "artifact_kind": "mcfl",
            "family": "circ",
            "dimension": 1,
        },
    ]
    with tempfile.TemporaryDirectory() as directory:
      root = Path(directory)
      files: dict[str, list[Path]] = {}
      for name, names in {
          "union-dyck": ["union.pairs"],
          "staged-on-demand": ["union.pairs", "on-demand.pairs"],
          "acf": ["components.map"],
          "mcfl-plus-d1": ["g-plus-1.pairs"],
      }.items():
        base = root / name
        base.mkdir()
        files[name] = []
        for filename in names:
          path = base / filename
          contents = "1 2\n" if name == "mcfl-plus-d1" else ""
          path.write_text(contents, encoding="utf-8")
          files[name].append(path)
      rows = [self.make_row(item, files[str(item["name"])]) for item in experiments[:-1]]
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
      self.assertEqual(bounds[0]["mcfl_plus_size"], "1")

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
