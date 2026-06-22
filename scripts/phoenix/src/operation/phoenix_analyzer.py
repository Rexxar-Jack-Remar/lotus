import os
from util.color import *
from util.runner import *
from operation.analyzer import Analyzer


class PhoenixAnalyzer(Analyzer):

    TOOL: str = ""

    CONFIGURATIONS: dict[str, str] = {
        "lotus-alias-sparrow-aa --andersen-k-cs=0": GREEN,
        "lotus-alias-aser-aa --analysis-mode=ci": YELLOW,
        "lotus-alias-tpa --k-limit=0": MAGENTA,
    }

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
        self.note: str = (self.configuration.split(" ")[0]).split("-")[0]
        self.log_directory_path: str = os.path.join(
            "log",
            f"{self.project_name}",
        )
