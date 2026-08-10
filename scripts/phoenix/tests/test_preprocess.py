from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "src"))

from execution.local_executor import LocalExecutor
from experiment.preprocess import apply_preprocessing
from experiment.spec import BenchmarkSpec, ExecutionSpec, ExperimentSpec, PreprocessingStep, RunSpec


class PreprocessingTest(unittest.TestCase):
  def test_replaces_run_input_with_preprocessed_artifact(self) -> None:
    with tempfile.TemporaryDirectory() as temporary_directory:
      root = Path(temporary_directory)
      source = root / "input.bc"
      source.write_bytes(b"bitcode")
      experiment = ExperimentSpec(
          "demo", BenchmarkSpec(root), (), ExecutionSpec(),
          preprocessing=(PreprocessingStep("copy", ("/bin/cp", "{input}", "{output}")),),
      )
      run = RunSpec("demo", source, "sparrow", {}, 0, 10, None, "original")
      processed = apply_preprocessing(experiment, [run], LocalExecutor(root / "results"), root / "artifacts")

      self.assertNotEqual(processed[0].benchmark, source)
      self.assertTrue(processed[0].benchmark.is_file())
      self.assertEqual(processed[0].benchmark_sha256, "original")
