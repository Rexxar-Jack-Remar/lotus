#!/usr/bin/env python3
"""Run an external classical-CFL graph dataset through lotus-cfl-solve.

The script deliberately keeps dataset policy outside the solver binary. It
emits one JSON object per run and never invokes the LLVM alias/value-flow
frontends; those remain separate end-to-end bitcode entry points.
"""

import argparse
import concurrent.futures
import json
import os
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple


DEFAULT_CASES = (
    "cactus",
    "deepsjeng",
    "imagick",
    "lbm",
    "leela",
    "mcf",
    "nab",
    "omnetpp",
    "parest",
    "perlbench",
    "povray",
    "x264",
    "xz",
)

ANALYSES = {
    "alias": ("pegs", "aa"),
    "value-flow": ("svfgs", "vf"),
    "taint": ("svfgs", "taint"),
}

DEFAULT_CONFIGURATIONS = (
    "pocr:cfg:count",
    "skewed:ecfg:count",
)

GENERAL_SOLVERS = (
    "sparse-set",
    "sparse-bitvector",
    "graspan",
    "sqid",
    "pearl",
    "skewed",
    "cat",
    "iea",
    "iea-ocr",
    "transitive-closure",
    "pocr",
    "hpocr",
    "focr",
    "endpoint-quotient",
    "cert",
)

ACTIVE_PROCESSES = set()
ACTIVE_PROCESSES_LOCK = threading.Lock()
STOP_REQUESTED = threading.Event()


def terminate_process(process: subprocess.Popen, grace_seconds: float = 1.0) -> None:
    if process.poll() is not None:
        return
    try:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGTERM)
        else:
            process.terminate()
        process.wait(timeout=grace_seconds)
        return
    except (ProcessLookupError, subprocess.TimeoutExpired):
        pass
    if process.poll() is not None:
        return
    try:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGKILL)
        else:
            process.kill()
    except ProcessLookupError:
        return


def terminate_all_processes() -> None:
    with ACTIVE_PROCESSES_LOCK:
        processes = list(ACTIVE_PROCESSES)
    for process in processes:
        terminate_process(process)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--data",
        type=Path,
        default=Path("benchmarks/real-world/CFL/Classical"),
    )
    parser.add_argument(
        "--binary", type=Path, default=Path("build/bin/lotus-cfl-solve")
    )
    parser.add_argument(
        "--case",
        action="append",
        dest="cases",
        help="Repeat or use commas; defaults to all 13 benchmarks",
    )
    parser.add_argument(
        "--analysis",
        action="append",
        dest="analyses",
        help="Repeat or use commas: alias,value-flow,taint; defaults to all",
    )
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument(
        "--configuration",
        action="append",
        dest="configurations",
        metavar="SOLVER:GRAMMAR_SUFFIX:RESULT_SCOPE",
        help="Repeatable, for example pocr:cfg:count or skewed:ecfg:count",
    )
    selection.add_argument(
        "--solver",
        action="append",
        dest="solvers",
        help="Repeat or use commas; uses --grammar-suffix and --result-scope",
    )
    selection.add_argument(
        "--all-solvers",
        action="store_true",
        help="Run every generic SolverSession backend on the original grammar",
    )
    parser.add_argument("--grammar-suffix", default="cfg")
    parser.add_argument(
        "--result-scope", choices=("all", "start", "count"), default="count"
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--timeout",
        type=float,
        default=0.0,
        metavar="SECONDS",
        help="Per-run timeout; zero disables it",
    )
    parser.add_argument(
        "--workers", type=int, default=1, help="Concurrent solver processes"
    )
    parser.add_argument("--fail-fast", action="store_true")
    parser.add_argument("--validate-only", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--max-attribute-expansions", type=int, default=1_000_000
    )
    args = parser.parse_args()
    if args.workers < 1:
        parser.error("--workers must be positive")
    if args.timeout < 0:
        parser.error("--timeout cannot be negative")
    return args


def expand_values(values: Optional[List[str]]) -> List[str]:
    expanded = []
    for value in values or []:
        expanded.extend(item.strip() for item in value.split(",") if item.strip())
    return list(dict.fromkeys(expanded))


def parse_configuration(specification: str) -> Tuple[str, str, str]:
    fields = specification.split(":")
    if len(fields) != 3 or fields[2] not in ("all", "start", "count"):
        raise ValueError(
            "configuration must be SOLVER:GRAMMAR_SUFFIX:all|start|count"
        )
    if fields[0] == "stg":
        raise ValueError(
            "STG requires --stg-spec and must be invoked directly with "
            "lotus-cfl-solve"
        )
    return fields[0], fields[1], fields[2]


def paths_for(
    data: Path, analysis: str, case: str, grammar_suffix: str
) -> Tuple[Path, Path]:
    graph_dir, grammar_stem = ANALYSES[analysis]
    graph = data / "spec2017" / graph_dir / f"{case}.g"
    return graph, data / "grammars" / f"{grammar_stem}.{grammar_suffix}"


def command_for(
    binary: Path,
    graph: Path,
    grammar: Path,
    solver: str,
    result_scope: str,
    maximum_expansions: int,
    validate_only: bool,
) -> List[str]:
    command = [
        str(binary),
        "--grammar",
        str(grammar),
        "--graph",
        str(graph),
        "--graph-mode",
        "plain",
        "--max-attribute-expansions",
        str(maximum_expansions),
    ]
    if validate_only:
        return command + ["--validate-only"]
    command += ["--solver", solver]
    if result_scope != "all":
        command += ["--result-scope", result_scope]
    return command + ["--json-stats"]


def output_text(value) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value or ""


def run_task(
    task: Dict, args: argparse.Namespace, binary: Path
) -> Dict:
    graph = task["graph"]
    grammar = task["grammar"]
    record = {
        "analysis": task["analysis"],
        "benchmark": task["benchmark"],
        "solver": task["solver"],
        "grammar_variant": task["grammar_suffix"],
        "requested_result_scope": task["result_scope"],
        "graph": str(graph),
        "grammar": str(grammar),
    }
    if STOP_REQUESTED.is_set():
        record["status"] = "cancelled"
        return record
    missing = [
        str(path) for path in (binary, graph, grammar) if not path.is_file()
    ]
    if missing:
        record.update({"status": "missing-input", "missing": missing})
        return record

    command = command_for(
        binary,
        graph,
        grammar,
        task["solver"],
        task["result_scope"],
        args.max_attribute_expansions,
        args.validate_only,
    )
    record["command"] = command
    if args.dry_run:
        record["status"] = "dry-run"
        return record

    start = time.monotonic()
    process = None
    try:
        process = subprocess.Popen(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=os.name == "posix",
        )
        with ACTIVE_PROCESSES_LOCK:
            ACTIVE_PROCESSES.add(process)
        if STOP_REQUESTED.is_set():
            terminate_process(process)
        stdout, stderr = process.communicate(
            timeout=args.timeout if args.timeout > 0 else None
        )
    except subprocess.TimeoutExpired:
        if process is not None:
            terminate_process(process)
            stdout, stderr = process.communicate()
        else:
            stdout, stderr = "", ""
        record.update(
            {
                "status": "timeout",
                "exit_code": None,
                "timeout_seconds": args.timeout,
                "wall_seconds": time.monotonic() - start,
            }
        )
        if stdout:
            record["stdout"] = output_text(stdout).strip()
        if stderr:
            record["stderr"] = output_text(stderr).strip()
        return record
    except OSError as error:
        record.update(
            {
                "status": "launch-failed",
                "exit_code": None,
                "error": str(error),
                "wall_seconds": time.monotonic() - start,
            }
        )
        return record
    finally:
        if process is not None:
            with ACTIVE_PROCESSES_LOCK:
                ACTIVE_PROCESSES.discard(process)

    record.update(
        {
            "exit_code": process.returncode,
            "status": (
                "interrupted"
                if STOP_REQUESTED.is_set()
                else ("ok" if process.returncode == 0 else "failed")
            ),
            "wall_seconds": time.monotonic() - start,
        }
    )
    if process.returncode == 0 and not args.validate_only:
        try:
            record.update(json.loads(stdout))
        except json.JSONDecodeError as error:
            record["status"] = "invalid-json"
            record["parse_error"] = str(error)
            record["stdout"] = stdout
    else:
        record["stdout"] = stdout.strip()
    if stderr:
        record["stderr"] = stderr.strip()
    return record


def main() -> int:
    args = parse_args()
    data = args.data.expanduser().resolve()
    binary = args.binary.expanduser().resolve()
    cases = expand_values(args.cases) or list(DEFAULT_CASES)
    analyses = expand_values(args.analyses) or list(ANALYSES)
    unknown_analyses = sorted(set(analyses) - set(ANALYSES))
    unknown_cases = sorted(set(cases) - set(DEFAULT_CASES))
    if unknown_analyses:
        raise SystemExit("unknown analyses: " + ", ".join(unknown_analyses))
    if unknown_cases:
        raise SystemExit("unknown benchmarks: " + ", ".join(unknown_cases))

    requested = args.configurations
    if args.all_solvers:
        requested = [f"{solver}:cfg:count" for solver in GENERAL_SOLVERS]
    elif args.solvers:
        solvers = expand_values(args.solvers)
        unknown_solvers = sorted(set(solvers) - set(GENERAL_SOLVERS))
        if unknown_solvers:
            raise SystemExit("unknown solvers: " + ", ".join(unknown_solvers))
        requested = [
            f"{solver}:{args.grammar_suffix}:{args.result_scope}"
            for solver in solvers
        ]
    configurations = [
        parse_configuration(specification)
        for specification in (requested or DEFAULT_CONFIGURATIONS)
    ]
    destination = (
        args.output.open("w", encoding="utf-8") if args.output else sys.stdout
    )
    tasks = []
    for analysis in analyses:
        for case in cases:
            for solver, grammar_suffix, result_scope in configurations:
                graph, grammar = paths_for(data, analysis, case, grammar_suffix)
                tasks.append(
                    {
                        "analysis": analysis,
                        "benchmark": case,
                        "solver": solver,
                        "grammar_suffix": grammar_suffix,
                        "result_scope": result_scope,
                        "graph": graph,
                        "grammar": grammar,
                    }
                )

    failed = False

    executor = concurrent.futures.ThreadPoolExecutor(max_workers=args.workers)
    futures = []
    interrupted = False
    try:
        futures = [
            executor.submit(run_task, task, args, binary) for task in tasks
        ]
        for future in concurrent.futures.as_completed(futures):
            record = future.result()
            print(
                json.dumps(record, sort_keys=True),
                file=destination,
                flush=True,
            )
            failed = failed or record["status"] not in ("ok", "dry-run")
            if failed and args.fail_fast:
                STOP_REQUESTED.set()
                terminate_all_processes()
                for pending in futures:
                    pending.cancel()
                break
    except KeyboardInterrupt:
        interrupted = True
        STOP_REQUESTED.set()
        terminate_all_processes()
        for future in futures:
            future.cancel()
        print("interrupted: terminated active solver processes", file=sys.stderr)
    finally:
        executor.shutdown(wait=True)
        if destination is not sys.stdout:
            destination.close()
    if interrupted:
        return 130
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
