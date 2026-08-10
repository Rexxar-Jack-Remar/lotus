import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).parents[1] / "src"))

from main import run_experiment


class DeclarativeRunIntegrationTest(unittest.TestCase):
  def test_run_writes_jsonl_summary_and_manifest(self) -> None:
    with tempfile.TemporaryDirectory() as temporary_directory:
      root = Path(temporary_directory)
      benchmarks = root / "benchmarks"
      benchmarks.mkdir()
      (benchmarks / "input.bc").write_bytes(b"bitcode")
      experiment_path = root / "experiment.yaml"
      experiment_path.write_text("placeholder", encoding="utf-8")
      output = root / "results"
      document = {
          "name": "integration",
          "benchmarks": {"root": "benchmarks"},
          "analyzers": [{"name": "phoenix", "configs": [{"command": ["/usr/bin/env", "true"]}]}],
          "execution": {"repetitions": 1},
      }
      yaml = types.SimpleNamespace(safe_load=lambda _: document, YAMLError=ValueError)
      with mock.patch.dict(sys.modules, {"yaml": yaml}):
        result_dir = run_experiment(experiment_path, output)

      records = [json.loads(line) for line in (result_dir / "results.jsonl").read_text().splitlines()]
      self.assertEqual(records[0]["status"], "success")
      self.assertTrue((result_dir / "summary.json").is_file())
      manifest = json.loads((result_dir / "manifest.json").read_text())
      self.assertEqual(manifest["experiment"], "integration")
      self.assertEqual(manifest["benchmarks"][0]["path"], str((benchmarks / "input.bc").resolve()))
