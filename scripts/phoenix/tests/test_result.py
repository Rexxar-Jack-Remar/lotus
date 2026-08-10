import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "src"))

from experiment.spec import RunSpec
from result.run_result import RunResult, RunStatus
from result.store import JsonlResultStore
from result.aggregate import aggregate


class ResultStoreTest(unittest.TestCase):
  def test_jsonl_keeps_a_failed_repetition(self) -> None:
    spec = RunSpec("demo", Path("gzip.bc"), "sparrow", {}, 2, 60, 1024)
    result = RunResult(
        run_spec=spec,
        status=RunStatus.CRASH,
        wall_time_sec=1.25,
        cpu_time_sec=None,
        peak_rss_bytes=4096,
        exit_code=1,
        signal=None,
        stdout_path=Path("stdout.log"),
        stderr_path=Path("stderr.log"),
        command=("lotus-alias-sparrow-aa", "gzip.bc"),
    )
    with tempfile.TemporaryDirectory() as temporary_directory:
      store = JsonlResultStore(temporary_directory)
      store.append(result)
      record = json.loads(store.path.read_text(encoding="utf-8"))

    self.assertEqual(record["status"], "crash")
    self.assertEqual(record["repetition"], 2)
    self.assertEqual(record["peak_rss_bytes"], 4096)

  def test_aggregation_keeps_original_benchmark_after_preprocessing(self) -> None:
    spec = RunSpec(
        "demo", Path("artifacts/canonical.bc"), "sparrow", {}, 0, 60, 1024,
        "source-hash", ("canonicalize",), Path("benchmarks/gzip.bc"),
    )
    result = RunResult(
        spec, RunStatus.SUCCESS, 1.0, None, None, 0, None,
        Path("stdout.log"), Path("stderr.log"), ("sparrow",),
    )

    summary = aggregate([result.to_dict()])

    self.assertEqual(summary[0]["benchmark"], "benchmarks/gzip.bc")
