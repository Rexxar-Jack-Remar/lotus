"""Reproducible, optional bitcode preprocessing pipeline."""

from __future__ import annotations

from dataclasses import replace
from hashlib import sha256
from pathlib import Path
from typing import Mapping

from execution.executor import Executor
from result.run_result import RunStatus

from .errors import ExperimentConfigError
from .spec import ExperimentSpec, RunSpec


def apply_preprocessing(
    experiment: ExperimentSpec,
    runs: list[RunSpec],
    executor: Executor,
    artifacts_dir: str | Path,
) -> list[RunSpec]:
  if not experiment.preprocessing:
    return runs
  artifacts = Path(artifacts_dir)
  by_benchmark: dict[Path, Path] = {}
  for benchmark in {run.benchmark for run in runs}:
    current = benchmark
    for index, step in enumerate(experiment.preprocessing):
      benchmark_id = sha256(str(benchmark).encode("utf-8")).hexdigest()[:16]
      output = artifacts / benchmark_id / f"{index:02d}-{step.name}.bc"
      output.parent.mkdir(parents=True, exist_ok=True)
      command = [part.replace("{input}", str(current)).replace("{output}", str(output)) for part in step.command]
      preprocessing_spec = RunSpec(
          experiment=experiment.name, benchmark=current, analyzer=f"preprocess:{step.name}",
          configuration={"command": command}, repetition=0, timeout_sec=experiment.execution.timeout_sec,
          memory_limit_bytes=experiment.execution.memory_limit_bytes,
      )
      result = executor.execute(command, preprocessing_spec)
      if result.status != RunStatus.SUCCESS or not output.is_file():
        raise ExperimentConfigError(f"preprocessing step {step.name} failed for {benchmark}: {result.status.value}")
      current = output.resolve()
    by_benchmark[benchmark] = current
  return [replace(run, benchmark=by_benchmark[run.benchmark]) for run in runs]
