from importlib.util import find_spec
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "src"))

from experiment.loader import load_experiment
from experiment.loader import _load_preprocessing


@unittest.skipUnless(find_spec("yaml"), "PyYAML is an optional Phoenix runtime dependency")
class LoaderTest(unittest.TestCase):
  def test_loads_relative_paths_and_memory_gb(self) -> None:
    with tempfile.TemporaryDirectory() as temporary_directory:
      directory = Path(temporary_directory)
      (directory / "benchmarks").mkdir()
      (directory / "experiment.yaml").write_text(
          """name: demo
benchmarks:
  root: benchmarks
analyzers:
  - name: sparrow
    configs:
      - andersen-k-cs: 1
execution:
  repetitions: 2
  memory_limit_gb: 2
""",
          encoding="utf-8",
      )
      experiment = load_experiment(directory / "experiment.yaml")

    self.assertEqual(experiment.benchmarks.root, (directory / "benchmarks").resolve())
    self.assertEqual(experiment.execution.memory_limit_bytes, 2 * 1024**3)
    self.assertEqual(experiment.execution.repetitions, 2)


class PreprocessingSyntaxTest(unittest.TestCase):
  def test_accepts_placeholders_embedded_in_arguments(self) -> None:
    steps = _load_preprocessing([
        {"name": "opt", "command": ["opt", "{input}", "-o={output}"]},
    ])
    self.assertEqual(steps[0].name, "opt")
