import os
from util.color import *
from util.runner import *
from operation.analyzer import Analyzer


class AserAnalyzer(Analyzer):

    TOOL: str = "lotus-alias-aser-aa"

    CONFIGURATIONS: dict[str, str] = {
        "--analysis-mode=ci": GREEN,
        "--analysis-mode=1-cfa": YELLOW,
        "--analysis-mode=2-cfa": MAGENTA,
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
        self.note: str = f"aser[{((self.configuration.split('=')[1]).split('-')[0])}]"
        if mask is not None:
            self.note += f"[{mask}]"
        self.log_directory_path: str = os.path.join(
            "log",
            f"{self.project_name}",
        )
