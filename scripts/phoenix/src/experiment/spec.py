"""Data structures describing a Phoenix experiment.

The structures in this module deliberately contain no execution behaviour.  A
``RunSpec`` is the complete, serialisable description of one analyzer process
invocation.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from hashlib import sha256
import json
from pathlib import Path
from typing import Any, Mapping


@dataclass(frozen=True)
class BenchmarkSpec:
  root: Path
  include: tuple[str, ...] = ("**/*.bc",)


@dataclass(frozen=True)
class AnalyzerSpec:
  name: str
  configurations: tuple[Mapping[str, Any], ...] = field(default_factory=lambda: ({},))


@dataclass(frozen=True)
class ExecutionSpec:
  repetitions: int = 1
  timeout_sec: int | None = None
  memory_limit_bytes: int | None = None
  executor: Mapping[str, Any] = field(default_factory=lambda: {"type": "local"})


@dataclass(frozen=True)
class PreprocessingStep:
  name: str
  command: tuple[str, ...]


@dataclass(frozen=True)
class ExperimentSpec:
  name: str
  benchmarks: BenchmarkSpec
  analyzers: tuple[AnalyzerSpec, ...]
  execution: ExecutionSpec = field(default_factory=ExecutionSpec)
  preprocessing: tuple[PreprocessingStep, ...] = ()
  fingerprint_files: tuple[Path, ...] = ()
  source_path: Path | None = None
  output_dir: Path | None = None


@dataclass(frozen=True)
class RunSpec:
  """One repetition of one analyzer configuration on one benchmark."""

  experiment: str
  benchmark: Path
  analyzer: str
  configuration: Mapping[str, Any]
  repetition: int
  timeout_sec: int | None
  memory_limit_bytes: int | None
  benchmark_sha256: str | None = None
  preprocessing: tuple[str, ...] = ()
  input_benchmark: Path | None = None
  benchmark_id: str | None = None

  def to_dict(self) -> dict[str, Any]:
    return {
        "experiment": self.experiment,
        "benchmark": str(self.benchmark),
        "analyzer": self.analyzer,
        "configuration": dict(self.configuration),
        "repetition": self.repetition,
        "timeout_sec": self.timeout_sec,
        "memory_limit_bytes": self.memory_limit_bytes,
        "benchmark_sha256": self.benchmark_sha256,
        "preprocessing": list(self.preprocessing),
        "input_benchmark": str(self.input_benchmark) if self.input_benchmark else None,
        "benchmark_id": self.benchmark_id,
    }

  @property
  def run_id(self) -> str:
    """A stable identifier within an experiment result directory."""
    encoded = json.dumps(self.to_dict(), sort_keys=True, separators=(",", ":"))
    return sha256(encoded.encode("utf-8")).hexdigest()[:16]
