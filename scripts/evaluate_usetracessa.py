#!/usr/bin/env python3
"""Compare UseTraceSSA, object-expanded UFG, and Saber on LLVM bitcode.

UseTraceSSA, UFG, and Saber run double-free, use-after-free, memory-leak, and
file-leak. Saber UAF and the IR leak clients report candidates without SMT path
validation; the IR leak clients have limited ownership-escape modeling.
Runs are serial by default so wall time and peak RSS are easier
to compare; --max-workers enables throughput-oriented parallel runs.

Examples:
    python3 scripts/evaluate_usetracessa.py
    python3 scripts/evaluate_usetracessa.py --tools usetracessa,ufg \
        --inputs /path/to/example.bc --results /tmp/example-eval.json
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINS = {
    "usetracessa": ROOT / "build/bin/lotus-ir-usetracessa",
    "ufg": ROOT / "build/bin/lotus-ir-ufg",
    "saber": ROOT / "build/bin/lotus-check",
}
SHARED_CHECKS = ("double-free", "use-after-free", "memory-leak", "file-leak")
SABER_CHECKS = ("double-free", "use-after-free", "memory-leak", "file-leak")
SABER_SMT_MODES = (("smt", ()), ("nosmt", ("--saber.no-smt",)))
KEY_VALUE = re.compile(r"([a-z][a-z0-9_]*)=([^\s]+)")


def key_values(line):
    return dict(KEY_VALUE.findall(line))


def get_process_rss_mb(pid):
    """Current RSS for the direct tool process, in MiB."""
    status = Path(f"/proc/{pid}/status")
    try:
        if status.exists():
            match = re.search(r"^VmRSS:\s+(\d+)\s+kB", status.read_text(), re.MULTILINE)
            return int(match.group(1)) / 1024.0 if match else 0.0
        output = subprocess.check_output(
            ["ps", "-o", "rss=", "-p", str(pid)], stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
        return float(output) / 1024.0 if output else 0.0
    except (OSError, ValueError, subprocess.CalledProcessError):
        return 0.0


def kill_proc_group(proc):
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except (OSError, ProcessLookupError):
        try:
            proc.kill()
        except OSError:
            pass


def reap_with_usage(proc, options):
    pid, status, usage = os.wait4(proc.pid, options)
    if pid == 0:
        return None
    proc.returncode = os.waitstatus_to_exitcode(status)
    return usage


def peak_rss_mb(usage):
    if usage is None:
        return 0.0
    # Darwin reports bytes; Linux reports KiB.
    divisor = 1024.0 * 1024.0 if sys.platform == "darwin" else 1024.0
    return usage.ru_maxrss / divisor


def run_cmd_with_limits(cmd, timeout_s, mem_limit_gb):
    start = time.monotonic()
    with tempfile.TemporaryFile(mode="w+t") as stdout_file, \
            tempfile.TemporaryFile(mode="w+t") as stderr_file:
        try:
            proc = subprocess.Popen(
                cmd, stdout=stdout_file, stderr=stderr_file,
                start_new_session=True,
            )
        except OSError as error:
            return {
                "status": "ERROR(start)", "wall_time_s": 0.0,
                "max_rss_mb": 0.0, "stdout": "", "stderr": str(error),
            }
        peak_rss = 0.0
        killed_reason = None
        usage = None
        while True:
            usage = reap_with_usage(proc, os.WNOHANG)
            if usage is not None:
                break
            peak_rss = max(peak_rss, get_process_rss_mb(proc.pid))
            elapsed = time.monotonic() - start
            if elapsed > timeout_s:
                killed_reason = "TIMEOUT"
            elif peak_rss > mem_limit_gb * 1024.0:
                killed_reason = "OOM"
            if killed_reason:
                kill_proc_group(proc)
                break
            time.sleep(0.1)
        if usage is None:
            usage = reap_with_usage(proc, 0)
        peak_rss = max(peak_rss, peak_rss_mb(usage))
        if not killed_reason and peak_rss > mem_limit_gb * 1024.0:
            killed_reason = "OOM"
        stdout_file.seek(0)
        stderr_file.seek(0)
        stdout = stdout_file.read()
        stderr = stderr_file.read()
    status = killed_reason or ("SUCCESS" if proc.returncode == 0
                               else f"ERROR(rc={proc.returncode})")
    return {
        "status": status,
        "wall_time_s": round(time.monotonic() - start, 3),
        "max_rss_mb": round(peak_rss, 2),
        "stdout": stdout,
        "stderr": stderr,
    }


def parse_ir_output(stdout, stderr):
    """Parse key=value lines without relying on field order or adjacency."""
    parsed = {"check_result": None, "findings": None,
              "finding_sites": None, "exhaustive": None}
    for line in stdout.splitlines():
        if line.startswith(("svfg_nodes=", "batch_products=",
                            "lane_product_states=")):
            for key, value in key_values(line).items():
                try:
                    parsed[key] = int(value)
                except ValueError:
                    pass
        elif line.startswith("check="):
            fields = key_values(line)
            parsed["check_result"] = fields.get("result")
            for key in ("findings", "finding_sites"):
                if key in fields:
                    parsed[key] = int(fields[key])
            if "exhaustive" in fields:
                parsed["exhaustive"] = fields["exhaustive"] == "yes"
    for line in stderr.splitlines():
        if not line.startswith("timing_ms "):
            continue
        for key, value in key_values(line).items():
            try:
                parsed[f"{key}_ms"] = float(value)
            except ValueError:
                pass
    return parsed


def parse_saber_output(stdout, stderr):
    parsed = {"findings": None}
    output = stdout + "\n" + stderr
    for field in ("elapsed-ms", "functions", "instructions"):
        match = re.search(rf"{re.escape(field)}:\s+(\d+)", output)
        if match:
            parsed[field.replace("-", "_")] = int(match.group(1))
    match = re.search(r"Findings \((\d+)\)", output)
    if match:
        parsed["findings"] = int(match.group(1))
    else:
        match = re.search(r"\bfindings:\s*(\d+)", output)
        if match:
            parsed["findings"] = int(match.group(1))
        elif "No findings." in output:
            parsed["findings"] = 0
    return parsed


def build_cmd(tool, bug_type, smt_flags, bc_path, binaries,
              context_limit):
    if tool in ("usetracessa", "ufg"):
        cmd = [str(binaries[tool]), str(bc_path), "--quiet", "--timing",
               f"--check={bug_type}"]
        if context_limit is not None:
            cmd.append(f"--context-limit={context_limit}")
        return cmd
    return [str(binaries["saber"]), "--engine=saber",
            f"--checks={bug_type}", "--analysis-stats",
            f"--saber.context-limit={context_limit}",
            *smt_flags, str(bc_path)]


def make_tasks(benchmarks, tools):
    tasks = []
    for bc_path in benchmarks:
        for bug_type in SHARED_CHECKS:
            for tool in ("usetracessa", "ufg"):
                if tool in tools:
                    tasks.append((tool, bug_type, "nosmt", (), bc_path))
            if bug_type in SABER_CHECKS and "saber" in tools:
                if bug_type == "use-after-free":
                    tasks.append(("saber", bug_type, "nosmt",
                                  ("--saber.no-smt",), bc_path))
                else:
                    for label, flags in SABER_SMT_MODES:
                        tasks.append(("saber", bug_type, label, flags, bc_path))
    return tasks


def execute_task(index, task, timeout_s, mem_limit_gb, binaries,
                 context_limit):
    tool, bug_type, smt_label, smt_flags, bc_path = task
    cmd = build_cmd(tool, bug_type, smt_flags, bc_path, binaries,
                    context_limit)
    execution = run_cmd_with_limits(cmd, timeout_s, mem_limit_gb)
    parsed = (parse_saber_output(execution["stdout"], execution["stderr"])
              if tool == "saber" else
              parse_ir_output(execution["stdout"], execution["stderr"]))
    status = execution["status"]
    if status == "SUCCESS":
        required = ("findings",) if tool == "saber" else (
            "check_result", "findings", "finding_sites", "total_ms",
        )
        if any(parsed.get(field) is None for field in required):
            status = "PARSE_ERROR"
    record = {
        "task_id": index, "tool": tool, "bug_type": bug_type,
        "smt_mode": smt_label,
        "bc": bc_path.name, "bc_path": str(bc_path),
        "analysis_scope": ("context-bounded-icfg-candidate"
                           if tool == "saber" and bug_type == "use-after-free"
                           else "exit-candidate-limited-escape-model"
                           if tool != "saber" and bug_type in ("memory-leak", "file-leak")
                           else "tool-default"),
        "context_semantics": (f"call-string-k={context_limit}"
                              if context_limit is not None
                              else "unbounded-realizable"),
        "comparison_group": ("system-baseline" if tool == "saber"
                             else "representation"),
        "finding_unit": ("bug-report" if tool == "saber"
                         else "sink-with-object-set"),
        "context_limit": context_limit,
        "command": cmd, "status": status,
        "wall_time_s": execution["wall_time_s"],
        "max_rss_mb": execution["max_rss_mb"],
        **parsed,
    }
    if status != "SUCCESS":
        record["stdout_tail"] = execution["stdout"][-2000:]
        record["stderr_tail"] = execution["stderr"][-2000:]
    return record


def save_results(path, records):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="w", dir=path.parent, prefix=f".{path.name}.",
        suffix=".tmp", delete=False,
    ) as output:
        json.dump([item for item in records if item is not None], output, indent=2)
        output.write("\n")
        temporary = Path(output.name)
    os.replace(temporary, path)


def main():
    parser = argparse.ArgumentParser(
        description="Evaluate UseTraceSSA, object-expanded UFG, and Saber")
    parser.add_argument("--tools", default="usetracessa,ufg,saber",
                        help="Comma-separated tools (default: all three)")
    parser.add_argument("--max-workers", type=int, default=1,
                        help="Concurrent runs (default: 1 for timing comparisons)")
    parser.add_argument("--timeout", type=float, default=300.0,
                        help="Per-run timeout in seconds (default: 300)")
    parser.add_argument("--mem-limit-gb", type=float, default=6.0,
                        help="Sampled direct-process RSS limit in GiB (default: 6)")
    context_options = parser.add_mutually_exclusive_group()
    context_options.add_argument("--context-limit", type=int, default=3,
                                 help="Call-string k for all tools (default: 3; "
                                      "zero immediately merges contexts)")
    context_options.add_argument("--unbounded-context", action="store_true",
                                 help="Omit the call-string limit; requires "
                                      "--tools without Saber")
    parser.add_argument("--benchmarks-dir", type=Path,
                        default=ROOT / "benchmarks/real-world/SPEC2017",
                        help="Directory containing LLVM bitcode files")
    parser.add_argument("--inputs", type=Path, nargs="+",
                        help="Explicit LLVM bitcode files instead of --benchmarks-dir")
    parser.add_argument("--results", type=Path, default=Path("eval_results.json"))
    for tool in DEFAULT_BINS:
        parser.add_argument(f"--{tool}-bin", type=Path, default=DEFAULT_BINS[tool])
    args = parser.parse_args()

    tools = tuple(part.strip() for part in args.tools.split(",") if part.strip())
    if not tools or len(set(tools)) != len(tools) or any(
            tool not in DEFAULT_BINS for tool in tools):
        parser.error("--tools must list distinct names from usetracessa,ufg,saber")
    if args.max_workers < 1 or args.timeout <= 0 or args.mem_limit_gb <= 0:
        parser.error("workers, timeout, and memory limit must be positive")
    if args.context_limit < 0:
        parser.error("--context-limit must be nonnegative")
    if "saber" in tools and args.unbounded_context:
        parser.error("Saber has no unbounded context mode; select only "
                     "UseTraceSSA/UFG with --unbounded-context")
    context_limit = None if args.unbounded_context else args.context_limit
    binaries = {tool: getattr(args, f"{tool}_bin").resolve() for tool in tools}
    for tool, binary in binaries.items():
        if not binary.is_file():
            parser.error(f"{tool} binary not found: {binary}")

    if args.inputs:
        benchmarks = [path.resolve() for path in args.inputs]
    else:
        directory = args.benchmarks_dir.resolve()
        if not directory.is_dir():
            parser.error(f"benchmark directory not found: {directory}")
        benchmarks = sorted(path for path in directory.iterdir()
                            if path.is_file() and not path.name.startswith("."))
    if not benchmarks or any(not path.is_file() for path in benchmarks):
        parser.error("no valid benchmark bitcode files were selected")

    tasks = make_tasks(benchmarks, tools)
    records = [None] * len(tasks)
    print(f"Benchmarks: {len(benchmarks)}  Tools: {','.join(tools)}")
    print(f"Tasks: {len(tasks)}  Workers: {args.max_workers}")
    print(f"Timeout: {args.timeout:g}s  RSS limit: {args.mem_limit_gb:g} GiB")
    print(f"Call-string k: {context_limit if context_limit is not None else 'unbounded'}")
    if "saber" in tools:
        print("Saber remains a distinct system baseline; common k aligns only "
              "the call-string bound, not other analysis semantics.")
    print(f"Results: {args.results}")
    if args.max_workers > 1:
        print("Parallel runs may contend for CPU and memory; compare timings with care.")
    with concurrent.futures.ThreadPoolExecutor(
            max_workers=args.max_workers) as executor:
        futures = {
            executor.submit(execute_task, index, task, args.timeout,
                            args.mem_limit_gb, binaries,
                            context_limit): index
            for index, task in enumerate(tasks)
        }
        for future in concurrent.futures.as_completed(futures):
            index = futures[future]
            record = future.result()
            records[index] = record
            save_results(args.results, records)
            print(
                f"[{record['status']:12s}] {record['tool']:12s} "
                f"{record['bug_type']:16s} {record['bc']:16s} "
                f"mode={record['smt_mode']:9s} "
                f"time={record['wall_time_s']:8.3f}s "
                f"rss={record['max_rss_mb']:8.1f}MiB "
                f"result={record.get('check_result') or '-':8s} "
                f"findings={record.get('findings')}",
                flush=True,
            )
    print(f"Done. Results saved to {args.results}")
    return 0 if all(record["status"] == "SUCCESS" for record in records) else 1


if __name__ == "__main__":
    sys.exit(main())
