from __future__ import annotations

from pathlib import Path
from typing import Mapping

from experiment.errors import ExperimentConfigError

from .docker_executor import DockerExecutor
from .local_executor import LocalExecutor
from .slurm_executor import SlurmExecutor


def create_executor(output_dir: str | Path, settings: Mapping[str, object]):
  executor_type = settings.get("type", "local")
  normalized = dict(settings)
  if executor_type == "local":
    return LocalExecutor(output_dir)
  if executor_type == "docker":
    return DockerExecutor(output_dir, normalized)
  if executor_type == "slurm":
    return SlurmExecutor(output_dir, normalized)
  raise ExperimentConfigError(f"Unsupported executor type: {executor_type}")
