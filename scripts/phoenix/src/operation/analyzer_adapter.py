"""Adapters that isolate analyzer-specific command construction and parsing."""

from __future__ import annotations

import shlex
from typing import Any, Mapping, Protocol

from experiment.errors import ExperimentConfigError
from experiment.spec import RunSpec


class AnalyzerAdapter(Protocol):
  def build_command(self, spec: RunSpec) -> list[str]: ...

  def parse_metrics(self, stdout: str, stderr: str) -> dict[str, Any]: ...


class _OptionAnalyzer:
  executable = ""

  def build_command(self, spec: RunSpec) -> list[str]:
    return [self.executable, *_render_options(spec.analyzer, spec.configuration), str(spec.benchmark)]

  def parse_metrics(self, stdout: str, stderr: str) -> dict[str, Any]:
    return {}


class SparrowAnalyzerAdapter(_OptionAnalyzer):
  executable = "lotus-alias-sparrow-aa"


class AserAnalyzerAdapter(_OptionAnalyzer):
  executable = "lotus-alias-aser-aa"


class TPAAnalyzerAdapter(_OptionAnalyzer):
  executable = "lotus-alias-tpa"


class SVFAnalyzerAdapter(_OptionAnalyzer):
  executable = "wpa"


class PhoenixAnalyzerAdapter:
  def build_command(self, spec: RunSpec) -> list[str]:
    command = spec.configuration.get("command")
    if isinstance(command, str) and command.strip():
      command = shlex.split(command)
    if not isinstance(command, list) or not command or not all(isinstance(part, str) for part in command):
      raise ExperimentConfigError("phoenix configurations require command: [executable, ...]")
    return [*command, str(spec.benchmark)]

  def parse_metrics(self, stdout: str, stderr: str) -> dict[str, Any]:
    return {}


def create_analyzer_adapter(name: str) -> AnalyzerAdapter:
  adapters: dict[str, type[AnalyzerAdapter]] = {
      "phoenix": PhoenixAnalyzerAdapter,
      "sparrow": SparrowAnalyzerAdapter,
      "aser": AserAnalyzerAdapter,
      "tpa": TPAAnalyzerAdapter,
      "svf": SVFAnalyzerAdapter,
  }
  adapter = adapters.get(name.lower())
  if adapter is None:
    choices = ", ".join(sorted(adapters))
    raise ExperimentConfigError(f"Unsupported analyzer {name!r}; choose one of: {choices}")
  return adapter()


def _render_options(analyzer: str, configuration: Mapping[str, Any]) -> list[str]:
  arguments = configuration.get("args", [])
  if not isinstance(arguments, list) or not all(isinstance(argument, str) for argument in arguments):
    raise ExperimentConfigError(f"{analyzer}: args must be a list of strings")
  rendered = list(arguments)
  for key, value in configuration.items():
    if key == "args":
      continue
    if not isinstance(key, str) or not key:
      raise ExperimentConfigError(f"{analyzer}: configuration keys must be non-empty strings")
    option = key if key.startswith("-") else f"--{key}"
    if value is None or value is True:
      rendered.append(option)
    elif value is False:
      rendered.append(f"{option}=false")
    elif isinstance(value, (str, int, float)):
      rendered.append(f"{option}={value}")
    else:
      raise ExperimentConfigError(f"{analyzer}: {key} has an unsupported value type")
  return rendered
