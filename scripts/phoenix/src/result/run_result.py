"""Per-repetition experiment outcomes."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any

from experiment.spec import RunSpec


class RunStatus(str, Enum):
  SUCCESS = "success"
  TIMEOUT = "timeout"
  OOM = "oom"
  CRASH = "crash"
  INVALID_INPUT = "invalid_input"
  PARSE_ERROR = "parse_error"


@dataclass(frozen=True)
class RunResult:
  run_spec: RunSpec
  status: RunStatus
  wall_time_sec: float | None
  cpu_time_sec: float | None
  peak_rss_bytes: int | None
  exit_code: int | None
  signal: int | None
  stdout_path: Path
  stderr_path: Path
  command: tuple[str, ...]
  metrics: dict[str, Any] = field(default_factory=dict)

  @property
  def run_id(self) -> str:
    return self.run_spec.run_id

  def to_dict(self) -> dict[str, Any]:
    result = self.run_spec.to_dict()
    result.update(
        {
            "run_id": self.run_id,
            "status": self.status.value,
            "wall_time_sec": self.wall_time_sec,
            "cpu_time_sec": self.cpu_time_sec,
            "peak_rss_bytes": self.peak_rss_bytes,
            "exit_code": self.exit_code,
            "signal": self.signal,
            "stdout_path": str(self.stdout_path),
            "stderr_path": str(self.stderr_path),
            "command": list(self.command),
            "metrics": self.metrics,
        }
    )
    return result
