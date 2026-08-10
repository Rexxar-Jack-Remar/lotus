"""Human-readable views generated from JSONL source data."""

from __future__ import annotations

import csv
import json
from pathlib import Path
from typing import Any

from .aggregate import aggregate, load_records


def export_csv(result_dir: str | Path, output_path: str | Path | None = None) -> Path:
  directory = Path(result_dir)
  output = Path(output_path) if output_path else directory / "summary.csv"
  rows = aggregate(load_records(directory))
  with output.open("w", newline="", encoding="utf-8") as handle:
    writer = csv.DictWriter(handle, fieldnames=[
        "benchmark", "analyzer", "configuration", "run_count", "success_count",
        "failed_count", "success_rate", "median_wall_time_sec", "mean_wall_time_sec",
        "stddev_wall_time_sec", "max_peak_rss_bytes", "statuses",
    ])
    writer.writeheader()
    for row in rows:
      writer.writerow({
          "benchmark": row["benchmark"], "analyzer": row["analyzer"],
          "configuration": json.dumps(row["configuration"], sort_keys=True),
          "run_count": row["run_count"], "success_count": row["success_count"],
          "failed_count": row["failed_count"], "success_rate": row["success_rate"],
          "median_wall_time_sec": row["wall_time_sec"]["median"],
          "mean_wall_time_sec": row["wall_time_sec"]["mean"],
          "stddev_wall_time_sec": row["wall_time_sec"]["stddev"],
          "max_peak_rss_bytes": row["peak_rss_bytes"]["max"],
          "statuses": json.dumps(row["statuses"], sort_keys=True),
      })
  return output


def markdown_report(result_dir: str | Path) -> str:
  rows = aggregate(load_records(result_dir))
  lines = [
      "# Phoenix experiment report", "",
      "| Benchmark | Analyzer | Success | Median time (s) | Max RSS (bytes) |",
      "| --- | --- | ---: | ---: | ---: |",
  ]
  for row in rows:
    lines.append(
        f"| {Path(str(row['benchmark'])).name} | {row['analyzer']} | "
        f"{row['success_count']}/{row['run_count']} | {row['wall_time_sec']['median']} | "
        f"{row['peak_rss_bytes']['max']} |"
    )
  return "\n".join(lines) + "\n"
