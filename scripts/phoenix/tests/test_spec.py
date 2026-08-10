from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "src"))

from experiment.spec import RunSpec


class RunSpecTest(unittest.TestCase):
  def test_run_id_is_stable_and_accounts_for_repetition(self) -> None:
    common = dict(
        experiment="demo",
        benchmark=Path("gzip.bc"),
        analyzer="sparrow",
        configuration={"andersen-k-cs": 1},
        timeout_sec=60,
        memory_limit_bytes=1024,
    )
    first = RunSpec(repetition=0, **common)
    same = RunSpec(repetition=0, **common)
    second = RunSpec(repetition=1, **common)

    self.assertEqual(first.run_id, same.run_id)
    self.assertNotEqual(first.run_id, second.run_id)
    self.assertEqual(first.to_dict()["benchmark"], "gzip.bc")
