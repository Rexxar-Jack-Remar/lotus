"""Interactive Slurm executor using srun; results are still collected locally."""

from __future__ import annotations

from pathlib import Path

from experiment.spec import RunSpec
from result.run_result import RunResult

from .local_executor import LocalExecutor


class SlurmExecutor(LocalExecutor):
  def __init__(self, output_dir: str | Path, settings: dict) -> None:
    super().__init__(output_dir)
    self.settings = settings

  def execute(self, command: list[str], spec: RunSpec) -> RunResult:
    slurm_command = ["srun", "--quiet"]
    partition = self.settings.get("partition")
    if isinstance(partition, str) and partition:
      slurm_command.extend(["--partition", partition])
    if spec.timeout_sec is not None:
      minutes = max(1, (spec.timeout_sec + 59) // 60)
      slurm_command.append(f"--time={minutes}")
    if spec.memory_limit_bytes is not None:
      slurm_command.append(f"--mem={max(1, spec.memory_limit_bytes // (1024 * 1024))}M")
    slurm_command.extend(command)
    return super().execute(slurm_command, spec)
