import signal
import resource
import subprocess
import time
import re
from util.color import *


class Runner:

    TIME_LIMIT: int = 60 * 60 * 12  # 12h time limit for a single test
    MEMORY_LIMIT: int = 1024 * 1024 * 1024 * 128  # 128 GB memory limit for a single test
    SIGKILL = -signal.SIGKILL
    SIGXCPU = -signal.SIGXCPU

    def __init__(
        self,
        cmd: str,
        stderr_path: str,
    ) -> None:
        self.cmd: str = cmd
        self.stderr_path: str = stderr_path
        self.elapsed_time: float | str = "error"
        self.memory_usage: int | str = "error"

    @classmethod
    def set_limits(cls) -> None:
        resource.setrlimit(resource.RLIMIT_CPU, (cls.TIME_LIMIT, cls.TIME_LIMIT))
        resource.setrlimit(resource.RLIMIT_AS, (cls.MEMORY_LIMIT, cls.MEMORY_LIMIT))

    def __extract_max_resident_size(self) -> int:
        """Extracts the 'Maximum resident set size (kbytes)' from the specified file."""
        pattern: str = r"Maximum resident set size \(kbytes\):\s*(\d+)"
        result: int | None = None
        with open(self.stderr_path, "r") as file:
            for line in file:
                match = re.search(pattern, line)
                if match:
                    result = int(match.group(1))
                    break
        assert isinstance(result, int)
        return result

    def run_cmd(self) -> None:
        self.result = None
        try:
            # Run the command with time and memory limits
            begin_time: float = time.time()
            result = subprocess.run(
                self.cmd,
                shell=True,
                preexec_fn=Runner.set_limits,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            end_time: float = time.time()

            # Check if the command was killed by limits
            match result.returncode:
                case 0:
                    pass
                case Runner.SIGKILL:
                    raise MemoryError("MLE")
                case Runner.SIGXCPU:
                    raise TimeoutError("TLE")
                case _:
                    raise Exception

            # Calculate time and extract memory usage
            self.elapsed_time = end_time - begin_time
            self.memory_usage = self.__extract_max_resident_size()
            assert isinstance(self.elapsed_time, float)
            assert isinstance(self.memory_usage, int)

        except MemoryError:
            colored_write_line(text=f"{self.cmd}: Memory Limit Exceeded!", color_code=RED)
            self.elapsed_time = "MLE"
            self.memory_usage = "MLE"
        except TimeoutError:
            colored_write_line(text=f"{self.cmd}: Time Limit Exceeded!", color_code=RED)
            self.elapsed_time = "TLE"
            self.memory_usage = "TLE"
        except Exception as e:
            colored_write_line(text=f"{self.cmd}: FAILED due to error: {e}", color_code=RED)
            self.elapsed_time = "error"
            self.memory_usage = "error"