import os
import shlex
from pathlib import Path
from util.color import *
from util.runner import *
import config.config as config
from experiment.spec import RunSpec
from result.run_result import RunResult, RunStatus


class Analyzer:

    TOOL: str = ""

    CONFIGURATIONS: dict[str, str] = {}
    # OPTIONS: dict[str, str] = {
    #     "sparrow-aa --andersen-k-cs=0": GREEN,
    #     "aser-aa --analysis-mode=ci": YELLOW,
    #     "tpa --k-limit=0": MAGENTA,
    #     "wpa -ander": GREEN,
    #     "wpa -sander": YELLOW,
    #     "wpa -fspta": MAGENTA,
    # }

    OPTIMIZATIONS: list[str] = [
        "--enable-hcd",
        "--enable-hu",
        "--enable-hvn",
        "--enable-lcd",
    ]

    REPETITION_NUM: int = config.REPETITION_NUM

    def __init__(
        self,
        project_name: str,
        bc_file_path: str,
        configuration: str,
        mask: str | None,
    ) -> None:
        self.project_name: str = project_name
        self.bc_path: str = bc_file_path
        self.configuration: str = configuration
        self.mask: str | None = mask
        self.optimization: list[str] | None = self.__class__.mask2optimization(mask=mask)
        self.option: str = ""
        if self.mask is None:
            assert self.optimization is None
            self.option = self.__class__.TOOL + " " + configuration
        else:
            assert self.optimization is not None
            self.option = self.__class__.TOOL + " " + configuration + " " + " ".join(self.optimization)
        self.log_directory_path: str = "log"
        self.note: str = "note"
        self.time_list: list[float] = []
        self.average_time: float | None = None
        self.memory_list: list[int] = []
        self.max_memory: int | None = None
        self.run_results: list[RunResult] = []

    @staticmethod
    def init(project_name: str) -> None:
        log_directory_path: str = os.path.join(
            "log",
            project_name,
        )
        try:
            os.makedirs(log_directory_path, exist_ok=False)
        except FileExistsError as e:
            colored_write_line(f"Log directory [{log_directory_path}] has already exists!", RED)
            colored_write_line(str(e), RED)
            return

    @classmethod
    def mask2optimization(cls, mask: str | None) -> list[str] | None:
        if mask is not None:
            assert len(mask) == len(cls.OPTIMIZATIONS)
            optimization: list[str] = []
            for j in range(0, len(mask)):
                if mask[j] == "1":
                    optimization.append(cls.OPTIMIZATIONS[j])
            return optimization
        else:
            return None

    def analyze(self, run_specs: list[RunSpec] | None = None) -> list[RunResult]:
        """Run the supplied repetitions and retain every outcome.

        ``None`` preserves the legacy interactive behaviour.  Declarative
        callers pass one ``RunSpec`` per repetition, allowing failures to be
        persisted instead of disappearing from an average.
        """
        if run_specs is None:
            run_specs = [
                RunSpec(
                    experiment="interactive",
                    benchmark=Path(os.path.abspath(self.bc_path)),
                    analyzer=self.__class__.TOOL or self.configuration.split(" ")[0],
                    configuration={"legacy_configuration": self.configuration, "mask": self.mask},
                    repetition=i,
                    timeout_sec=Runner.TIME_LIMIT,
                    memory_limit_bytes=Runner.MEMORY_LIMIT,
                )
                for i in range(self.__class__.REPETITION_NUM)
            ]
        pta_log_prefix: str = os.path.join(
            self.log_directory_path,
            f"{self.project_name}-{self.note}",
        )
        current_color_code: str = self.__class__.CONFIGURATIONS[self.configuration]
        os.makedirs(self.log_directory_path, exist_ok=True)
        for run_spec in run_specs:
            if (run_spec.benchmark != Path(os.path.abspath(self.bc_path))
                    and str(run_spec.benchmark) != self.bc_path):
                raise ValueError("RunSpec benchmark does not match this analyzer")
            pta_log_1: str = f"{pta_log_prefix}-r{run_spec.repetition}-1.log"
            pta_log_2: str = f"{pta_log_prefix}-r{run_spec.repetition}-2.log"
            pta_cmd: str = f"/usr/bin/time -v {self.option} {self.bc_path}" + f" > {pta_log_1} 2>{pta_log_2}"
            colored_write_line(text=pta_cmd, color_code=current_color_code)

            runner: Runner = Runner(
                cmd=pta_cmd,
                stderr_path=pta_log_2,
                time_limit_sec=run_spec.timeout_sec,
                memory_limit_bytes=run_spec.memory_limit_bytes,
            )
            runner.run_cmd()
            status = RunStatus(runner.status)
            peak_rss_bytes = runner.memory_usage * 1024 if isinstance(runner.memory_usage, int) else None
            signal_number = -runner.return_code if runner.return_code is not None and runner.return_code < 0 else None
            run_result = RunResult(
                run_spec=run_spec,
                status=status,
                wall_time_sec=runner.elapsed_time if isinstance(runner.elapsed_time, float) else None,
                cpu_time_sec=None,
                peak_rss_bytes=peak_rss_bytes,
                exit_code=runner.return_code if runner.return_code is not None and runner.return_code >= 0 else None,
                signal=signal_number,
                stdout_path=os.path.abspath(pta_log_1),
                stderr_path=os.path.abspath(pta_log_2),
                command=tuple(shlex.split(self.option) + [self.bc_path]),
            )
            self.run_results.append(run_result)
            if status == RunStatus.SUCCESS:
                if isinstance(runner.elapsed_time, float):
                    self.time_list.append(runner.elapsed_time)
                if isinstance(runner.memory_usage, int):
                    self.memory_list.append(runner.memory_usage)

        if len(self.time_list) == 0 or len(self.memory_list) == 0:
            self.average_time = None
            self.max_memory = None
        else:
            self.average_time = sum(self.time_list) / len(self.time_list)
            self.max_memory = max(self.memory_list)

        colored_write_line(text=f"note: {self.note}", color_code=current_color_code)
        colored_write_line(text=f"pta time: {self.average_time}", color_code=current_color_code)
        colored_write_line(text=f"pta memory: {self.max_memory}", color_code=current_color_code)
        return self.run_results
