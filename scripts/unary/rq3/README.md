# RQ3 experiment driver

This directory runs the typed lower/upper-bound experiment from paper Section
6.4.  All dataset, solver, repetition, timeout, memory, and output policy lives
in `config.py`.

The default experiment runs the taint corpus with standalone Union-Dyck,
StagedBounds, ACF,
`G_1^+`, `G_2^+`, `G_3^+`, `G_1^circ`, `G_2^circ`, and `G_3^circ`.  Dimension
three is attempted under the common limits and is recorded as excluded when it
times out or exhausts memory.  Value-flow is configured but disabled because its
StagedBounds endpoint language must first be applied consistently to the MCFL
and ACF relations.

Run with the requested Python environment:

```sh
cmake --build build --target lotus-cfl-interleaved-dyck -j2
conda run -n py311 python scripts/unary/rq3/run_rq3.py --dry-run
conda run -n py311 python scripts/unary/rq3/run_rq3.py
conda run -n py311 python scripts/unary/rq3/aggregate_rq3.py
```

Use `--dataset`, `--benchmark`, `--experiment`, and `--restart` for pilot runs.  The runner
sets `RLIMIT_AS` from `MEMORY_LIMIT_GIB`, kills the complete process group on a
timeout, appends and fsyncs each completed CSV row, and resumes existing rows
by default.

Only the first successful measured repetition writes relations.  StagedBounds
writes sorted Union-Dyck and on-demand relations, MCFL writes a sorted relation
for every completed dimension, and ACF writes a linear-size vertex/component
map.  Later repetitions measure the solver without relation serialization.

The aggregator requires every enabled method to complete all configured
repetitions, verifies captured relation hashes, and computes

```text
L_d = Union-Dyck union G_d^+
U   = on-demand intersect ACF
```

and rejects rows that violate the required set inclusions.  It produces
`rq3-methods.csv` for method cost/size data and `rq3-bounds.csv` for certified
coverage, unresolved gap, and ACF upper-bound reduction.
