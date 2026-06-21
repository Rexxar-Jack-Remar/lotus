import csv
from .color import *
from operation.phoenix_analyzer import *
from operation.svf_analyzer import *
from operation.tpa_analyzer import *
from operation.aser_analyzer import *
from operation.sparrow_analyzer import *


class Writer:

    heading: list[str] = [
        "project_name",
        "tool",
        "configuration",
        "mask",
        "optimization",
        "pta_time",
        "pta_memory",
        "pta_time_list",
        "pta_memory_list",
        "directory_path",
    ]
    cwd: str = ""
    directory_path: str = ""
    csv_file_path: str = ""

    def __init__(
        self,
        project_name: str,
        analyzer: PhoenixAnalyzer | SVFAnalyzer | TPAAnalyzer | AserAnalyzer | SparrowAnalyzer,
    ) -> None:

        self._project_name: str = project_name
        self._tool: str = analyzer.__class__.TOOL
        if self._tool == "":
            self._tool = analyzer.configuration.split(" ")[0]
        self._configuration: str = analyzer.configuration
        self._mask: str | None = analyzer.mask
        self._optimization: list[str] | None = analyzer.optimization
        self._analyzer: PhoenixAnalyzer | SVFAnalyzer | TPAAnalyzer | AserAnalyzer | SparrowAnalyzer = analyzer
        self._row: list[int | float | str | list | None] = [
            self._project_name,  # "project_name",
            self._tool,  # "tool",
            self._configuration,  # "configuration",
            self._mask,  # mask
            self._optimization,  # optimization
            self._analyzer.average_time,  # "pta_time",
            self._analyzer.max_memory,  # "pta_memory",
            str(self._analyzer.time_list),  # "pta_time_list",
            str(self._analyzer.memory_list),  # "pta_memory_list",
            self.__class__.directory_path,  # "directory_path",
        ]

    @classmethod
    def write_heading(cls) -> None:
        cls.csv_file_path: str = os.path.join(
            os.path.dirname(cls.cwd),
            f"{os.path.basename(cls.directory_path)}.csv",
        )
        with open(cls.csv_file_path, "a", newline="", encoding="utf-8") as file:
            writer = csv.writer(file)
            writer.writerow(cls.heading)
            file.flush()

    def append_row(self) -> None:
        with open(self.__class__.csv_file_path, "a", newline="", encoding="utf-8") as file:
            writer = csv.writer(file)
            writer.writerow(self._row)
            file.flush()
