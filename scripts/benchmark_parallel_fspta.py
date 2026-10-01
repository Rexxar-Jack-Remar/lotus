#!/usr/bin/env python3
"""Record reproducible local FS-PTA correctness and performance experiments.

Verification runs are separate from timed runs. Each JSON record includes its
exact command, input hash, wall time, solver statistics, exit status, and logs.
Never interpret a failed/timed-out/inequivalent run as a speedup observation.
"""
import argparse
import hashlib
import json
import os
import platform
import re
import random
import signal
import shutil
import tempfile
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path


def run(command, timeout):
    started = time.perf_counter()
    time_tool = Path("/usr/bin/time")
    measured = command
    if time_tool.is_file():
        measured = [str(time_tool), "-l" if platform.system() == "Darwin" else "-v"] + command
    process = subprocess.Popen(measured, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True,
                               start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=timeout)
        status = process.returncode
    except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
        # The memory-measurement wrapper and analysis share a dedicated group.
        # Kill both; killing only the wrapper leaves the analysis running.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        stdout, stderr = process.communicate()
        if isinstance(error, KeyboardInterrupt):
            raise
        status = "timeout"
    stats = {}
    for line in stdout.splitlines():
        if line.startswith("fspta.") and "=" in line:
            key, value = line.split("=", 1)
            stats[key] = value
    memory = re.search(r"(\d+)\s+maximum resident set size", stderr)
    rss_bytes = int(memory.group(1)) if memory else None
    if not memory:
        memory = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", stderr)
        rss_bytes = int(memory.group(1)) * 1024 if memory else None
    return {"command": measured, "analysis_command": command, "status": status,
            "wall_seconds": time.perf_counter() - started,
            "maximum_rss_bytes": rss_bytes,
            "statistics": stats, "stdout": stdout, "stderr": stderr}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path)
    parser.add_argument("--binary", type=Path,
                        default=Path("build/bin/lotus-alias-fspta"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--threads", nargs="+", type=int, default=[1, 2, 4, 8])
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--solver-option", action="append", default=[],
                        help="Additional option for parallel runs only")
    parser.add_argument("--sequential-backends", nargs="+",
                        choices=["mutable", "hash-consed"],
                        default=["mutable", "hash-consed"])
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--seed", type=int, default=0,
                        help="Seed for interleaved measurement order")
    args = parser.parse_args()
    if args.repetitions < 1 or any(worker < 1 for worker in args.threads):
        parser.error("repetitions and worker counts must be positive")
    if args.timeout <= 0 or args.warmups < 0:
        parser.error("timeout must be positive and warmups nonnegative")
    args.threads = list(dict.fromkeys(args.threads))
    args.sequential_backends = list(dict.fromkeys(args.sequential_backends))
    original_binary = args.binary.resolve()
    digest = hashlib.sha256(original_binary.read_bytes()).hexdigest()
    snapshot_directory = Path(tempfile.gettempdir()) / 'lotus-fspta-binaries'
    snapshot_directory.mkdir(exist_ok=True)
    binary = snapshot_directory / ('fspta-' + digest)
    if not binary.exists():
        shutil.copy2(original_binary, binary)
    document = {"timestamp": datetime.now(timezone.utc).isoformat(),
                "platform": platform.platform(), "processor": platform.processor(),
                "binary": str(binary), "original_binary": str(original_binary),
                "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "experiment": {"threads": args.threads,
                               "sequential_backends": args.sequential_backends,
                               "warmups": args.warmups, "seed": args.seed,
                               "parallel_options": args.solver_option},
                "git_head": subprocess.check_output(
                    ["git", "rev-parse", "HEAD"], text=True).strip(),
                "git_status": subprocess.check_output(
                    ["git", "status", "--short"], text=True),
                "runs": []}
    random_order = random.Random(args.seed)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for input_path in args.inputs:
        input_path = input_path.resolve()
        digest = hashlib.sha256(input_path.read_bytes()).hexdigest()
        verified = {}
        for threads in args.threads:
            command = [str(binary), str(input_path), "--parallel",
                       f"--threads={threads}", "--verify-parallel", "--dump-stats"]
            command += args.solver_option
            record = run(command, args.timeout)
            record.update(input=str(input_path), input_sha256=digest,
                          phase="verification", threads=threads)
            document["runs"].append(record)
            verified[threads] = (record["status"] == 0 and
                                record["statistics"].get(
                                    "fspta.parallel-equivalent") == "true")
            print(input_path.name, "verify", threads, record["status"], flush=True)
            args.output.write_text(json.dumps(document, indent=2) + "\n")
        # Keep sequential measurements even if a parallel configuration fails.
        modes = [(0, backend) for backend in args.sequential_backends]
        modes += [(t, "parallel") for t in args.threads if verified[t]]
        for repetition in range(-args.warmups, args.repetitions):
            order = list(modes)
            random_order.shuffle(order)
            for threads, backend in order:
                command = [str(binary), str(input_path), "--dump-stats"]
                if threads:
                    command += ["--parallel", f"--threads={threads}"] + args.solver_option
                else:
                    command += [f"--points-to-sets={backend}"]
                record = run(command, args.timeout)
                record.update(input=str(input_path), input_sha256=digest,
                              phase="warmup" if repetition < 0 else "measurement",
                              threads=threads, backend=backend,
                              repetition=repetition)
                document["runs"].append(record)
                print(input_path.name, record["phase"], threads, backend, record["status"],
                      record["statistics"].get("fspta.solve-seconds"), flush=True)
                args.output.write_text(json.dumps(document, indent=2) + "\n")
    failed = any(r["status"] != 0 for r in document["runs"])
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
