import os
from util.color import *
from util.runner import *
import config.config as config


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

    def analyze(self) -> None:
        pta_log_prefix: str = os.path.join(
            self.log_directory_path,
            f"{self.project_name}-{self.note}",
        )
        current_color_code: str = self.__class__.CONFIGURATIONS[self.configuration]
        for i in range(0, self.__class__.REPETITION_NUM):
            pta_log_1: str = f"{pta_log_prefix}-{chr(ord('a') + i)}-1.log"
            pta_log_2: str = f"{pta_log_prefix}-{chr(ord('a') + i)}-2.log"
            pta_cmd: str = f"/usr/bin/time -v {self.option} {self.bc_path}" + f" > {pta_log_1} 2>{pta_log_2}"
            colored_write_line(text=pta_cmd, color_code=current_color_code)

            elapsed_time_temp: float | str = "error"
            memory_usage_temp: int | str = "error"
            runner: Runner = Runner(cmd=pta_cmd, stderr_path=pta_log_2)
            runner.run_cmd()
            elapsed_time_temp = runner.elapsed_time
            memory_usage_temp = runner.memory_usage

            if not isinstance(elapsed_time_temp, float) or not isinstance(memory_usage_temp, int):
                continue
            else:
                self.time_list.append(elapsed_time_temp)
                self.memory_list.append(memory_usage_temp)

        if len(self.time_list) == 0 or len(self.memory_list) == 0:
            self.average_time = None
            self.max_memory = None
        else:
            self.average_time = sum(self.time_list) / len(self.time_list)
            self.max_memory = max(self.memory_list)

        colored_write_line(text=f"note: {self.note}", color_code=current_color_code)
        colored_write_line(text=f"pta time: {self.average_time}", color_code=current_color_code)
        colored_write_line(text=f"pta memory: {self.max_memory}", color_code=current_color_code)
