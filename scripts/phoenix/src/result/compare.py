"""Baseline/new experiment comparison with input compatibility checks."""

from __future__ import annotations

from math import exp, log
import json
from pathlib import Path
from typing import Any

from .aggregate import aggregate, load_records


def compare_results(baseline_dir: str | Path, new_dir: str | Path, threshold: float = 0.05) -> dict[str, Any]:
  warnings = _compatibility_warnings(Path(baseline_dir), Path(new_dir))
  baseline_rows = aggregate(load_records(baseline_dir))
  new_rows = aggregate(load_records(new_dir))
  baseline = _by_identity(baseline_rows)
  new = _by_identity(new_rows)
  common = sorted(set(baseline) & set(new))
  # Different analyzers/configurations are a legitimate comparison when each
  # result set has one configuration per benchmark.  Matrix experiments use
  # the exact identity matching above instead.
  if not common:
    baseline = _by_benchmark_unique(baseline_rows)
    new = _by_benchmark_unique(new_rows)
    common = sorted(set(baseline) & set(new))
  comparisons: list[dict[str, Any]] = []
  speedups: list[float] = []
  for key in common:
    old, current = baseline[key], new[key]
    old_time = old["wall_time_sec"]["median"]
    new_time = current["wall_time_sec"]["median"]
    speedup = old_time / new_time if old_time and new_time else None
    if speedup is not None:
      speedups.append(speedup)
    comparisons.append({
        "benchmark": current["benchmark"],
        "baseline_success": old["success_count"],
        "new_success": current["success_count"],
        "speedup": speedup,
        "time_change_percent": ((new_time - old_time) / old_time * 100) if old_time and new_time else None,
        "memory_change_percent": _percent_change(
            old["peak_rss_bytes"]["max"], current["peak_rss_bytes"]["max"]
        ),
        "regression": speedup is not None and speedup < 1 - threshold,
    })
  return {
      "baseline": str(Path(baseline_dir).resolve()), "new": str(Path(new_dir).resolve()),
      "warnings": warnings, "benchmarks": len(common),
      "geomean_speedup": exp(sum(log(value) for value in speedups) / len(speedups)) if speedups else None,
      "regressions": [row for row in comparisons if row["regression"]],
      "comparisons": comparisons,
      "baseline_only": len(set(baseline) - set(new)), "new_only": len(set(new) - set(baseline)),
  }


def format_comparison(comparison: dict[str, Any]) -> str:
  lines = ["# Phoenix comparison", "", f"Comparable benchmarks: {comparison['benchmarks']}"]
  if comparison["geomean_speedup"] is not None:
    lines.append(f"Geomean speedup: {comparison['geomean_speedup']:.3f}x")
  for warning in comparison["warnings"]:
    lines.append(f"Warning: {warning}")
  if comparison["regressions"]:
    lines.extend(["", "## Regressions"])
    for row in comparison["regressions"]:
      lines.append(f"- {Path(row['benchmark']).name}: {row['time_change_percent']:+.1f}% time")
  return "\n".join(lines) + "\n"


def _by_identity(rows: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
  result: dict[str, dict[str, Any]] = {}
  for row in rows:
    key = json.dumps({
        "benchmark": {
            "id": row.get("benchmark_id"),
            "sha256": row.get("benchmark_sha256"),
            "path": row["benchmark"],
        },
        "analyzer": row["analyzer"],
        "configuration": row["configuration"],
        "preprocessing": row.get("preprocessing", []),
    }, sort_keys=True)
    if key in result:
      raise ValueError("Duplicate analyzer/configuration summaries in compared result directory")
    result[key] = row
  return result


def _by_benchmark_unique(rows: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
  result: dict[str, dict[str, Any]] = {}
  for row in rows:
    key = json.dumps({
        "id": row.get("benchmark_id"),
        "sha256": row.get("benchmark_sha256"),
        "path": row["benchmark"] if not row.get("benchmark_id") and not row.get("benchmark_sha256") else None,
    }, sort_keys=True)
    if key in result:
      raise ValueError(
          "No analyzer/configuration identities match; compare requires one configuration per benchmark"
      )
    result[key] = row
  return result


def _compatibility_warnings(baseline: Path, new: Path) -> list[str]:
  warnings: list[str] = []
  old_manifest = _read_manifest(baseline)
  new_manifest = _read_manifest(new)
  if old_manifest is None or new_manifest is None:
    return ["missing manifest.json; benchmark compatibility could not be fully verified"]
  if old_manifest.get("benchmark_fingerprint") != new_manifest.get("benchmark_fingerprint"):
    warnings.append("incompatible experiment inputs: benchmark fingerprints differ")
  if old_manifest.get("preprocessing_fingerprint") != new_manifest.get("preprocessing_fingerprint"):
    warnings.append("incompatible experiment inputs: preprocessing pipelines differ")
  if old_manifest.get("fingerprint_files_fingerprint") != new_manifest.get("fingerprint_files_fingerprint"):
    warnings.append("incompatible experiment inputs: tracked fingerprint files differ")
  return warnings


def _read_manifest(directory: Path) -> dict[str, Any] | None:
  import json
  path = directory / "manifest.json"
  if not path.is_file():
    return None
  data = json.loads(path.read_text(encoding="utf-8"))
  return data if isinstance(data, dict) else None


def _percent_change(old: float | int | None, new: float | int | None) -> float | None:
  return (new - old) / old * 100 if old and new is not None else None
