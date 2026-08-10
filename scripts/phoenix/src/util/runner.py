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
        time_limit_sec: int | None = None,
        memory_limit_bytes: int | None = None,
    ) -> None:
        self.cmd: str = cmd
        self.stderr_path: str = stderr_path
        self.time_limit_sec = time_limit_sec or self.__class__.TIME_LIMIT
        self.memory_limit_bytes = memory_limit_bytes or self.__class__.MEMORY_LIMIT
        self.elapsed_time: float | str = "error"
        self.memory_usage: int | str = "error"
        self.return_code: int | None = None
        self.status: str = "crash"

    def set_limits(self) -> None:
        resource.setrlimit(resource.RLIMIT_CPU, (self.time_limit_sec, self.time_limit_sec))
        resource.setrlimit(
            resource.RLIMIT_AS,
            (self.memory_limit_bytes, self.memory_limit_bytes),
        )

    def __extract_max_resident_size(self) -> int | None:
        """Extracts the 'Maximum resident set size (kbytes)' from the specified file."""
        pattern: str = r"Maximum resident set size \(kbytes\):\s*(\d+)"
        try:
            with open(self.stderr_path, "r", encoding="utf-8", errors="replace") as file:
                for line in file:
                    match = re.search(pattern, line)
                    if match:
                        return int(match.group(1))
        except OSError:
            return None
        return None

    def __stderr_indicates_oom(self) -> bool:
        try:
            with open(self.stderr_path, "r", encoding="utf-8", errors="replace") as file:
                stderr = file.read().lower()
        except OSError:
            return False
        return any(marker in stderr for marker in ("out of memory", "cannot allocate memory", "bad_alloc"))

    def __classify_status(self) -> str:
        if self.return_code == 0:
            return "success"
        # With shell=True, a signal from the child is conventionally represented
        # as 128 + signal number.  Support both forms until command-list based
        # execution replaces the legacy shell runner.
        timeout_codes = {-signal.SIGXCPU, 128 + signal.SIGXCPU}
        killed_codes = {-signal.SIGKILL, 128 + signal.SIGKILL}
        if self.return_code in timeout_codes:
            return "timeout"
        if self.return_code in killed_codes:
            if isinstance(self.elapsed_time, float) and self.elapsed_time >= self.time_limit_sec * 0.95:
                return "timeout"
            return "oom"
        if self.__stderr_indicates_oom():
            return "oom"
        return "crash"

    def run_cmd(self) -> None:
        self.result = None
        try:
            begin_time: float = time.time()
            result = subprocess.run(
                self.cmd,
                shell=True,
                preexec_fn=self.set_limits,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            end_time: float = time.time()
            self.elapsed_time = end_time - begin_time
            self.return_code = result.returncode
            memory_usage = self.__extract_max_resident_size()
            if memory_usage is not None:
                self.memory_usage = memory_usage
            self.status = self.__classify_status()
            if self.status != "success":
                colored_write_line(
                    text=f"{self.cmd}: {self.status.upper()} (exit code {self.return_code})",
                    color_code=RED,
                )
        except Exception as e:
            colored_write_line(text=f"{self.cmd}: FAILED due to error: {e}", color_code=RED)
            self.status = "crash"
