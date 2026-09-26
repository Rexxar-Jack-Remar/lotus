# Dynamic Bidirected Dyck Benchmarks

The `dacapo_bench/` and `tal/` inputs are copied from the C++ benchmark corpus
of the [POPL 2022 dynamic bidirected Dyck implementation](https://github.com/yuanboli233/dynamic_bidirected_dyck).
The data retain the original filenames and contents, including incremental,
decremental, and mixed update sequences. The duplicate nested DaCapo copy
from the source archive is omitted; both original C++ table scripts use the
top-level copy. Unused `backup/` snapshots are also omitted; neither table
references them. `init.dot` is the original empty initial graph.

Build the Lotus tool and run the original table scripts directly:

```sh
cmake --build build --target lotus-cfl-dynamic-dyck
bash scripts/cfl/dynamic-dyck/gen_table3.sh
bash scripts/cfl/dynamic-dyck/gen_table4.sh
```

The scripts resolve data relative to the repository and use
`build/bin/lotus-cfl-dynamic-dyck` by default, so they also work from another
working directory. `DYCK_REACH_BINARY` and `DYCK_BENCHMARK_ROOT` select a
different executable or corpus. Their case order, modes, stdout, and timing
scopes match the original workflow. Table 3 has 26 cases and four runs per case
(incremental dynamic, incremental recompute, decremental dynamic, decremental
recompute). Table 4
has 25 cases, five initial ratios, and two modes per ratio. These are 354
invocations in total. Large full recomputation workloads can take substantial
time; use a Release build when measuring performance.

From the repository root, the optional runner selects subsets in the same
table ordering, validates input paths, and compares final partitions:

```sh
python3 benchmarks/real-world/CFL/DynamicDyck/run_benchmarks.py --table 3 --dry-run
python3 benchmarks/real-world/CFL/DynamicDyck/run_benchmarks.py --table 4 --dry-run
python3 benchmarks/real-world/CFL/DynamicDyck/run_benchmarks.py \
  --table 3 --cases helloworld --verify
python3 benchmarks/real-world/CFL/DynamicDyck/run_benchmarks.py \
  --table 4 --cases helloworld --ratios 10 20 30 40 50 --verify
```

`--binary PATH` selects an executable from another build, and
`--benchmark-root PATH` selects a different corpus. `--mode dynamic`
or `--mode recompute` selects one mode. `--verify` requires both modes and
compares sorted final partitions; printed timings still use the original
measured scopes. Timing values depend on the compiler, build type, hardware,
and the documented correctness fixes; reproducing the publication's numerical
speedups is not asserted.

This corpus and workflow cover the original C++ implementation. The DDlog
comparison implementation and its drivers are not included.
