"""Statistical summaries derived from raw JSONL records."""

from __future__ import annotations

import json
from math import sqrt
from pathlib import Path
from statistics import mean, median
from typing import Any, Iterable


def load_records(result_dir: str | Path) -> list[dict[str, Any]]:
  path = Path(result_dir) / "results.jsonl"
  if not path.is_file():
    raise FileNotFoundError(f"No results.jsonl in {Path(result_dir).resolve()}")
  records: list[dict[str, Any]] = []
  for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
    if not line.strip():
      continue
    try:
      record = json.loads(line)
    except json.JSONDecodeError as exc:
      raise ValueError(f"Invalid JSONL record at {path}:{line_number}") from exc
    if not isinstance(record, dict):
      raise ValueError(f"JSONL record at {path}:{line_number} is not an object")
    records.append(record)
  return records


def aggregate(records: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
  groups: dict[tuple[str, str, str, str], list[dict[str, Any]]] = {}
  for record in records:
    key = (
        str(record.get("benchmark_id") or record.get("benchmark_sha256") or record.get("benchmark")),
        str(record.get("analyzer")),
        json.dumps(record.get("configuration", {}), sort_keys=True),
        json.dumps(record.get("preprocessing", []), sort_keys=True),
    )
    groups.setdefault(key, []).append(record)
  return [_aggregate_group(group) for _, group in sorted(groups.items())]


def write_summary(result_dir: str | Path) -> dict[str, Any]:
  directory = Path(result_dir)
  summary = {"runs": aggregate(load_records(directory))}
  (directory / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
  return summary


def _aggregate_group(records: list[dict[str, Any]]) -> dict[str, Any]:
  first = records[0]
  successes = [record for record in records if record.get("status") == "success"]
  times = [float(record["wall_time_sec"]) for record in successes if record.get("wall_time_sec") is not None]
  memories = [int(record["peak_rss_bytes"]) for record in successes if record.get("peak_rss_bytes") is not None]
  return {
      "benchmark": first.get("input_benchmark") or first.get("benchmark"),
      "benchmark_sha256": first.get("benchmark_sha256"),
      "benchmark_id": first.get("benchmark_id"),
      "analyzer": first.get("analyzer"),
      "configuration": first.get("configuration", {}),
      "preprocessing": first.get("preprocessing", []),
      "run_count": len(records),
      "success_count": len(successes),
      "failed_count": len(records) - len(successes),
      "success_rate": len(successes) / len(records),
      "statuses": _status_counts(records),
      "wall_time_sec": _statistics(times),
      "peak_rss_bytes": {
          "max": max(memories) if memories else None,
          "median": median(memories) if memories else None,
      },
  }


def _status_counts(records: Iterable[dict[str, Any]]) -> dict[str, int]:
  counts: dict[str, int] = {}
  for record in records:
    status = str(record.get("status", "unknown"))
    counts[status] = counts.get(status, 0) + 1
  return counts


def _statistics(values: list[float]) -> dict[str, float | None]:
  if not values:
    return {"mean": None, "median": None, "stddev": None, "min": None, "max": None}
  average = mean(values)
  variance = sum((value - average) ** 2 for value in values) / len(values)
  return {
      "mean": average,
      "median": median(values),
      "stddev": sqrt(variance),
      "min": min(values),
      "max": max(values),
  }
