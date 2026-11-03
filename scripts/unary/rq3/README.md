# RQ3 experiment driver

RQ3 compares three sound upper approximations: Regularization, Mutual
Refinement, and ACF. Dataset, solver, repetition, timeout, memory, and output
settings live in `config.py`.

Select suites with `enabled` in `BENCHMARK_SUITES`. Standalone Union-Dyck,
the full on-demand pipeline, and MCFL are
disabled. `--method mutual-refinement` still executes its prerequisite stages,
including the internal lower-bound computation; its total cost includes them.

Run with the requested Python environment:

```sh
conda run -n py311 python scripts/unary/rq3/run_rq3.py --dry-run
conda run -n py311 python scripts/unary/rq3/run_rq3.py
conda run -n py311 python scripts/unary/rq3/aggregate_rq3.py
conda run -n py311 python scripts/unary/plots/plot_rq3_performance_heatmaps.py
```

All generated results default to `scripts/unary/rq3/data/`, controlled by
`DATA_DIRECTORY` in `config.py`:

```text
data/
├── rq3-runs.csv                 # Raw measurements and capture metadata
├── rq3-methods.csv              # Performance summary
├── rq3-bounds.csv               # Precision summary
├── log/                        # Measured and capture run logs
├── artifacts/                  # Upper-bound pairs and component maps
└── rq3-performance-heatmaps.pdf # Performance plot
```

Output directories are created automatically and `data/` is ignored by Git.
Aggregation and plotting read the configured run CSV by default.

Use `--dataset`, `--benchmark`, `--experiment`, and `--restart` for pilot runs.  The runner
sets `RLIMIT_AS` from `MEMORY_LIMIT_GIB`, kills the complete process group on a
timeout, appends and fsyncs each completed CSV row, and resumes existing rows
by default.

Selected reruns preserve CSV rows for unselected inputs/methods. Each row records
SHA-256 hashes of the input and executable, the actual command/client analysis,
and `EXTRA_ENVIRONMENT`. Resume rejects a changed identity; aggregation and
plotting reject mixed executable/environment versions or different input/client
identities for the same benchmark. Hashing happens outside measured intervals.
Keep inputs, the executable, and the execution environment fixed during a run.

All five measured repetitions omit relation export. Once they succeed, a
separate capture run writes `upper.pairs` for typed methods or `components.map`
for ACF. Typed output goes to a temporary file and is processed line by line,
so the runner does not keep the entire printed relation in memory. Capture
costs are excluded from the performance samples; its command, status, log path,
and solver statistics are recorded separately in the CSV.

`median_wall_time_seconds` covers process launch, input parsing, solving, and
normal summary output. `median_time_ms` is the tool's internal solving time,
which starts after DOT parsing. `median_peak_rss_bytes` uses the process RSS
high-water mark reported by the tool at the end of solving, including parsing;
it is not ACF's construction-payload estimate. The heatmap uses wall time and
RSS. Each median is computed independently across the five measured runs.
The timeout uses the same wall-clock start, including process launch. Killing
and reaping a timed-out process can make its recorded duration exceed the limit
slightly. The 128 GiB limit applies to virtual address space (`RLIMIT_AS`),
not RSS; reported peak RSS can therefore be below the limit on allocation failure.

Successful runs must report valid time and RSS statistics. Failed attempts
retain their wall times and status, with null solver time/RSS samples; incomplete
measurements have blank performance summaries. Capture errors are logged and
prevent a successful precision comparison. The runner exits nonzero if a
selected experiment fails. An unexplained SIGKILL is recorded as a signal,
not automatically labeled memory exhaustion.

The aggregator verifies repetitions, sample/summary consistency, configurations,
and artifact hashes. It
writes `rq3-methods.csv` with performance and relation sizes, and
`rq3-bounds.csv` with one row per typed method and benchmark:

```text
typed_upper_size    = |U|                 (Regularization or Mutual Refinement)
intersection_size  = |U intersect ACF|
newly_excluded_size = |U| - |U intersect ACF|
upper_reduction    = newly_excluded_size / |U|
```

An empty typed upper bound has a blank reduction ratio. Missing or failed
methods produce incomplete comparisons and a nonzero aggregator exit status;
the other typed method can still be compared with ACF independently. No lower
bound, certification ratio, or exactness claim is reported. ACF's standalone
relation size covers the whole relaxed graph and should not be compared
directly with client-filtered typed relation sizes.

The heatmap reads `rq3-runs.csv` directly (override with `--runs PATH`) and shows
the new three methods for the benchmarks present in that file. Successful cells
and solved counts require all configured performance repetitions; resource
failures are shown as TO/MO. Relation-capture failure does not erase valid
performance measurements: `rq3-methods.csv` records performance `status` and
`capture_status` separately, with no relation size when capture failed. Precision
comparisons additionally require successful capture. Plotting and aggregation
use the same performance checks.
