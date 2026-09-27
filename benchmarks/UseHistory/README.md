# Single-property UseHistory measurements

## SPEC2006 versus Saber

Run one double-free check per process on the same SPEC2006 bitcode. The
comparison records findings, `Unknown` results, elapsed process time and
peak resident memory. Results are written after each program so partial runs
can be inspected.
The default JSON output is under ``/tmp``. Local files in this benchmark's
``results/`` directory are ignored by Git.

```sh
python3 benchmarks/UseHistory/compare_saber.py \
  benchmarks/real-world/SPEC2006/429.mcf.bc \
  benchmarks/real-world/SPEC2006/401.bzip2.bc \
  --runs=3 --output=/tmp/usehistory-spec-saber.json
```

To compare Saber with and without path-condition solving, run the same
single-property experiment with ``--saber-no-smt``. The no-SMT mode preserves
Saber's source/sink traversal but treats its candidates conservatively:

```sh
python3 benchmarks/UseHistory/compare_saber.py \
  benchmarks/real-world/SPEC2006/429.mcf.bc \
  benchmarks/real-world/SPEC2006/433.milc.bc \
  --saber-no-smt --runs=3 --output=/tmp/usehistory-spec-no-smt.json
```

This mode also skips branch-condition preparation and the path-condition
propagation stage, so its timing difference is **not** an isolated measurement
of Z3 solver calls.

In the initial pilot, no-SMT Saber took 90.8 ms on `mcf` (default: 96.3 ms)
and 5.32 s on `milc` (default: 13.32 s). On `bzip2`, two successful no-SMT
runs took 1.41-1.42 s and reported two candidates (default: 8.36 s, no
reports); another run crashed during SVFG construction. A subsequent three-run
repeat completed without a crash, but the earlier failure remains recorded.
This is a path-condition ablation, not a precision-equivalent comparison with
UseHistory. A default
Saber/UseHistory comparison also includes differences in SVFG construction,
resource modeling, and path feasibility handling.
