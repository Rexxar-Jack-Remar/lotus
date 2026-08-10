"""Safe command-list local execution with resource limits and isolated logs."""

from __future__ import annotations

import os
from pathlib import Path
import platform
import re
import resource
import signal
import subprocess
from time import perf_counter

from experiment.spec import RunSpec
from result.run_result import RunResult, RunStatus


class LocalExecutor:
  def __init__(self, output_dir: str | Path) -> None:
    self.output_dir = Path(output_dir).resolve()
    self.logs_dir = self.output_dir / "logs"
    self.logs_dir.mkdir(parents=True, exist_ok=True)

  def execute(self, command: list[str], spec: RunSpec) -> RunResult:
    stem = spec.run_id
    stdout_path = self.logs_dir / f"{stem}.stdout.log"
    stderr_path = self.logs_dir / f"{stem}.stderr.log"
    time_path = self.logs_dir / f"{stem}.time.log"
    if not spec.benchmark.is_file():
      stderr_path.write_text(f"Invalid benchmark input: {spec.benchmark}\n", encoding="utf-8")
      stdout_path.touch()
      return RunResult(
          run_spec=spec, status=RunStatus.INVALID_INPUT, wall_time_sec=None,
          cpu_time_sec=None, peak_rss_bytes=None, exit_code=None, signal=None,
          stdout_path=stdout_path, stderr_path=stderr_path, command=tuple(command), metrics={},
      )
    # GNU time supplies per-process RSS and CPU statistics on Linux.  macOS
    # ships a different implementation, so execute directly there rather than
    # turning every valid run into a command-line error.
    timed_command = (
        ["/usr/bin/time", "-v", "-o", str(time_path), *command]
        if platform.system() == "Linux" else command
    )
    begin = perf_counter()
    timed_out = False
    return_code: int | None = None
    try:
      with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        process = subprocess.Popen(
            timed_command,
            stdin=subprocess.DEVNULL,
            stdout=stdout,
            stderr=stderr,
            start_new_session=True,
            preexec_fn=self._limit_function(spec),
        )
        try:
          process.wait(timeout=spec.timeout_sec)
        except subprocess.TimeoutExpired:
          timed_out = True
          os.killpg(process.pid, signal.SIGKILL)
          process.wait()
        return_code = process.returncode
    except (FileNotFoundError, subprocess.SubprocessError, OSError) as exc:
      # The command is invalid or /usr/bin/time is absent.  Preserve a result
      # record rather than aborting an entire benchmark suite.
      stderr_path.write_text(f"Phoenix could not start: {exc}\n", encoding="utf-8")
      return_code = 127 if isinstance(exc, FileNotFoundError) else 1
    wall_time = perf_counter() - begin
    time_metrics = _parse_time_file(time_path)
    status = _classify(return_code, timed_out, wall_time, spec.timeout_sec, stderr_path)
    return RunResult(
        run_spec=spec,
        status=status,
        wall_time_sec=wall_time,
        cpu_time_sec=time_metrics.get("cpu_time_sec"),
        peak_rss_bytes=time_metrics.get("peak_rss_bytes"),
        exit_code=return_code if return_code is not None and return_code >= 0 else None,
        signal=_signal_number(return_code),
        stdout_path=stdout_path,
        stderr_path=stderr_path,
        command=tuple(command),
        metrics={},
    )

  @staticmethod
  def _limit_function(spec: RunSpec):
    def apply_limits() -> None:
      if spec.timeout_sec is not None:
        # Keep a one-second hard-limit grace period so CPU exhaustion is
        # reported as SIGXCPU/TIMEOUT rather than indistinguishable SIGKILL.
        resource.setrlimit(resource.RLIMIT_CPU, (spec.timeout_sec, spec.timeout_sec + 1))
      if spec.memory_limit_bytes is not None:
        resource.setrlimit(resource.RLIMIT_AS, (spec.memory_limit_bytes, spec.memory_limit_bytes))
    return apply_limits


def _parse_time_file(path: Path) -> dict[str, float | int]:
  try:
    text = path.read_text(encoding="utf-8", errors="replace")
  except OSError:
    return {}
  user = _number(text, r"User time \(seconds\):\s*([0-9.]+)")
  system = _number(text, r"System time \(seconds\):\s*([0-9.]+)")
  rss = _number(text, r"Maximum resident set size \(kbytes\):\s*(\d+)")
  metrics: dict[str, float | int] = {}
  if user is not None and system is not None:
    metrics["cpu_time_sec"] = user + system
  if rss is not None:
    metrics["peak_rss_bytes"] = int(rss) * 1024
  return metrics


def _number(text: str, pattern: str) -> float | None:
  match = re.search(pattern, text)
  return float(match.group(1)) if match else None


def _classify(
    return_code: int | None,
    timed_out: bool,
    wall_time: float,
    timeout_sec: int | None,
    stderr_path: Path,
) -> RunStatus:
  if timed_out or return_code in (-signal.SIGXCPU, 128 + signal.SIGXCPU):
    return RunStatus.TIMEOUT
  if return_code == 0:
    return RunStatus.SUCCESS
  if return_code in (-signal.SIGKILL, 128 + signal.SIGKILL):
    if timeout_sec is not None and wall_time >= timeout_sec * 0.95:
      return RunStatus.TIMEOUT
    return RunStatus.OOM
  try:
    stderr = stderr_path.read_text(encoding="utf-8", errors="replace").lower()
  except OSError:
    stderr = ""
  if any(marker in stderr for marker in ("out of memory", "cannot allocate memory", "bad_alloc")):
    return RunStatus.OOM
  return RunStatus.CRASH


def _signal_number(return_code: int | None) -> int | None:
  if return_code is None:
    return None
  if return_code < 0:
    return -return_code
  candidate = return_code - 128
  return candidate if candidate in signal.valid_signals() else None
