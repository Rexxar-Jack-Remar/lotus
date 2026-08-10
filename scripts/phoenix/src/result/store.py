"""Append-only JSONL persistence for raw run outcomes."""

from __future__ import annotations

import json
from pathlib import Path

from .run_result import RunResult


class JsonlResultStore:
  """The source of truth for raw Phoenix experiment data."""

  def __init__(self, output_dir: str | Path) -> None:
    self.output_dir = Path(output_dir).resolve()
    self.output_dir.mkdir(parents=True, exist_ok=True)
    self.path = self.output_dir / "results.jsonl"

  def append(self, result: RunResult) -> None:
    with self.path.open("a", encoding="utf-8") as handle:
      json.dump(result.to_dict(), handle, sort_keys=True)
      handle.write("\n")
      handle.flush()
