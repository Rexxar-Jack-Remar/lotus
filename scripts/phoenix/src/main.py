import os
from util.color import *
from util.runner import *
from operation.phoenix_analyzer import *
from operation.svf_analyzer import *
from operation.tpa_analyzer import *
from operation.aser_analyzer import *
from operation.sparrow_analyzer import *
from util.writer import *
import config.config as config


def main():

    os.chdir(config.WORK_DIRECTORY_PATH)
    colored_write_line(f"work: {sorted(os.listdir(os.getcwd()))}", GOLD)
    directory_path: str = input("Please enter a directory path to execute analysis, or enter * to exit: ")
    if directory_path == "*" or directory_path == "":
        return
    os.chdir(directory_path)
    cwd: str = os.getcwd()
    colored_write_line(f"CWD: {cwd}", CYAN)
    bc_file_path_list: list[str] = sorted(os.listdir(cwd))
    colored_write_line(f"batch: {bc_file_path_list}", GOLD)
    tool: str = input("Please choose Phoenix/SVF/TPA/Aser/Sparrow/Sparrow-plus-1/Sparrow-plus-2: ")
    match tool.lower():
        case "phoenix":
            tool = "Phoenix"
        case "svf":
            tool = "SVF"
        case "tpa":
            tool = "TPA"
        case "aser":
            tool = "Aser"
        case "sparrow":
            tool = "Sparrow"
        case "sparrow-plus-1":
            tool = "Sparrow-plus-1"
        case "sparrow-plus-2":
            tool = "Sparrow-plus-2"
        case _:
            colored_write_line("There is something wrong with the tool!", RED)
            return
    # enabling_optimization: bool | str = input("Please enter Y/y to enable optimization: ")
    # if enabling_optimization == 'y' or enabling_optimization == 'Y':
    #     enabling_optimization = True
    # else:
    #     enabling_optimization = False
    ok: str = input("Please enter Y/y to start: ")
    if ok != "Y" and ok != "y":
        return

    Writer.directory_path = directory_path
    Writer.cwd = cwd
    Writer.write_heading()

    CONFIGURATIONS: dict[str, str] = {}
    match tool.lower():
        case "phoenix":
            CONFIGURATIONS = PhoenixAnalyzer.CONFIGURATIONS
        case "svf":
            CONFIGURATIONS = SVFAnalyzer.CONFIGURATIONS
        case "tpa":
            CONFIGURATIONS = TPAAnalyzer.CONFIGURATIONS
        case "aser":
            CONFIGURATIONS = AserAnalyzer.CONFIGURATIONS
        case "sparrow":
            CONFIGURATIONS = SparrowAnalyzer.CONFIGURATIONS
        case "sparrow-plus-1":
            CONFIGURATIONS = SparrowAnalyzer.CONFIGURATION1
        case "sparrow-plus-2":
            CONFIGURATIONS = SparrowAnalyzer.CONFIGURATION2
        case _:
            colored_write_line("There is something wrong with the tool!", RED)
            return

    for bc_file_path in bc_file_path_list:
        if "config" == bc_file_path:
            continue
        colored_write_line("#" * os.get_terminal_size().columns, BLUE)
        colored_write_line("#" * os.get_terminal_size().columns, BLUE)
        colored_write_line("#" * os.get_terminal_size().columns, BLUE)

        project_name: str = os.path.splitext(bc_file_path)[0]
        Analyzer.init(project_name=project_name)
        analyzers: list[PhoenixAnalyzer | SVFAnalyzer | TPAAnalyzer | AserAnalyzer | SparrowAnalyzer] = []
        for configuration in CONFIGURATIONS.keys():
            analyzer: PhoenixAnalyzer | SVFAnalyzer | TPAAnalyzer | AserAnalyzer | SparrowAnalyzer | None = None
            match tool.lower():
                case "phoenix":
                    analyzer = PhoenixAnalyzer(
                        project_name=project_name,
                        bc_file_path=bc_file_path,
                        configuration=configuration,
                        mask=None,
                    )
                    analyzers.append(analyzer)
                case "svf":
                    analyzer = SVFAnalyzer(
                        project_name=project_name,
                        bc_file_path=bc_file_path,
                        configuration=configuration,
                        mask=None,
                    )
                    analyzers.append(analyzer)
                case "tpa":
                    analyzer = TPAAnalyzer(
                        project_name=project_name,
                        bc_file_path=bc_file_path,
                        configuration=configuration,
                        mask=None,
                    )
                    analyzers.append(analyzer)
                case "aser":
                    analyzer = AserAnalyzer(
                        project_name=project_name,
                        bc_file_path=bc_file_path,
                        configuration=configuration,
                        mask=None,
                    )
                    analyzers.append(analyzer)
                case "sparrow":
                    analyzer = SparrowAnalyzer(
                        project_name=project_name,
                        bc_file_path=bc_file_path,
                        configuration=configuration,
                        mask=None,
                    )
                    analyzers.append(analyzer)
                case "sparrow-plus-1":
                    length: int = len(SparrowAnalyzer.OPTIMIZATIONS)
                    for i in range(0, 2**length):
                        mask: str = bin(i)[2:].zfill(length)
                        analyzer = SparrowAnalyzer(
                            project_name=project_name,
                            bc_file_path=bc_file_path,
                            configuration=configuration,
                            mask=mask,
                        )
                        analyzers.append(analyzer)
                case "sparrow-plus-2":
                    length: int = len(SparrowAnalyzer.OPTIMIZATIONS)
                    for i in range(0, 2**length):
                        mask: str = bin(i)[2:].zfill(length)
                        analyzer = SparrowAnalyzer(
                            project_name=project_name,
                            bc_file_path=bc_file_path,
                            configuration=configuration,
                            mask=mask,
                        )
                        analyzers.append(analyzer)
                case _:
                    colored_write_line("There is something wrong with the tool!", RED)
                    return
        for analyzer in analyzers:
            colored_write_line("#" * os.get_terminal_size().columns, GOLD)
            colored_write_line(text=f"Processing [{analyzer.option} {bc_file_path}]", color_code=CYAN)
            analyzer.analyze()
            writer: Writer = Writer(
                project_name=project_name,
                analyzer=analyzer,
            )
            writer.append_row()
        colored_write_line("#" * os.get_terminal_size().columns, GOLD)

    colored_write_line("#" * os.get_terminal_size().columns, BLUE)
    colored_write_line("#" * os.get_terminal_size().columns, BLUE)
    colored_write_line("#" * os.get_terminal_size().columns, BLUE)


if __name__ == "__main__":
    main()
