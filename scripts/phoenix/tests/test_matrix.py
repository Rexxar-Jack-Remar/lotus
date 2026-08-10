from pathlib import Path
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).parents[1] / "src"))

from experiment.loader import expand_runs
from experiment.spec import AnalyzerSpec, BenchmarkSpec, ExecutionSpec, ExperimentSpec


class RunExpansionTest(unittest.TestCase):
  def test_expands_matrix(self) -> None:
    from experiment.matrix import expand_configurations

    configurations = expand_configurations(
        [{"args": ["--verbose"]}],
        {"andersen-k-cs": [0, 1], "enable-hcd": [True, False]},
    )
    self.assertEqual(len(configurations), 4)
    self.assertTrue(all(configuration["args"] == ["--verbose"] for configuration in configurations))

  def test_expands_benchmarks_configs_and_repetitions(self) -> None:
    with (mock.patch("experiment.loader._find_benchmarks", return_value=[Path("a.bc"), Path("b.bc")]),
          mock.patch("experiment.loader._sha256_file", return_value="hash")):
      experiment = ExperimentSpec(
          name="demo",
          benchmarks=BenchmarkSpec(Path("benchmarks")),
          analyzers=(AnalyzerSpec("sparrow", ({"andersen-k-cs": 0}, {"andersen-k-cs": 1})),),
          execution=ExecutionSpec(repetitions=3),
      )
      runs = expand_runs(experiment)

    self.assertEqual(len(runs), 12)
    self.assertEqual({run.repetition for run in runs}, {0, 1, 2})
    self.assertEqual({run.configuration["andersen-k-cs"] for run in runs}, {0, 1})
