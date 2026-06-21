import os
from util.color import *
from util.runner import *
from operation.analyzer import Analyzer


class SparrowAnalyzer(Analyzer):

    TOOL: str = "lotus-alias-sparrow-aa"

    CONFIGURATIONS: dict[str, str] = {
        "--andersen-k-cs=1": YELLOW,
        "--andersen-k-cs=2": MAGENTA,
    }

    CONFIGURATION1: dict[str, str] = {
        "--andersen-k-cs=1": YELLOW,
    }

    CONFIGURATION2: dict[str, str] = {
        "--andersen-k-cs=2": MAGENTA,
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
        self.note: str = f"sparrow[{self.configuration.split('=')[-1]}]"
        if mask is not None:
            self.note += f"[{mask}]"
        self.log_directory_path: str = os.path.join(
            "log",
            f"{self.project_name}",
        )
