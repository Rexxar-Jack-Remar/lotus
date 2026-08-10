from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "src"))

from execution.docker_executor import _translate_command
from execution.local_executor import LocalExecutor, _signal_number
from experiment.spec import RunSpec
from result.run_result import RunStatus


class LocalExecutorTest(unittest.TestCase):
  def test_records_success_and_crash_separately(self) -> None:
    with tempfile.TemporaryDirectory() as temporary_directory:
      input_path = Path(temporary_directory) / "input.bc"
      input_path.write_bytes(b"bitcode")
      executor = LocalExecutor(temporary_directory)
      success = executor.execute(
          ["/usr/bin/env", "true"],
          RunSpec("demo", input_path, "test", {}, 0, 10, None),
      )
      failure = executor.execute(
          ["/usr/bin/env", "false"],
          RunSpec("demo", input_path, "test", {}, 1, 10, None),
      )

    self.assertEqual(success.status, RunStatus.SUCCESS)
    self.assertEqual(failure.status, RunStatus.CRASH)
    self.assertTrue(success.stdout_path.is_absolute())

  def test_decodes_wrapped_signal_and_translates_docker_paths(self) -> None:
    self.assertEqual(_signal_number(137), 9)
    translated = _translate_command(
        ["tool", "/host/bench/input.bc", "/host/other"],
        {"/host/bench": "/container/bench"},
    )
    self.assertEqual(translated, ["tool", "/container/bench/input.bc", "/host/other"])
