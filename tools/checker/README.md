# Checker tools

This directory contains standalone bug-finding frontends built on top of
`lib/Checker/` and related Lotus analysis infrastructure.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Binaries are emitted under `build/bin/`.

## Input format

All checker tools consume LLVM bitcode or textual LLVM IR.

```bash
clang -emit-llvm -c test.c -o test.bc
build/bin/<checker> test.bc
```

Use `build/bin/<checker> --help` to inspect the full option set.

## Tools

| Tool | Purpose | Notes |
| --- | --- | --- |
| `lotus-kint` | Integer bug detector | Checks integer overflow, division by zero, bad shifts, array bounds, and dead branches. Supports enabling all checks or individual `--check-*` options. |
| `lotus-taint` | Interprocedural taint analysis | IFDS-based taint tracking with selectable alias analysis via `--aa` and optional micro-benchmark evaluation mode. |
| `lotus-concur` | Concurrency checker | Detects races, deadlocks, atomicity issues, condvar misuse, lock mismatches, and OpenMP/MPI bugs. |
| `lotus-pulse` | Pulse-inspired bug finder | Biabductive analysis with optional SMT disabling via `--no-smt`; can emit JSON findings. |
| `lotus-fitx` | FiTx multi-checker driver | Runs FiTx detectors such as `df`, `dl`, `dul`, `leak`, `nullptr`, `uaf`, `ubi`, and reference-count checkers. |
| `lotus-saber` | Source-sink bug checker | Runs memory leak, double-free, and file-descriptor leak checks; defaults to leak checking when no specific checker is selected. |
| `lotus-ae` | Abstract-execution checker | Covers overflow, null dereference, use-after-free, invalid free, and memory leak detection. |
| `lotus-symex` | Symbolic-execution checker | Runs the `lib/Analysis/SymbolicExecution` engine on GVFG/LotusAA and emits path-sensitive bug reports. |
| `lotus-check` | Generic checker driver | Loads built-in YAML specs from `config/checkers/` and runs declarative checkers through the shared report manager. |

## Common workflows

### Run Kint on integer-heavy code

```bash
build/bin/lotus-kint test.bc --check-all=true
```

### Run taint analysis with explicit sources and sinks

```bash
build/bin/lotus-taint test.bc \
  --aa=dyck \
  --sources=recv,getenv \
  --sinks=system,execve \
  --show-results
```

### Run targeted bug checkers

```bash
build/bin/lotus-saber test.bc --all
build/bin/lotus-ae test.bc --all
build/bin/lotus-symex test.bc --symex-checkers=null-deref,uaf
build/bin/lotus-fitx test.bc --detector=uaf
build/bin/lotus-concur test.bc --checks=race,deadlock
build/bin/lotus-pulse test.bc --json-output pulse.json
```

## Reporting

- Most checkers support shared report-output flags such as `--report-json` and
  `--report-sarif` through the common report manager.
- Several tools also support suppression files and confidence filtering through
  the same reporting layer.
- `lotus-pulse` accepts `--json-output` directly in addition to the common
  reporting options.

## Related documentation

- `lib/Checker/README.md` describes the checker subsystems.
- `tools/alias/README.md` documents the alias-analysis drivers used by some
  checker pipelines.
