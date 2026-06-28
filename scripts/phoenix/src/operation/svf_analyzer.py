import os
from util.color import *
from util.runner import *
from operation.analyzer import Analyzer


class SVFAnalyzer(Analyzer):

    TOOL: str = "wpa"

    CONFIGURATIONS: dict[str, str] = {
        "-ander": GREEN,
        "-sander": YELLOW,
        "-fspta": MAGENTA,
    }

    OPTIMIZATIONS: list[str] = []

    def __init__(
        self,
        project_name: str,
        bc_file_path: str,
        configuration: str,
        mask: str | None,
    ) -> None:
        super().__init__(
            project_name=project_name,
            bc_file_path=bc_file_path,
            configuration=configuration,
            mask=mask,
        )
        self.note: str = f"svf[{self.configuration[1:]}]"
        self.log_directory_path: str = os.path.join(
            "log",
            f"{self.project_name}",
        )
