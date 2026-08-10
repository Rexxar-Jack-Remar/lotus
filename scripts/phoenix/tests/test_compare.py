import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "src"))

from result.compare import compare_results


def _record(seconds: float) -> dict:
  return {
      "benchmark": "/bench/gzip.bc", "benchmark_sha256": "gzip", "analyzer": "sparrow",
      "configuration": {}, "preprocessing": [], "status": "success", "wall_time_sec": seconds,
      "peak_rss_bytes": 100, "repetition": 0,
  }


class CompareTest(unittest.TestCase):
  def test_reports_geomean_and_regression(self) -> None:
    with tempfile.TemporaryDirectory() as temporary_directory:
      root = Path(temporary_directory)
      baseline, new = root / "old", root / "new"
      baseline.mkdir()
      new.mkdir()
      (baseline / "results.jsonl").write_text(json.dumps(_record(10)) + "\n", encoding="utf-8")
      (new / "results.jsonl").write_text(json.dumps(_record(12)) + "\n", encoding="utf-8")
      manifest = {"benchmark_fingerprint": "same", "preprocessing_fingerprint": "same"}
      (baseline / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
      (new / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
      comparison = compare_results(baseline, new)

    self.assertAlmostEqual(comparison["geomean_speedup"], 10 / 12)
    self.assertEqual(len(comparison["regressions"]), 1)

  def test_matches_each_matrix_configuration(self) -> None:
    with tempfile.TemporaryDirectory() as temporary_directory:
      root = Path(temporary_directory)
      baseline, new = root / "old", root / "new"
      baseline.mkdir()
      new.mkdir()
      first = _record(10)
      second = {**_record(20), "configuration": {"andersen-k-cs": 1}}
      (baseline / "results.jsonl").write_text(
          "\n".join((json.dumps(first), json.dumps(second))) + "\n", encoding="utf-8"
      )
      (new / "results.jsonl").write_text(
          "\n".join((json.dumps({**first, "wall_time_sec": 9}), json.dumps({**second, "wall_time_sec": 18}))) + "\n",
          encoding="utf-8",
      )
      comparison = compare_results(baseline, new)

    self.assertEqual(comparison["benchmarks"], 2)
    self.assertAlmostEqual(comparison["geomean_speedup"], 10 / 9)

  def test_does_not_merge_equal_content_from_distinct_benchmarks(self) -> None:
    with tempfile.TemporaryDirectory() as temporary_directory:
      root = Path(temporary_directory)
      baseline, new = root / "old", root / "new"
      baseline.mkdir()
      new.mkdir()
      first = {**_record(10), "benchmark": "/bench/a.bc", "benchmark_id": "a.bc", "benchmark_sha256": "same"}
      second = {**_record(20), "benchmark": "/bench/b.bc", "benchmark_id": "b.bc", "benchmark_sha256": "same"}
      (baseline / "results.jsonl").write_text(
          "\n".join((json.dumps(first), json.dumps(second))) + "\n", encoding="utf-8"
      )
      (new / "results.jsonl").write_text(
          "\n".join((json.dumps({**first, "wall_time_sec": 9}), json.dumps({**second, "wall_time_sec": 18}))) + "\n",
          encoding="utf-8",
      )
      comparison = compare_results(baseline, new)

    self.assertEqual(comparison["benchmarks"], 2)
