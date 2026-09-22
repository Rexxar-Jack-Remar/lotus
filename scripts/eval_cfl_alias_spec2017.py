#!/usr/bin/env python3
import os
import sys
import glob
import json
import time
import signal
import subprocess
from concurrent.futures import ProcessPoolExecutor, as_completed

BENCHMARKS_DIR = os.path.abspath("benchmarks/real-world/SPEC2017")
BINARY = os.path.abspath("./build/bin/lotus-cfl-alias")
OUTPUT_FILE = os.path.abspath("tmp/spec2017_cfl_alias_results.json")
TIMEOUT_SECONDS = 300

SOLVERS = ["sqid", "cat", "endpoint-quotient", "cert-cfl"]

def get_benchmarks():
    files = sorted(glob.glob(os.path.join(BENCHMARKS_DIR, "*")))
    res = []
    for f in files:
        if os.path.basename(f).startswith("."):
            continue
        res.append(f)
    return res

def run_single(benchmark_path, solver):
    bench_name = os.path.basename(benchmark_path)
    cmd = [
        "/usr/bin/time", "-l",
        BINARY,
        "--solver", solver,
        "--json-stats",
        benchmark_path
    ]
    
    start_time = time.time()
    timed_out = False
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        preexec_fn=os.setsid
    )
    
    stdout, stderr = "", ""
    try:
        stdout, stderr = proc.communicate(timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except Exception:
            pass
        stdout, stderr = proc.communicate()
    
    elapsed = time.time() - start_time
    
    max_rss_bytes = None
    for line in stderr.splitlines():
        if "maximum resident set size" in line:
            parts = line.strip().split()
            if parts:
                try:
                    max_rss_bytes = int(parts[0])
                except ValueError:
                    pass
    
    json_stats = None
    for line in stdout.splitlines():
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            try:
                json_stats = json.loads(line)
                break
            except Exception:
                pass

    return {
        "benchmark": bench_name,
        "solver": solver,
        "timed_out": timed_out,
        "returncode": proc.returncode,
        "elapsed_seconds": round(elapsed, 2),
        "max_rss_mb": round(max_rss_bytes / (1024 * 1024), 2) if max_rss_bytes else None,
        "json_stats": json_stats,
        "error_snippet": stderr[-500:] if (proc.returncode != 0 and not timed_out) else None
    }

def main():
    os.makedirs(os.path.dirname(OUTPUT_FILE), exist_ok=True)
    benchmarks = get_benchmarks()
    tasks = []
    for b in benchmarks:
        for s in SOLVERS:
            tasks.append((b, s))
            
    print(f"Total benchmarks: {len(benchmarks)}")
    print(f"Total solvers: {len(SOLVERS)}")
    print(f"Total tasks: {len(tasks)}")
    print(f"Timeout per run: {TIMEOUT_SECONDS}s")
    
    existing_results = {}
    if os.path.exists(OUTPUT_FILE):
        try:
            with open(OUTPUT_FILE, "r") as f:
                data = json.load(f)
                for item in data.get("results", []):
                    key = (item["benchmark"], item["solver"])
                    existing_results[key] = item
            print(f"Loaded {len(existing_results)} existing results from {OUTPUT_FILE}")
        except Exception as e:
            print(f"Could not load existing results: {e}")
            
    pending_tasks = [t for t in tasks if (os.path.basename(t[0]), t[1]) not in existing_results]
    print(f"Pending tasks to run: {len(pending_tasks)}")
    
    results = list(existing_results.values())
    
    num_workers = 10
    with ProcessPoolExecutor(max_workers=num_workers) as executor:
        future_map = {
            executor.submit(run_single, b_path, solver): (os.path.basename(b_path), solver)
            for b_path, solver in pending_tasks
        }
        
        completed_count = len(existing_results)
        for future in as_completed(future_map):
            b_name, solver = future_map[future]
            try:
                res = future.result()
                results.append(res)
                completed_count += 1
                status = "TIMEOUT" if res["timed_out"] else f"DONE in {res['elapsed_seconds']}s (RSS: {res['max_rss_mb']} MB)"
                print(f"[{completed_count}/{len(tasks)}] {b_name} | {solver}: {status}", flush=True)
                
                with open(OUTPUT_FILE, "w") as f:
                    json.dump({"results": results}, f, indent=2)
            except Exception as e:
                print(f"[{b_name} | {solver}] Worker failed with exception: {e}", flush=True)

    print(f"\nAll {len(tasks)} tasks finished. Results saved to {OUTPUT_FILE}")

if __name__ == "__main__":
    main()
