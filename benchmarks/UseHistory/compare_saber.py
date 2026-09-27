#!/usr/bin/env python3
"""Measure one double-free check per SPEC2006 process against Lotus Saber.

This records observed outcomes as well as time and memory. Different models or
Unknown results prevent a like-for-like accuracy or performance claim.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import multiprocessing as mp
from pathlib import Path
import platform
import random
import re
import statistics
import subprocess
import time
from queue import Empty

try:
    import resource
except ImportError:  # pragma: no cover - Windows does not provide it
    resource = None


HISTORY_RESULT = re.compile(
    r"check=double-free result=(Found|NotFound|Unknown) "
    r"findings=(\d+) finding_sites=(\d+) exhaustive=(yes|no)"
)
HISTORY_ISSUES = re.compile(r"\bissues=(\d+)")
SABER_FINDINGS = re.compile(r"Findings \((\d+)\)")


def _worker(command: list[str], timeout: int, queue: mp.Queue) -> None:
    started = time.perf_counter()
    try:
        completed = subprocess.run(command, text=True, capture_output=True,
                                   timeout=timeout, check=False)
        elapsed = (time.perf_counter() - started) * 1000
        rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss if resource else None
        if rss is not None and platform.system() == "Linux":
            rss *= 1024
        queue.put({"wall_ms": elapsed, "rss_bytes": rss,
                   "exit_code": completed.returncode,
                   "stdout": completed.stdout, "stderr": completed.stderr})
    except subprocess.TimeoutExpired:
        queue.put({"error": f"timeout after {timeout}s"})


def measure(command: list[str], timeout: int) -> dict:
    # A fresh worker gives each invocation its own RUSAGE_CHILDREN peak RSS.
    context = mp.get_context("spawn")
    queue = context.Queue()
    process = context.Process(target=_worker, args=(command, timeout, queue))
    process.start()
    process.join(timeout + 10)
    if process.is_alive():
        process.terminate()
        process.join()
        return {"error": "measurement worker did not finish"}
    try:
        return queue.get(timeout=2)
    except Empty:
        return {"error": f"measurement worker exited {process.exitcode}"}


def summarize(tool: str, measurement: dict) -> dict:
    if "error" in measurement:
        return measurement
    stdout = measurement.pop("stdout")
    stderr = measurement.pop("stderr")
    if measurement["exit_code"]:
        return {**measurement, "error": (stderr or stdout)[-1200:]}
    if tool == "usehistory":
        result = HISTORY_RESULT.search(stdout)
        issues = HISTORY_ISSUES.search(stdout)
        if not result or not issues:
            return {**measurement, "error": "unrecognized UseHistory output"}
        return {**measurement, "result": result.group(1),
                "findings": int(result.group(2)),
                "finding_sites": int(result.group(3)),
                "exhaustive": result.group(4) == "yes",
                "issues": int(issues.group(1))}
    findings = SABER_FINDINGS.search(stdout)
    if findings:
        count = int(findings.group(1))
    elif "No findings." in stdout:
        count = 0
    else:
        return {**measurement, "error": "unrecognized Saber output"}
    return {**measurement, "findings": count}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="*", type=Path,
                        help="SPEC2006 .bc files (default: all non-specrand files)")
    parser.add_argument("--usehistory-tool", type=Path,
                        default=Path("build/bin/lotus-ir-usehistory"))
    parser.add_argument("--saber-tool", type=Path,
                        default=Path("build/bin/lotus-check"))
    parser.add_argument("--saber-no-smt", action="store_true",
                        help="Use Saber's conservative no-SMT mode")
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--output", type=Path,
                        default=Path("/tmp/usehistory-spec-saber.json"))
    args = parser.parse_args()
    if args.runs < 1 or args.warmups < 0 or args.timeout < 1:
        parser.error("runs and timeout must be positive; warmups must be nonnegative")
    inputs = args.inputs or sorted(Path("benchmarks/real-world/SPEC2006").glob("*.bc"))
    inputs = [path.resolve(strict=True) for path in inputs
              if "specrand" not in path.name]
    usehistory = args.usehistory_tool.resolve(strict=True)
    saber = args.saber_tool.resolve(strict=True)
    rng = random.Random(0)
    report = {"property": "double-free", "platform": platform.platform(),
              "runs": args.runs, "warmups": args.warmups,
              "saber_no_smt": args.saber_no_smt,
              "tools": {"usehistory": str(usehistory), "saber": str(saber)},
              "programs": []}
    for path in inputs:
        entry = {"input": str(path),
                 "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                 "samples": {"usehistory": [], "saber": []}}
        commands = {
            "usehistory": [str(usehistory), str(path), "--check=double-free",
                           "--quiet"],
            "saber": [str(saber), "--engine=saber", str(path),
                      "--checks=double-free", "--log-level=error",
                      "--saber.solver-timeout-ms=10000"],
        }
        if args.saber_no_smt:
            commands["saber"].append("--saber.no-smt")
        failed = False
        for trial in range(args.warmups + args.runs):
            order = ["usehistory", "saber"]
            rng.shuffle(order)
            for tool in order:
                sample = summarize(tool, measure(commands[tool], args.timeout))
                if trial >= args.warmups:
                    entry["samples"][tool].append(sample)
                if "error" in sample:
                    failed = True
                    break
            if failed:
                break
        entry["median_wall_ms"] = {}
        entry["median_rss_bytes"] = {}
        entry["failed_runs"] = {}
        for tool, samples in entry["samples"].items():
            valid = [sample for sample in samples if "error" not in sample]
            entry["failed_runs"][tool] = sum("error" in sample for sample in samples)
            if len(valid) == args.runs:
                entry["median_wall_ms"][tool] = statistics.median(
                    sample["wall_ms"] for sample in valid)
                memory = [sample["rss_bytes"] for sample in valid
                          if sample["rss_bytes"] is not None]
                if memory:
                    entry["median_rss_bytes"][tool] = statistics.median(memory)
        if all(tool in entry["median_wall_ms"] for tool in commands):
            entry["wall_ratio_saber_over_usehistory"] = (
                entry["median_wall_ms"]["saber"] /
                entry["median_wall_ms"]["usehistory"]
            )
        if all(tool in entry["median_rss_bytes"] for tool in commands):
            entry["rss_ratio_saber_over_usehistory"] = (
                entry["median_rss_bytes"]["saber"] /
                entry["median_rss_bytes"]["usehistory"]
            )
        history_outcomes = {sample["result"] for sample in
                            entry["samples"]["usehistory"] if "result" in sample}
        history_counts = {sample["findings"] for sample in
                          entry["samples"]["usehistory"] if "findings" in sample}
        history_sites = {sample["finding_sites"] for sample in
                         entry["samples"]["usehistory"] if "finding_sites" in sample}
        saber_outcomes = {sample["findings"] for sample in
                          entry["samples"]["saber"] if "findings" in sample}
        entry["outcomes"] = {"usehistory": sorted(str(x) for x in history_outcomes),
                             "usehistory_findings": sorted(str(x) for x in history_counts),
                             "usehistory_sites": sorted(str(x) for x in history_sites),
                             "saber_findings": sorted(str(x) for x in saber_outcomes)}
        entry["alarm_presence_agrees"] = (
            (history_outcomes == {"Found"} and saber_outcomes and
             all(isinstance(count, int) and count > 0 for count in saber_outcomes))
            or (history_outcomes == {"NotFound"} and saber_outcomes == {0})
        )
        entry["alarm_count_agrees"] = history_sites == saber_outcomes
        entry["same_findings_verified"] = False
        report["programs"].append(entry)
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"{path.name}: UseHistory {entry['outcomes']['usehistory']} "
              f"{entry['median_wall_ms'].get('usehistory', '?')} ms; "
              f"Saber {entry['outcomes']['saber_findings']} "
              f"{entry['median_wall_ms'].get('saber', '?')} ms "
              f"(failed runs: {entry['failed_runs']})", flush=True)
    print(f"Saved {args.output}")


if __name__ == "__main__":
    main()
