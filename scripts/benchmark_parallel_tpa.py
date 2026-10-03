#!/usr/bin/env python3
"""Run serial/parallel TPA benchmarks with separate full-solution verification.

Runs are sequential to avoid competition between benchmark processes. Record
timeouts and failures alongside successful runs; never treat either as speedups.
The POSIX wait4 resource record measures each analysis process without a time
wrapper. The binary is snapshotted so all appended phases use identical code.
"""

import argparse
import hashlib
import json
import os
import platform
import random
import shutil
import signal
import subprocess
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, timeout, cwd):
    started = time.perf_counter()
    with tempfile.TemporaryFile(mode="w+") as stdout_file, tempfile.TemporaryFile(
        mode="w+"
    ) as stderr_file:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            stdout=stdout_file,
            stderr=stderr_file,
            start_new_session=True,
        )
        timed_out = False
        try:
            while True:
                pid, wait_status, usage = os.wait4(process.pid, os.WNOHANG)
                if pid:
                    break
                if time.perf_counter() - started >= timeout:
                    timed_out = True
                    os.killpg(process.pid, signal.SIGKILL)
                    _, wait_status, usage = os.wait4(process.pid, 0)
                    break
                time.sleep(0.05)
        except BaseException:
            try:
                os.killpg(process.pid, signal.SIGKILL)
                _, wait_status, _ = os.wait4(process.pid, 0)
                process.returncode = os.waitstatus_to_exitcode(wait_status)
            except ProcessLookupError:
                pass
            raise
        process.returncode = os.waitstatus_to_exitcode(wait_status)
        elapsed = time.perf_counter() - started
        stdout_file.seek(0)
        stderr_file.seek(0)
        stdout = stdout_file.read()
        stderr = stderr_file.read()
    stats = {}
    for line in stdout.splitlines():
        if line.startswith("tpa.") and "=" in line:
            key, value = line.split("=", 1)
            stats[key] = value
    return {
        "command": command,
        "status": "timeout" if timed_out else process.returncode,
        "wall_seconds": elapsed,
        "user_cpu_seconds": usage.ru_utime,
        "system_cpu_seconds": usage.ru_stime,
        "maximum_rss_bytes": usage.ru_maxrss
        * (1 if platform.system() == "Darwin" else 1024),
        "timeout_seconds": timeout,
        "statistics": stats,
        "stdout": stdout,
        "stderr": stderr,
    }


def save(document, output):
    temporary = output.with_suffix(output.suffix + ".tmp")
    temporary.write_text(json.dumps(document, indent=2) + "\n")
    temporary.replace(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path)
    parser.add_argument("--binary", type=Path, default=Path("build/bin/lotus-alias-tpa"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--append", action="store_true")
    parser.add_argument("--label", default="", help="Label a run family, e.g. repeated or grid")
    parser.add_argument(
        "--phase", choices=["baseline", "measurement", "verification"], default="baseline"
    )
    parser.add_argument("--threads", nargs="+", type=int, default=[1, 2, 4, 8])
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--lookahead", type=int, default=4)
    parser.add_argument("--k-limit", type=int, default=0)
    parser.add_argument(
        "--context-strategy", choices=["klimit", "selective"], default="klimit"
    )
    parser.add_argument("--ext", type=Path, default=Path("config/ptr.spec"))
    parser.add_argument("--seed", type=int, default=0)
    args = parser.parse_args()
    if (
        args.repetitions < 1
        or args.timeout <= 0
        or args.lookahead < 1
        or args.k_limit < 0
    ):
        parser.error(
            "repetitions, timeout and lookahead must be positive; k-limit nonnegative"
        )
    if any(t < 0 for t in args.threads):
        parser.error("thread counts must be nonnegative (0 selects serial measurement)")
    if args.phase == "verification" and 0 in args.threads:
        parser.error("verification requires positive parallel thread counts")
    root = Path(__file__).resolve().parents[1]
    original_binary = args.binary.resolve()
    binary_hash = sha256(original_binary)
    snapshots = Path(tempfile.gettempdir()) / "lotus-tpa-binaries"
    snapshots.mkdir(exist_ok=True)
    binary = snapshots / ("tpa-" + binary_hash)
    if not binary.exists():
        shutil.copy2(original_binary, binary)
    configuration = {
        "binary_sha256": binary_hash,
        "k_limit": args.k_limit,
        "context_strategy": args.context_strategy,
        "lookahead": args.lookahead,
        "external_models": str(args.ext.resolve()),
        "external_models_sha256": sha256(args.ext.resolve()),
    }
    if args.append:
        document = json.loads(args.output.read_text())
        if document["configuration"] != configuration:
            parser.error(
                "appended phases must use the same binary, contexts and external models"
            )
    else:
        if args.output.exists():
            parser.error("output exists; use --append or a new output path")
        document = {
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "platform": platform.platform(),
            "processor": platform.processor(),
            "logical_cpus": os.cpu_count(),
            "original_binary": str(original_binary),
            "binary": str(binary),
            "working_directory": str(root),
            "configuration": configuration,
            "git_head": subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=root, text=True
            ).strip(),
            "git_status": subprocess.check_output(
                ["git", "status", "--short"], cwd=root, text=True
            ),
            "runs": [],
            "invocations": [],
        }
    invocation = vars(args).copy()
    invocation["inputs"] = [str(p.resolve()) for p in args.inputs]
    for key in ("binary", "output", "ext"):
        invocation[key] = str(getattr(args, key).resolve())
    document["invocations"].append(
        {"timestamp": datetime.now(timezone.utc).isoformat(), "arguments": invocation}
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    save(document, args.output)
    order = random.Random(args.seed)
    failed = False
    inputs = dict.fromkeys(p.resolve() for p in args.inputs)
    for path in sorted(inputs, key=lambda p: p.stat().st_size):
        digest = sha256(path)
        modes = [0] if args.phase == "baseline" else list(dict.fromkeys(args.threads))
        for repetition in range(args.repetitions):
            current_modes = modes[:]
            if args.phase == "measurement":
                order.shuffle(current_modes)
            for threads in current_modes:
                command = [
                    str(binary),
                    str(path),
                    "--dump-stats",
                    f"--k-limit={args.k_limit}",
                    f"--context-strategy={args.context_strategy}",
                    f"--ext={args.ext.resolve()}",
                ]
                if threads:
                    command += [
                        "--parallel",
                        f"--threads={threads}",
                        f"--parallel-lookahead={args.lookahead}",
                    ]
                if args.phase == "verification":
                    command += ["--verify-parallel"]
                print(path.name, args.phase, "threads=", threads, "START", flush=True)
                record = run(command, args.timeout, root)
                record.update(
                    input=str(path),
                    input_sha256=digest,
                    input_bytes=path.stat().st_size,
                    phase=args.phase,
                    threads=threads,
                    repetition=repetition,
                    label=args.label,
                )
                work_done = int(record["statistics"].get("tpa.transfers", "0")) > 0
                correct = record["statistics"].get("tpa.parallel-equivalent") == "true"
                record["completed_solver_work"] = record["status"] == 0 and work_done
                record["verified"] = (
                    args.phase == "verification"
                    and record["status"] == 0
                    and correct
                    and work_done
                )
                failed |= record["status"] != 0 or not record["completed_solver_work"]
                if args.phase == "verification":
                    failed |= not record["verified"]
                document["runs"].append(record)
                save(document, args.output)
                print(
                    path.name,
                    args.phase,
                    "threads=", threads,
                    record["status"],
                    "wall=", round(record["wall_seconds"], 3),
                    "solve=", record["statistics"].get("tpa.solve-seconds", "unavailable"),
                    "rss-MiB=", round(record["maximum_rss_bytes"] / 1024**2, 1),
                    flush=True,
                )
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
