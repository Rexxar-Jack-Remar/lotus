# Checker tools

This directory now builds a single checker frontend, `lotus-check`, on top of
`lib/Checker/` and related Lotus analysis infrastructure.

`lotus-check` supports:

- top-level generic/declarative checking
- native in-process checker subcommands such as `ae`, `kint`, `pulse`,
  `saber`, `fitx`, `concur`, `symex`, and `taint`

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

Use `build/bin/lotus-check --help` or
`build/bin/lotus-check <subcommand> --help` to inspect the full option set.

## Subcommands

| Subcommand | Purpose | Notes |
| --- | --- | --- |
| `generic` | Generic checker driver | Resolves specs from `--spec-dir`, `LOTUS_CHECKER_SPEC_DIR`, the installed data directory, or the source tree (in that order). Top-level invocation without a subcommand also uses this mode. |
| `kint` | Integer bug detector | Checks integer overflow, division by zero, bad shifts, array bounds, and dead branches. Supports enabling all checks or individual `--check-*` options. |
| `taint` | Interprocedural taint analysis | IFDS-based taint tracking with selectable alias analysis via `--aa` and optional micro-benchmark evaluation mode. |
| `concur` | Concurrency checker | Detects races, deadlocks, atomicity issues, condvar misuse, lock mismatches, and OpenMP/MPI bugs. |
| `pulse` | Pulse-inspired bug finder | Biabductive analysis with optional SMT disabling via `--no-smt`; can emit JSON findings. |
| `fitx` | FiTx multi-checker driver | Runs FiTx detectors such as `df`, `dl`, `dul`, `leak`, `nullptr`, `uaf`, `ubi`, and reference-count checkers. |
| `saber` | Source-sink bug checker | Runs memory leak, double-free, and file-descriptor leak checks; defaults to leak checking when no specific checker is selected. Implemented by `tools/checker/lotus-check-saber.cpp`. |
| `ae` | Abstract-execution checker | Covers overflow, null dereference, use-after-free, invalid free, and memory leak detection. Implemented by `tools/checker/lotus-check-ae.cpp`. |
| `symex` | Symbolic-execution checker | Runs the `lib/SymbolicExecution` engine on GVFG/LotusAA and emits path-sensitive bug reports. Implemented by `tools/checker/lotus-check-symex.cpp`. |

## Common workflows

### Use the unified frontend

```bash
build/bin/lotus-check --list-checkers
build/bin/lotus-check --checker=forbidden.system test.bc
build/bin/lotus-check generic test.bc --checker=forbidden.system
build/bin/lotus-check ae test.bc --all
build/bin/lotus-check concur test.bc --checks=race,deadlock
```

### Run Kint on integer-heavy code

```bash
build/bin/lotus-check kint test.bc --check-all=true
```

### Run taint analysis with explicit sources and sinks

```bash
build/bin/lotus-check taint test.bc \
  --aa=dyck \
  --sources=recv,getenv \
  --sinks=system,execve \
  --show-results
```

### Run targeted bug checkers

```bash
build/bin/lotus-check saber test.bc --all
build/bin/lotus-check ae test.bc --all
build/bin/lotus-check symex test.bc --symex-checkers=null-deref,uaf
build/bin/lotus-check fitx test.bc --detector=uaf
build/bin/lotus-check concur test.bc --checks=race,deadlock
build/bin/lotus-check pulse test.bc --json-output pulse.json
```

## Reporting

- Most checkers support shared report-output flags such as `--report-json` and
  `--report-sarif` through the common report manager.
- Several tools also support suppression files and confidence filtering through
  the same reporting layer.
- Successful analysis returns 0, `--fail-on-findings` returns 1 when filtered
  findings remain, and handled parameter/report I/O failures return 2.
- `lotus-pulse` accepts `--json-output` directly in addition to the common
  reporting options.

## Related documentation

- `lib/Checker/README.md` describes the checker subsystems.
- `tools/alias/README.md` documents the alias-analysis drivers used by some
  checker pipelines.
