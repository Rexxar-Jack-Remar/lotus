#!/usr/bin/env python3
"""Evaluate UseHistory vs Saber on SPEC2017 benchmarks.

Enumerates every (bug-type × tool × benchmark) combination.
Shared bug types: double-free, use-after-free.
Saber additionally supports: memory-leak, file-leak.

Usage:
    python3 scripts/evaluate_usehistory_vs_saber.py [--max-workers N]
                                                     [--timeout SECS]
                                                     [--mem-limit-gb GB]
                                                     [--benchmarks-dir DIR]
                                                     [--results FILE]
"""
import argparse
import concurrent.futures
import glob
import json
import os
import re
import signal
import subprocess
import sys
import threading
import time

USEHISTORY_BIN = "build/bin/lotus-ir-usehistory"
LOTUS_CHECK_BIN = "build/bin/lotus-check"

BUG_TYPES = [
    # (bug_type, supported_by_usehistory, supported_by_saber)
    ("double-free",    True,  True),
    ("use-after-free", True,  True),
    ("memory-leak",    False, True),
    ("file-leak",      False, True),
]

SABER_SMT_MODES = [
    ("smt",   []),
    ("nosmt", ["--saber.no-smt"]),
]

results_lock = threading.Lock()
all_results = []


def get_process_rss_mb(pid):
    try:
        out = subprocess.check_output(
            ["ps", "-o", "rss=", "-p", str(pid)], stderr=subprocess.DEVNULL
        ).decode().strip()
        if out:
            return float(out) / 1024.0
    except Exception:
        pass
    return 0.0


def kill_proc_group(proc):
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass


def run_cmd_with_limits(cmd, timeout_s, mem_limit_gb):
    start = time.time()
    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, preexec_fn=os.setsid,
    )

    max_rss_mb = 0.0
    killed_reason = None

    while proc.poll() is None:
        elapsed = time.time() - start
        if elapsed > timeout_s:
            killed_reason = "TIMEOUT"
            kill_proc_group(proc)
            break
        rss = get_process_rss_mb(proc.pid)
        max_rss_mb = max(max_rss_mb, rss)
        if rss > mem_limit_gb * 1024.0:
            killed_reason = f"OOM(>{mem_limit_gb}GB)"
            kill_proc_group(proc)
            break
        time.sleep(0.5)

    try:
        stdout, stderr = proc.communicate(timeout=5)
    except Exception:
        kill_proc_group(proc)
        stdout, stderr = "", ""

    wall_time = time.time() - start
    if killed_reason:
        status = killed_reason
    elif proc.returncode != 0:
        status = f"ERROR(rc={proc.returncode})"
    else:
        status = "SUCCESS"

    return {
        "status": status,
        "wall_time_s": round(wall_time, 2),
        "max_rss_mb": round(max_rss_mb, 2),
        "stdout": stdout,
        "stderr": stderr,
    }


def parse_usehistory_output(stdout, stderr):
    result = {
        "svfg_nodes": None, "svfg_edges": None,
        "history_nodes": None, "issues": None,
        "check_result": None, "findings": 0, "finding_sites": 0,
        "parse_ms": None, "svfg_ms": None,
        "usehistory_ms": None, "check_ms": None, "total_ms": None,
    }
    for line in stdout.splitlines():
        m = re.search(
            r"svfg_nodes=(\d+)\s+svfg_edges=(\d+)\s+history_nodes=(\d+)\s+issues=(\d+)",
            line,
        )
        if m:
            result["svfg_nodes"] = int(m.group(1))
            result["svfg_edges"] = int(m.group(2))
            result["history_nodes"] = int(m.group(3))
            result["issues"] = int(m.group(4))
        m = re.search(r"result=(\w+)\s+findings=(\d+)\s+finding_sites=(\d+)", line)
        if m:
            result["check_result"] = m.group(1)
            result["findings"] = int(m.group(2))
            result["finding_sites"] = int(m.group(3))
    for line in stderr.splitlines():
        m = re.search(
            r"parse=([\d.]+)\s+svfg=([\d.]+)\s+usehistory=([\d.]+)\s+check=([\d.]+)\s+total=([\d.]+)",
            line,
        )
        if m:
            result["parse_ms"] = float(m.group(1))
            result["svfg_ms"] = float(m.group(2))
            result["usehistory_ms"] = float(m.group(3))
            result["check_ms"] = float(m.group(4))
            result["total_ms"] = float(m.group(5))
    return result


def parse_saber_output(stdout, stderr):
    result = {
        "elapsed_ms": None, "functions": None, "instructions": None,
        "findings": 0,
    }
    for line in stdout.splitlines():
        m = re.search(r"elapsed-ms:\s+(\d+)", line)
        if m:
            result["elapsed_ms"] = int(m.group(1))
        m = re.search(r"functions:\s+(\d+)", line)
        if m:
            result["functions"] = int(m.group(1))
        m = re.search(r"instructions:\s+(\d+)", line)
        if m:
            result["instructions"] = int(m.group(1))
    m = re.search(r"Findings \((\d+)\)", stdout)
    if m:
        result["findings"] = int(m.group(1))
    return result


def build_cmd(tool, bug_type, smt_mode, bc_path):
    if tool == "usehistory":
        return [USEHISTORY_BIN, bc_path, "--quiet", "--timing",
                f"--check={bug_type}"]
    # tool == "saber"
    cmd = [LOTUS_CHECK_BIN, "--engine=saber", f"--checks={bug_type}",
           "--analysis-stats"]
    for flag in smt_mode:
        cmd.append(flag)
    cmd.append(bc_path)
    return cmd


def execute_task(task, timeout_s, mem_limit_gb, results_file):
    tool, bug_type, smt_label, smt_flags, bc_path = task
    bc_name = os.path.basename(bc_path)
    cmd = build_cmd(tool, bug_type, smt_flags, bc_path)

    exec_res = run_cmd_with_limits(cmd, timeout_s, mem_limit_gb)

    if tool == "usehistory":
        parsed = parse_usehistory_output(exec_res["stdout"], exec_res["stderr"])
    else:
        parsed = parse_saber_output(exec_res["stdout"], exec_res["stderr"])

    mode_label = f"{bug_type}" if tool == "usehistory" else f"{bug_type}/{smt_label}"
    record = {
        "tool": tool,
        "bug_type": bug_type,
        "smt_mode": smt_label if tool == "saber" else None,
        "bc": bc_name,
        "status": exec_res["status"],
        "wall_time_s": exec_res["wall_time_s"],
        "max_rss_mb": exec_res["max_rss_mb"],
        **parsed,
    }

    findings = record.get("findings", 0)
    print(
        f"[{record['status']:12s}] {tool:12s} {mode_label:22s} {bc_name:16s} "
        f"time={record['wall_time_s']:7.2f}s  rss={record['max_rss_mb']:7.1f}MB  "
        f"findings={findings}",
        flush=True,
    )

    with results_lock:
        all_results.append(record)
        with open(results_file, "w") as f:
            json.dump(all_results, f, indent=2)

    return record


def main():
    parser = argparse.ArgumentParser(
        description="Evaluate UseHistory vs Saber on SPEC2017 benchmarks")
    parser.add_argument("--max-workers", type=int, default=4,
                        help="Number of parallel workers (default: 4)")
    parser.add_argument("--timeout", type=int, default=300,
                        help="Per-run timeout in seconds (default: 300)")
    parser.add_argument("--mem-limit-gb", type=float, default=6.0,
                        help="Per-process memory limit in GB (default: 6.0)")
    parser.add_argument("--benchmarks-dir", type=str,
                        default="benchmarks/real-world/SPEC2017",
                        help="Directory containing benchmark bitcode files")
    parser.add_argument("--results", type=str, default="eval_results.json",
                        help="Output JSON results file (default: eval_results.json)")
    args = parser.parse_args()

    benchmarks = sorted(glob.glob(f"{args.benchmarks_dir}/*"))
    if not benchmarks:
        print(f"error: no benchmarks found in {args.benchmarks_dir}",
              file=sys.stderr)
        return 1

    print("=== UseHistory vs Saber Evaluation ===")
    print(f"Benchmarks:    {len(benchmarks)} files in {args.benchmarks_dir}")
    print(f"Max workers:   {args.max_workers}")
    print(f"Timeout:       {args.timeout}s")
    print(f"Memory limit:  {args.mem_limit_gb} GB")
    print(f"Results file:  {args.results}")
    print()

    # Build the full task matrix: bug_type × tool(+smt_mode) × benchmark.
    tasks = []
    for bug_type, has_usehistory, has_saber in BUG_TYPES:
        for bc in benchmarks:
            if has_usehistory:
                tasks.append(("usehistory", bug_type, None, [], bc))
            if has_saber:
                for smt_label, smt_flags in SABER_SMT_MODES:
                    tasks.append(("saber", bug_type, smt_label, smt_flags, bc))

    print(f"Total tasks:   {len(tasks)}")
    print()

    with concurrent.futures.ThreadPoolExecutor(
        max_workers=args.max_workers
    ) as executor:
        futures = [
            executor.submit(execute_task, t, args.timeout,
                            args.mem_limit_gb, args.results)
            for t in tasks
        ]
        concurrent.futures.wait(futures)

    print(f"\nDone. Results saved to {args.results}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
