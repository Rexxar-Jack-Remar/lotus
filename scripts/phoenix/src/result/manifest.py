"""Reproducibility metadata saved beside every experiment result set."""

from __future__ import annotations

from datetime import datetime, timezone
from hashlib import sha256
import json
import platform
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any, Iterable

from experiment.spec import ExperimentSpec, RunSpec


def write_manifest(output_dir: str | Path, experiment: ExperimentSpec, runs: Iterable[RunSpec], commands: Iterable[list[str]]) -> Path:
  run_list = list(runs)
  command_list = list(commands)
  benchmarks = sorted({
      (run.benchmark_id or str(run.input_benchmark or run.benchmark), run.benchmark_sha256,
       str(run.input_benchmark or run.benchmark))
      for run in run_list
  })
  executables = sorted({command[0] for command in command_list if command})
  data: dict[str, Any] = {
      "experiment": experiment.name,
      "created_at": datetime.now(timezone.utc).isoformat(),
      "lotus_git_commit": _git_commit(),
      "hostname": platform.node(), "platform": platform.platform(),
      "python_version": sys.version,
      "llvm_version": _command_version(["llvm-config", "--version"]),
      "experiment_sha256": _hash_file(experiment.source_path),
      "benchmarks": [
          {"id": benchmark_id, "path": path, "sha256": digest}
          for benchmark_id, digest, path in benchmarks
      ],
      # Paths are useful provenance but are intentionally excluded from the
      # compatibility key: two hosts commonly stage identical bitcode in
      # different directories.
      "benchmark_fingerprint": _fingerprint([
          {"id": benchmark_id, "sha256": digest}
          for benchmark_id, digest, _ in benchmarks
      ]),
      "fingerprint_files_fingerprint": _fingerprint([
          _hash_file(path) for path in experiment.fingerprint_files
      ]),
      "preprocessing": [step.name for step in experiment.preprocessing],
      "preprocessing_fingerprint": _fingerprint([step.command for step in experiment.preprocessing]),
      "fingerprint_files": [
          {"path": str(path), "sha256": _hash_file(path)} for path in experiment.fingerprint_files
      ],
      "executables": [_executable_metadata(executable) for executable in executables],
  }
  path = Path(output_dir) / "manifest.json"
  path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
  return path


def _git_commit() -> str | None:
  try:
    return subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
  except (OSError, subprocess.CalledProcessError):
    return None


def _command_version(command: list[str]) -> str | None:
  try:
    return subprocess.check_output(command, text=True, stderr=subprocess.DEVNULL).strip()
  except (OSError, subprocess.CalledProcessError):
    return None


def _executable_metadata(command: str) -> dict[str, str | None]:
  resolved = shutil.which(command) or (command if Path(command).is_file() else None)
  return {"command": command, "path": resolved, "sha256": _hash_file(Path(resolved)) if resolved else None}


def _hash_file(path: Path | None) -> str | None:
  if path is None or not path.is_file():
    return None
  digest = sha256()
  with path.open("rb") as handle:
    for chunk in iter(lambda: handle.read(1024 * 1024), b""):
      digest.update(chunk)
  return digest.hexdigest()


def _fingerprint(value: Any) -> str:
  return sha256(json.dumps(value, sort_keys=True, default=str).encode("utf-8")).hexdigest()
