#!/usr/bin/env python3
"""Prepare Boolean-program inputs for the independent DemandAPA experiment."""

import argparse
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile


def run_decomposer(binary: Path, graph: Path, output: Path, seconds: float) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with graph.open("rb") as source, output.open("wb") as destination, \
            tempfile.TemporaryFile() as errors:
        process = subprocess.Popen(
            [str(binary)], stdin=source, stdout=destination, stderr=errors
        )
        try:
            process.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        errors.seek(0)
        error = errors.read().decode(errors="replace")
    if output.stat().st_size == 0:
        raise RuntimeError(f"{binary.name} produced no decomposition for {graph}: {error}")
    with output.open(errors="replace") as result:
        if graph.parent.parent.name == "treewidth_solver_input":
            valid = any(line.startswith("s td ") for line in result)
        else:
            first = result.readline().strip()
            valid = first.isdecimal()
    if not valid:
        raise RuntimeError(f"{binary.name} produced an invalid decomposition for {graph}: {error}")


def main() -> None:
    repo = Path(__file__).resolve().parents[1]
    binaries = repo / "build" / "bin"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="one Boolean program or a directory of *.txt")
    parser.add_argument("output", type=Path, help="DemandAPA dataset root")
    parser.add_argument("--parser", type=Path, default=binaries / "lotus-bool-parse")
    parser.add_argument("--normalizer", type=Path, default=binaries / "lotus-bool-normalize")
    parser.add_argument("--treewidth-seconds", type=float, default=0)
    parser.add_argument("--treedepth-seconds", type=float, default=0)
    parser.add_argument("--treewidth-tool", type=Path, default=binaries / "flow_cutter_pace17")
    parser.add_argument("--treedepth-tool", type=Path, default=binaries / "flow_cutter_pace20")
    args = parser.parse_args()

    sources = sorted(args.input.glob("*.txt")) if args.input.is_dir() else [args.input]
    if not sources:
        parser.error("no Boolean-program inputs found")
    for source in sources:
        if not source.is_file():
            parser.error(f"input does not exist: {source}")
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            intermediate = work / "parsed.txt"
            subprocess.run([str(args.parser), str(source.resolve()), str(intermediate)], check=True)
            prepared = args.output / "prep_output" / "BP" / source.name
            prepared.parent.mkdir(parents=True, exist_ok=True)
            with intermediate.open("rb") as parsed, prepared.open("wb") as target:
                subprocess.run([str(args.normalizer)], stdin=parsed, stdout=target,
                               cwd=work, check=True)
            for kind in ("treewidth", "treedepth"):
                graph = args.output / f"{kind}_solver_input" / "BP" / source.name
                graph.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(str(work / f"{kind}_solver_input.txt"), graph)
                seconds = getattr(args, f"{kind}_seconds")
                if seconds > 0:
                    binary = getattr(args, f"{kind}_tool")
                    result = args.output / f"{kind}_solver_output" / "BP" / source.name
                    run_decomposer(binary, graph, result, seconds)
        print(source.name)


if __name__ == "__main__":
    main()
