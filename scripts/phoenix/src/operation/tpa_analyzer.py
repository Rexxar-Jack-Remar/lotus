import os
from util.color import *
from util.runner import *
from operation.analyzer import Analyzer


class TPAAnalyzer(Analyzer):

    TOOL: str = "lotus-alias-tpa"

    CONFIGURATIONS: dict[str, str] = {
        "--k-limit=1": GREEN,
        # "--k-limit=2": YELLOW,
        # "--k-limit=3": MAGENTA,
        # "--k-limit=4": GREEN,
        # "--k-limit=5": YELLOW,
        # "--k-limit=6": MAGENTA,
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
        self.note: str = f"tpa[{self.configuration.split('=')[-1]}]"
        self.log_directory_path: str = os.path.join(
            "log",
            f"{self.project_name}",
        )
