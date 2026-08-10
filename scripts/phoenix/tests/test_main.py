from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "src"))

from experiment.spec import RunSpec
from operation.analyzer_adapter import PhoenixAnalyzerAdapter, _render_options


class CommandRenderingTest(unittest.TestCase):
  def test_renders_generic_analyzer_options(self) -> None:
    arguments = _render_options(
        "sparrow",
        {"args": ["--verbose"], "andersen-k-cs": 1, "enable-hcd": True, "enable-hvn": False},
    )
    self.assertEqual(
        arguments,
        ["--verbose", "--andersen-k-cs=1", "--enable-hcd", "--enable-hvn=false"],
    )

  def test_phoenix_requires_a_full_command(self) -> None:
    self.assertEqual(
        PhoenixAnalyzerAdapter().build_command(
            RunSpec("demo", Path("input.bc"), "phoenix", {"command": ["lotus-alias-tpa", "--k-limit=1"]}, 0, None, None)
        ),
        ["lotus-alias-tpa", "--k-limit=1", "input.bc"],
    )
