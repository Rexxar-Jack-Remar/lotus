from __future__ import annotations

from typing import Protocol

from experiment.spec import RunSpec
from result.run_result import RunResult


class Executor(Protocol):
  def execute(self, command: list[str], spec: RunSpec) -> RunResult: ...
