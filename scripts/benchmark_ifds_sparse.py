#!/usr/bin/env python3
"""Compare dense/sparse IFDS work, timing and RSS on an identity-heavy fixture."""

import argparse
import hashlib
import json
import platform
import re
import statistics
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/bin/lotus-dfa-ifds"))
    parser.add_argument("--instructions", type=int, default=10000)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.instructions < 1 or args.repeats < 1:
        parser.error("instructions and repeats must be positive")
    binary = args.binary.resolve()
    rows = {"dense": [], "sparse": []}
    findings = None
    with tempfile.TemporaryDirectory(prefix="lotus-ifds-sparse-") as directory:
        root = Path(directory)
        ir = root / "fixture.ll"
        model = root / "taint.spec"
        model.write_text("SOURCE source Ret V T\nSINK sink Arg0 V\n")
        body = "\n".join(
            f"  %unused{i} = add i32 {i % 100}, 1" for i in range(args.instructions)
        )
        ir.write_text(
            "declare i32 @source()\ndeclare void @sink(i32)\n"
            "define i32 @main() {\n  %value = call i32 @source()\n"
            + body
            + "\n  call void @sink(i32 %value)\n  ret i32 0\n}\n"
        )
        # Alternate order to reduce systematic warm-cache ordering effects.
        for repeat in range(args.repeats):
            modes = ["dense", "sparse"] if repeat % 2 == 0 else ["sparse", "dense"]
            for mode in modes:
                output = root / mode
                output.mkdir(exist_ok=True)
                command = [
                    str(binary), str(ir), "--analysis=taint", "--statistics",
                    f"--taint-config={model}", f"--out-dir={output}",
                ]
                if mode == "sparse":
                    command.append("--sparse")
                if platform.system() == "Darwin":
                    command = ["/usr/bin/time", "-l", *command]
                elif platform.system() == "Linux":
                    command = ["/usr/bin/time", "-f", "peak_rss_kib=%M", *command]
                result = subprocess.run(command, capture_output=True, text=True)
                if result.returncode:
                    raise RuntimeError(result.stdout + result.stderr)
                match = re.search(
                    r"processed_edges=(\d+) sparse_transfers=(\d+) solve_seconds=([\deE.+-]+)",
                    result.stderr,
                )
                if not match:
                    raise RuntimeError("Missing solver statistics: " + result.stderr)
                sample = {
                    "processed_edges": int(match[1]),
                    "sparse_transfers": int(match[2]),
                    "solve_seconds": float(match[3]),
                }
                mac_rss = re.search(r"(\d+)\s+maximum resident set size", result.stderr)
                linux_rss = re.search(r"peak_rss_kib=(\d+)", result.stderr)
                if mac_rss:
                    sample["peak_rss_bytes"] = int(mac_rss[1])
                elif linux_rss:
                    sample["peak_rss_bytes"] = int(linux_rss[1]) * 1024
                current = (output / "ifds.txt").read_bytes()
                if b"Summary: 1 reachable sinks detected." not in current:
                    raise RuntimeError("Expected source-to-sink finding is missing")
                if findings is None:
                    findings = current
                if current != findings:
                    raise RuntimeError("Dense/sparse findings differ")
                rows[mode].append(sample)
    summary = {
        mode: {key: statistics.median(sample[key] for sample in samples)
               for key in samples[0]}
        for mode, samples in rows.items()
    }
    report = {
        "fixture": "one integer source; independent arithmetic; one sink",
        "instructions": args.instructions,
        "repeats": args.repeats,
        "platform": platform.platform(),
        "binary": str(binary),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "findings_identical": True,
        "findings_sha256": hashlib.sha256(findings).hexdigest(),
        "median": summary,
        "samples": rows,
        "scope": "Synthetic work-reduction fixture; not representative of all analyses. "
                 "Timing includes session construction during solve; RSS covers the process.",
    }
    text = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    print(text, end="")


if __name__ == "__main__":
    main()
