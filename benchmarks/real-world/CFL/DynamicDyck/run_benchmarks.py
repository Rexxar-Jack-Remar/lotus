#!/usr/bin/env python3
"""Run the original table ordering, with optional case selection and verification."""

import argparse
from pathlib import Path
import subprocess
import sys


DACAPO = "antlr bloat chart eclipse fop hsqldb jython luindex lusearch pmd xalan".split()
TAL3 = ("btree compiler crypto helloworld mushroom sample startup xml check compress "
        "derby mpegaudio parser scimark sunflow").split()
TAL4 = ("btree sample parser check compiler compress crypto derby helloworld mpegaudio "
        "scimark startup sunflow xml").split()


def jobs(root, table, cases, ratios, modes):
    for family, programs in (("dacapo_bench", DACAPO), ("tal", TAL3 if table == 3 else TAL4)):
        for program in programs:
            if cases and program not in cases:
                continue
            records = []
            if table == 3:
                records.append((root / "init.dot",
                                root / family / "incremental" / f"{program}_inc.seq"))
                records.append((root / family / "decremental" / f"{program}.dot",
                                root / family / "decremental" / f"{program}_dec.seq"))
            else:
                for ratio in ratios:
                    folder = root / family / "mixed"
                    records.append((folder / f"{program}_{ratio}init.dot",
                                    folder / f"{program}{ratio}.seq"))
            yield program, [(mode, initial, sequence)
                            for initial, sequence in records for mode in modes]


def run(binary, mode, initial, sequence, verify, timeout):
    command = [str(binary)]
    if verify:
        command.append("--print-components")
    command.extend([str(mode), str(initial), str(sequence)])
    result = subprocess.run(command, text=True, capture_output=True, timeout=timeout, check=True)
    fields = result.stdout.splitlines()
    elapsed = fields[0].strip()
    if float(elapsed) < 0:
        raise ValueError("negative elapsed time")
    return elapsed, fields[1:] if verify else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    repository = Path(__file__).resolve().parents[4]
    parser.add_argument("--binary", type=Path,
                        default=repository / "build" / "bin" / "lotus-cfl-dynamic-dyck")
    parser.add_argument("--benchmark-root", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--table", type=int, choices=(3, 4), required=True)
    parser.add_argument("--cases", nargs="+")
    parser.add_argument("--ratios", nargs="+", type=int, choices=(10, 20, 30, 40, 50),
                        default=[10, 20, 30, 40, 50])
    parser.add_argument("--mode", choices=("both", "dynamic", "recompute"), default="both")
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--timeout", type=float)
    args = parser.parse_args()
    root = args.benchmark_root.resolve()
    binary = args.binary.resolve()
    if not args.dry_run and not binary.is_file():
        parser.error("build the lotus-cfl-dynamic-dyck target first or supply --binary")
    available = set(DACAPO + (TAL3 if args.table == 3 else TAL4))
    if args.cases and set(args.cases) - available:
        parser.error("unknown cases: " + ", ".join(sorted(set(args.cases) - available)))
    if args.verify and args.mode != "both":
        parser.error("--verify requires --mode both")
    modes = (1, 0) if args.mode == "both" else (1,) if args.mode == "dynamic" else (0,)
    rows = list(jobs(root, args.table, args.cases, args.ratios, modes))
    for _, row in rows:
        for _, initial, sequence in row:
            if not initial.is_file() or not sequence.is_file():
                parser.error(f"missing benchmark input: {initial} or {sequence}")
    if args.dry_run:
        print(f"table={args.table} cases={len(rows)} runs={sum(len(row) for _, row in rows)} "
              "all_inputs_present=true")
        return
    for program, row in rows:
        values = []
        previous = None
        for mode, initial, sequence in row:
            elapsed, components = run(binary, mode, initial, sequence, args.verify, args.timeout)
            if args.verify:
                if mode == 1:
                    previous = components
                elif previous != components:
                    raise ValueError(f"partition mismatch: {program} {sequence.name}")
            values.append(elapsed)
        print(program + "\t" + " ".join(values) + " ", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, ValueError) as error:
        print(f"benchmark failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stderr, file=sys.stderr)
        sys.exit(1)
