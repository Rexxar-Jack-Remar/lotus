"""Phoenix entry point.

``python src/main.py run experiments/example.yaml`` is the reproducible,
declarative entry point.  Invoking the script without arguments retains the
original interactive workflow while experiments are migrated.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import shutil
import sys
from dataclasses import replace

import config.config as config
from experiment.errors import ExperimentConfigError
from experiment.loader import expand_runs, load_experiment
from experiment.preprocess import apply_preprocessing
from operation.analyzer import Analyzer
from operation.analyzer_adapter import create_analyzer_adapter
from operation.aser_analyzer import AserAnalyzer
from operation.phoenix_analyzer import PhoenixAnalyzer
from operation.sparrow_analyzer import SparrowAnalyzer
from operation.svf_analyzer import SVFAnalyzer
from operation.tpa_analyzer import TPAAnalyzer
from execution.factory import create_executor
from result.aggregate import write_summary
from result.compare import compare_results, format_comparison
from result.export import export_csv, markdown_report
from result.manifest import write_manifest
from result.store import JsonlResultStore
from util.color import *
from util.writer import Writer


def run_experiment(experiment_path: str | Path, output_dir: str | Path | None = None) -> Path:
    """Run an experiment and return its JSONL result directory."""
    experiment = load_experiment(experiment_path)
    runs = expand_runs(experiment)
    destination = _output_directory(experiment.name, experiment.output_dir, output_dir)
    if (destination / "results.jsonl").exists():
        raise FileExistsError(
            f"Result store already exists: {destination / 'results.jsonl'}; choose a new --output directory"
        )
    store = JsonlResultStore(destination)
    executor = create_executor(destination, experiment.execution.executor)
    runs = apply_preprocessing(experiment, runs, executor, destination / "artifacts")
    commands = [create_analyzer_adapter(run.analyzer).build_command(run) for run in runs]
    if experiment.source_path is not None:
        shutil.copy2(experiment.source_path, destination / "experiment.yaml")
    write_manifest(destination, experiment, runs, commands)

    colored_write_line(f"experiment: {experiment.name}", CYAN)
    colored_write_line(f"runs: {len(runs)}", GOLD)
    colored_write_line(f"results: {destination}", GOLD)
    for run_spec, command in zip(runs, commands):
        colored_write_line(
            text=(f"Processing [{run_spec.analyzer} {run_spec.benchmark.name} "
                  f"repetition={run_spec.repetition}]"),
            color_code=CYAN,
        )
        adapter = create_analyzer_adapter(run_spec.analyzer)
        result = executor.execute(command, run_spec)
        try:
            stdout = result.stdout_path.read_text(encoding="utf-8", errors="replace")
            stderr = result.stderr_path.read_text(encoding="utf-8", errors="replace")
            result = replace(result, metrics=adapter.parse_metrics(stdout, stderr))
        except OSError:
            pass
        store.append(result)
    write_summary(destination)
    return destination


def _output_directory(
    experiment_name: str,
    configured_output: Path | None,
    requested_output: str | Path | None,
) -> Path:
    if requested_output is not None:
        return Path(requested_output).expanduser().resolve()
    if configured_output is not None:
        return configured_output
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    return (Path.cwd() / "results" / f"{experiment_name}-{stamp}").resolve()


def interactive_main() -> None:
    """Original Phoenix CLI, retained temporarily for existing workflows."""
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
    tool = _canonical_interactive_tool(tool)
    if tool == "":
        colored_write_line("There is something wrong with the tool!", RED)
        return
    ok: str = input("Please enter Y/y to start: ")
    if ok not in ("Y", "y"):
        return

    Writer.directory_path = directory_path
    Writer.cwd = cwd
    Writer.write_heading()
    configurations = _interactive_configurations(tool)
    if configurations is None:
        colored_write_line("There is something wrong with the tool!", RED)
        return

    for bc_file_path in bc_file_path_list:
        if bc_file_path == "config":
            continue
        colored_write_line("#" * os.get_terminal_size().columns, BLUE)
        project_name = os.path.splitext(bc_file_path)[0]
        Analyzer.init(project_name=project_name)
        for analyzer in _interactive_analyzers(tool, project_name, bc_file_path, configurations):
            colored_write_line("#" * os.get_terminal_size().columns, GOLD)
            colored_write_line(text=f"Processing [{analyzer.option} {bc_file_path}]", color_code=CYAN)
            analyzer.analyze()
            Writer(project_name=project_name, analyzer=analyzer).append_row()
    colored_write_line("#" * os.get_terminal_size().columns, BLUE)


def _canonical_interactive_tool(value: str) -> str:
    names = {
        "phoenix": "Phoenix", "svf": "SVF", "tpa": "TPA", "aser": "Aser",
        "sparrow": "Sparrow", "sparrow-plus-1": "Sparrow-plus-1",
        "sparrow-plus-2": "Sparrow-plus-2",
    }
    return names.get(value.lower(), "")


def _interactive_configurations(tool: str) -> dict[str, str] | None:
    return {
        "Phoenix": PhoenixAnalyzer.CONFIGURATIONS,
        "SVF": SVFAnalyzer.CONFIGURATIONS,
        "TPA": TPAAnalyzer.CONFIGURATIONS,
        "Aser": AserAnalyzer.CONFIGURATIONS,
        "Sparrow": SparrowAnalyzer.CONFIGURATIONS,
        "Sparrow-plus-1": SparrowAnalyzer.CONFIGURATION1,
        "Sparrow-plus-2": SparrowAnalyzer.CONFIGURATION2,
    }.get(tool)


def _interactive_analyzers(
    tool: str,
    project_name: str,
    bc_file_path: str,
    configurations: dict[str, str],
) -> list[Analyzer]:
    analyzer_class: type[Analyzer]
    if tool == "Phoenix":
        analyzer_class = PhoenixAnalyzer
    elif tool == "SVF":
        analyzer_class = SVFAnalyzer
    elif tool == "TPA":
        analyzer_class = TPAAnalyzer
    elif tool == "Aser":
        analyzer_class = AserAnalyzer
    else:
        analyzer_class = SparrowAnalyzer

    analyzers: list[Analyzer] = []
    for configuration in configurations:
        masks: list[str | None] = [None]
        if tool in ("Sparrow-plus-1", "Sparrow-plus-2"):
            length = len(SparrowAnalyzer.OPTIMIZATIONS)
            masks = [bin(index)[2:].zfill(length) for index in range(2**length)]
        for mask in masks:
            analyzers.append(analyzer_class(project_name, bc_file_path, configuration, mask))
    return analyzers


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Phoenix program-analysis experiment runner")
    subparsers = parser.add_subparsers(dest="command")
    run_parser = subparsers.add_parser("run", help="run a declarative YAML experiment")
    run_parser.add_argument("experiment", help="path to experiment YAML")
    run_parser.add_argument("--output", help="directory for results.jsonl and logs")
    report_parser = subparsers.add_parser("report", help="write aggregate JSON and Markdown report")
    report_parser.add_argument("results", help="experiment result directory")
    report_parser.add_argument("--output", help="Markdown output path")
    export_parser = subparsers.add_parser("export", help="export aggregate data")
    export_parser.add_argument("results", help="experiment result directory")
    export_parser.add_argument("--format", choices=("csv", "markdown"), required=True)
    export_parser.add_argument("--output", help="export output path")
    compare_parser = subparsers.add_parser("compare", help="compare baseline and new results")
    compare_parser.add_argument("baseline", help="baseline result directory")
    compare_parser.add_argument("new", help="new result directory")
    compare_parser.add_argument("--threshold", type=float, default=0.05, help="regression threshold")
    compare_parser.add_argument("--output", help="Markdown output path")
    arguments = parser.parse_args(argv)

    if arguments.command is None:
        interactive_main()
        return 0
    try:
        if arguments.command == "run":
            run_experiment(arguments.experiment, arguments.output)
        elif arguments.command == "report":
            write_summary(arguments.results)
            report = markdown_report(arguments.results)
            output = Path(arguments.output) if arguments.output else Path(arguments.results) / "report.md"
            output.write_text(report, encoding="utf-8")
            print(report, end="")
        elif arguments.command == "export":
            if arguments.format == "csv":
                print(export_csv(arguments.results, arguments.output))
            else:
                report = markdown_report(arguments.results)
                if arguments.output:
                    Path(arguments.output).write_text(report, encoding="utf-8")
                else:
                    print(report, end="")
        elif arguments.command == "compare":
            report = format_comparison(compare_results(arguments.baseline, arguments.new, arguments.threshold))
            if arguments.output:
                Path(arguments.output).write_text(report, encoding="utf-8")
            print(report, end="")
    except (ExperimentConfigError, RuntimeError, OSError, ValueError) as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
