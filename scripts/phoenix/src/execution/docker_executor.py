"""Docker-backed executor.  Host paths remain visible through explicit mounts."""

from __future__ import annotations

from pathlib import Path

from experiment.errors import ExperimentConfigError
from experiment.spec import RunSpec
from result.run_result import RunResult

from .local_executor import LocalExecutor


class DockerExecutor(LocalExecutor):
  def __init__(self, output_dir: str | Path, settings: dict) -> None:
    super().__init__(output_dir)
    image = settings.get("image")
    if not isinstance(image, str) or not image:
      raise ExperimentConfigError("docker executor requires execution.executor.image")
    self.image = image
    self.settings = settings

  def execute(self, command: list[str], spec: RunSpec) -> RunResult:
    mounts = {str(spec.benchmark.parent.resolve()): str(spec.benchmark.parent.resolve())}
    for mount in self.settings.get("mounts", []):
      if (not isinstance(mount, dict) or not isinstance(mount.get("host"), str)
          or not isinstance(mount.get("container", mount.get("host")), str)):
        raise ExperimentConfigError("docker executor mounts must contain host and optional container")
      host = str(Path(mount["host"]).expanduser().resolve())
      mounts[host] = mount.get("container", host)
    docker_command = ["docker", "run", "--rm", "--network", "none"]
    if spec.memory_limit_bytes is not None:
      docker_command.extend(["--memory", str(spec.memory_limit_bytes)])
    if spec.timeout_sec is not None:
      docker_command.extend(["--ulimit", f"cpu={spec.timeout_sec}"])
    for host, container in mounts.items():
      docker_command.extend(["-v", f"{host}:{container}:ro"])
    # Preprocessing artifacts are created by containerized commands and must
    # remain available to subsequent host-side result collection.
    docker_command.extend(["-v", f"{self.output_dir}:{self.output_dir}"])
    docker_command.extend([self.image, *_translate_command(command, mounts)])
    return super().execute(docker_command, spec)


def _translate_command(command: list[str], mounts: dict[str, str]) -> list[str]:
  """Rewrite host-path command arguments for explicitly remapped mounts."""
  translated: list[str] = []
  ordered_mounts = sorted(mounts.items(), key=lambda item: len(item[0]), reverse=True)
  for argument in command:
    replacement = argument
    for host, container in ordered_mounts:
      if argument == host:
        replacement = container
        break
      if argument.startswith(host + "/"):
        replacement = container + argument[len(host):]
        break
    translated.append(replacement)
  return translated
