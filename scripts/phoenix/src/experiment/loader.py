"""YAML experiment loading and expansion into individual runs."""

from __future__ import annotations

from pathlib import Path
from typing import Any, Mapping

from .errors import ExperimentConfigError
from .matrix import expand_configurations
from .spec import (
    AnalyzerSpec,
    BenchmarkSpec,
    ExecutionSpec,
    ExperimentSpec,
    PreprocessingStep,
    RunSpec,
)


def load_experiment(path: str | Path) -> ExperimentSpec:
  """Load a YAML experiment file.

  PyYAML is intentionally an explicit dependency instead of a hand-written
  partial YAML parser.  This avoids silently interpreting experiment input
  differently from ordinary YAML tooling.
  """
  try:
    import yaml
  except ImportError as exc:
    raise RuntimeError(
        "YAML experiments require PyYAML; install scripts/phoenix/requirements.txt"
    ) from exc

  source_path = Path(path).expanduser().resolve()
  if not source_path.is_file():
    raise ExperimentConfigError(f"Experiment file does not exist: {source_path}")

  try:
    with source_path.open("r", encoding="utf-8") as handle:
      document = yaml.safe_load(handle)
  except yaml.YAMLError as exc:
    raise ExperimentConfigError(f"Invalid YAML in {source_path}: {exc}") from exc
  if not isinstance(document, Mapping):
    raise ExperimentConfigError("Experiment root must be a YAML mapping")

  name = _required_string(document, "name")
  benchmarks = _load_benchmarks(document.get("benchmarks"), source_path.parent)
  analyzers = _load_analyzers(document.get("analyzers"))
  execution = _load_execution(document.get("execution", {}))
  preprocessing = _load_preprocessing(document.get("preprocessing", []))
  fingerprint_files = _load_fingerprint_files(document.get("fingerprint_files", []), source_path.parent)
  output_dir = _optional_path(document.get("output_dir"), source_path.parent)
  return ExperimentSpec(
      name=name,
      benchmarks=benchmarks,
      analyzers=analyzers,
      execution=execution,
      preprocessing=preprocessing,
      fingerprint_files=fingerprint_files,
      source_path=source_path,
      output_dir=output_dir,
  )


def expand_runs(spec: ExperimentSpec) -> list[RunSpec]:
  """Produce the benchmark × analyzer configuration × repetition product."""
  benchmarks = _find_benchmarks(spec.benchmarks)
  runs: list[RunSpec] = []
  for benchmark in benchmarks:
    try:
      benchmark_id = benchmark.relative_to(spec.benchmarks.root).as_posix()
    except ValueError:
      # Keep direct API use usable for callers that supply a pre-expanded
      # benchmark list rather than paths rooted at BenchmarkSpec.root.
      benchmark_id = benchmark.name
    for analyzer in spec.analyzers:
      for configuration in analyzer.configurations:
        for repetition in range(spec.execution.repetitions):
          runs.append(
              RunSpec(
                  experiment=spec.name,
                  benchmark=benchmark,
                  analyzer=analyzer.name,
                  configuration=configuration,
                  repetition=repetition,
                  timeout_sec=spec.execution.timeout_sec,
                  memory_limit_bytes=spec.execution.memory_limit_bytes,
                  benchmark_sha256=_sha256_file(benchmark),
                  preprocessing=tuple(step.name for step in spec.preprocessing),
                  input_benchmark=benchmark,
                  benchmark_id=benchmark_id,
              )
          )
  return runs


def _load_benchmarks(value: Any, base_dir: Path) -> BenchmarkSpec:
  if not isinstance(value, Mapping):
    raise ExperimentConfigError("benchmarks must be a mapping")
  root_value = _required_string(value, "root")
  root = (base_dir / root_value).resolve() if not Path(root_value).is_absolute() else Path(root_value)
  include_value = value.get("include", ["**/*.bc"])
  if not isinstance(include_value, list) or not all(isinstance(item, str) for item in include_value):
    raise ExperimentConfigError("benchmarks.include must be a list of glob strings")
  return BenchmarkSpec(root=root, include=tuple(include_value))


def _load_analyzers(value: Any) -> tuple[AnalyzerSpec, ...]:
  if not isinstance(value, list) or not value:
    raise ExperimentConfigError("analyzers must be a non-empty list")
  analyzers: list[AnalyzerSpec] = []
  for item in value:
    if not isinstance(item, Mapping):
      raise ExperimentConfigError("each analyzer must be a mapping")
    name = _required_string(item, "name")
    configurations_value = item.get("configs", item.get("configurations"))
    if configurations_value is not None and (
        not isinstance(configurations_value, list) or not configurations_value
    ):
      raise ExperimentConfigError(f"analyzer {name}: configs must be a non-empty list")
    configurations: list[Mapping[str, Any]] = []
    for configuration in configurations_value or [{}]:
      if not isinstance(configuration, Mapping):
        raise ExperimentConfigError(f"analyzer {name}: every config must be a mapping")
      configurations.append(dict(configuration))
    configurations = list(expand_configurations(configurations, item.get("matrix")))
    analyzers.append(AnalyzerSpec(name=name, configurations=tuple(configurations)))
  return tuple(analyzers)


def _load_execution(value: Any) -> ExecutionSpec:
  if not isinstance(value, Mapping):
    raise ExperimentConfigError("execution must be a mapping")
  repetitions = value.get("repetitions", 1)
  timeout_sec = value.get("timeout", value.get("timeout_sec"))
  memory_limit_bytes = value.get("memory_limit_bytes")
  memory_limit_gb = value.get("memory_limit_gb")
  executor = value.get("executor", {"type": "local"})
  if not isinstance(repetitions, int) or repetitions < 1:
    raise ExperimentConfigError("execution.repetitions must be a positive integer")
  if timeout_sec is not None and (not isinstance(timeout_sec, int) or timeout_sec < 1):
    raise ExperimentConfigError("execution.timeout must be a positive integer")
  if memory_limit_bytes is not None and (not isinstance(memory_limit_bytes, int) or memory_limit_bytes < 1):
    raise ExperimentConfigError("execution.memory_limit_bytes must be a positive integer")
  if memory_limit_gb is not None:
    if memory_limit_bytes is not None or not isinstance(memory_limit_gb, (int, float)) or memory_limit_gb <= 0:
      raise ExperimentConfigError(
          "execution.memory_limit_gb must be positive and cannot be combined with memory_limit_bytes"
      )
    memory_limit_bytes = int(memory_limit_gb * 1024**3)
  if not isinstance(executor, Mapping):
    raise ExperimentConfigError("execution.executor must be a mapping")
  executor = dict(executor)
  executor_type = executor.get("type", "local")
  if executor_type not in ("local", "docker", "slurm"):
    raise ExperimentConfigError("execution.executor.type must be local, docker, or slurm")
  return ExecutionSpec(
      repetitions=repetitions,
      timeout_sec=timeout_sec,
      memory_limit_bytes=memory_limit_bytes,
      executor=executor,
  )


def _load_preprocessing(value: Any) -> tuple[PreprocessingStep, ...]:
  if value is None:
    return ()
  if not isinstance(value, list):
    raise ExperimentConfigError("preprocessing must be a list")
  steps: list[PreprocessingStep] = []
  for item in value:
    if not isinstance(item, Mapping):
      raise ExperimentConfigError("each preprocessing step must be a mapping")
    name = _required_string(item, "name")
    command = item.get("command")
    if not isinstance(command, list) or not command or not all(isinstance(part, str) for part in command):
      raise ExperimentConfigError(f"preprocessing step {name}: command must be a non-empty string list")
    if (not any("{input}" in part for part in command)
        or not any("{output}" in part for part in command)):
      raise ExperimentConfigError(f"preprocessing step {name}: command must include {{input}} and {{output}}")
    steps.append(PreprocessingStep(name=name, command=tuple(command)))
  return tuple(steps)


def _load_fingerprint_files(value: Any, base_dir: Path) -> tuple[Path, ...]:
  if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
    raise ExperimentConfigError("fingerprint_files must be a list of paths")
  paths: list[Path] = []
  for item in value:
    path = Path(item).expanduser()
    path = (base_dir / path).resolve() if not path.is_absolute() else path
    if not path.is_file():
      raise ExperimentConfigError(f"fingerprint file does not exist: {path}")
    paths.append(path)
  return tuple(paths)


def _find_benchmarks(spec: BenchmarkSpec) -> list[Path]:
  if not spec.root.is_dir():
    raise ExperimentConfigError(f"Benchmark root does not exist: {spec.root}")
  paths = {path.resolve() for pattern in spec.include for path in spec.root.glob(pattern) if path.is_file()}
  if not paths:
    raise ExperimentConfigError(f"No benchmarks match {list(spec.include)} below {spec.root}")
  return sorted(paths)


def _required_string(document: Mapping[str, Any], key: str) -> str:
  value = document.get(key)
  if not isinstance(value, str) or not value.strip():
    raise ExperimentConfigError(f"{key} must be a non-empty string")
  return value


def _optional_path(value: Any, base_dir: Path) -> Path | None:
  if value is None:
    return None
  if not isinstance(value, str) or not value:
    raise ExperimentConfigError("output_dir must be a non-empty string")
  path = Path(value).expanduser()
  return (base_dir / path).resolve() if not path.is_absolute() else path


def _sha256_file(path: Path) -> str:
  from hashlib import sha256

  digest = sha256()
  with path.open("rb") as handle:
    for chunk in iter(lambda: handle.read(1024 * 1024), b""):
      digest.update(chunk)
  return digest.hexdigest()
