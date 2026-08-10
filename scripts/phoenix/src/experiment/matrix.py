"""Expansion of declarative analyzer configuration matrices."""

from __future__ import annotations

from itertools import product
from typing import Any, Mapping

from .errors import ExperimentConfigError


def expand_configurations(
    configs: list[Mapping[str, Any]] | None,
    matrix: Mapping[str, Any] | None,
) -> tuple[Mapping[str, Any], ...]:
  """Merge base configs with the Cartesian product specified by ``matrix``."""
  base = [dict(config) for config in (configs or [{}])]
  if matrix is None:
    return tuple(base)
  if not isinstance(matrix, Mapping) or not matrix:
    raise ExperimentConfigError("analyzer matrix must be a non-empty mapping")
  keys: list[str] = []
  values: list[list[Any]] = []
  for key, candidates in matrix.items():
    if not isinstance(key, str) or not key:
      raise ExperimentConfigError("analyzer matrix keys must be non-empty strings")
    if not isinstance(candidates, list) or not candidates:
      raise ExperimentConfigError(f"matrix value for {key} must be a non-empty list")
    keys.append(key)
    values.append(candidates)
  combinations = [dict(zip(keys, combination)) for combination in product(*values)]
  return tuple({**configuration, **combination} for configuration in base for combination in combinations)
