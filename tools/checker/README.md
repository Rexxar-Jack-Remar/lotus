# Checker tools

This directory now builds a single checker frontend, `lotus-check`, on top of
`lib/Checker/` and related Lotus analysis infrastructure.

`lotus-check` supports:

- registry-backed generic/declarative checking
- native in-process checker engines such as `ae`, `kint`, `pulse`,
  `saber`, `fitx`, `concur`, `symex`, and `taint`

Each invocation selects exactly one engine with `--engine=<name>`. A native
engine may run several of its own checker kinds, while generic mode may run
several declarative checkers. Cross-engine orchestration such as running Pulse
and KINT in one invocation is not currently supported.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Binaries are emitted under `build/bin/`.

## Input format

All checker tools consume LLVM bitcode or textual LLVM IR.

```bash
clang -g -emit-llvm -c test.c -o test.bc
build/bin/lotus-check --engine=ae test.bc
```

Use `build/bin/lotus-check --help` or
`build/bin/lotus-check --engine=<name> --help` to inspect the full option set.

## Engines

| Engine | Purpose | Notes |
| --- | --- | --- |
| `generic` | Generic checker driver | Resolves specs from `--spec-dir`, `LOTUS_CHECKER_SPEC_DIR`, the installed data directory, or the source tree (in that order). |
| `kint` | Integer bug detector | Checks integer overflow, division by zero, bad shifts, array bounds, and dead branches. Runs all checks by default; individual `--check-*` options select a subset. |
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
build/bin/lotus-check --engine=generic --checker=forbidden.system test.bc
build/bin/lotus-check --engine=generic test.bc --checker=forbidden.system
build/bin/lotus-check --engine=ae test.bc --all
build/bin/lotus-check --engine=concur test.bc --checks=race,deadlock
```

`--list-checkers` identifies each registry entry as either `generic` or
`native-engine`. Generic entries can be passed to `--checker`; native entries
must be selected with the corresponding `--engine=<name>` value.

### Run Kint on integer-heavy code

```bash
build/bin/lotus-check --engine=kint test.bc
```

### Run taint analysis with explicit sources and sinks

```bash
build/bin/lotus-check --engine=taint test.bc \
  --aa=dyck \
  --sources=recv,getenv \
  --sinks=system,execve \
  --show-results
```

### Run targeted bug checkers

```bash
build/bin/lotus-check --engine=saber test.bc --all
build/bin/lotus-check --engine=ae test.bc --all
build/bin/lotus-check --engine=symex test.bc --symex-checkers=null-deref,uaf
build/bin/lotus-check --engine=fitx test.bc --detector=uaf
build/bin/lotus-check --engine=concur test.bc --checks=race,deadlock
build/bin/lotus-check --engine=pulse test.bc --report-json=pulse.json
```

## Reporting

- Most checkers support shared report-output flags such as `--report-json` and
  `--report-sarif` through the common report manager.
- Several tools also support suppression files and confidence filtering through
  the same reporting layer.
- Successful analysis returns 0, `--fail-on-findings` returns 1 when filtered
  findings remain, and handled parameter/report I/O failures return 2.
- Pulse uses the same `--report-json`, `--report-sarif`,
  `--report-min-score`, and `--fail-on-findings` options as the other native
  checker engines.

## Related documentation

- `lib/Checker/README.md` describes the checker subsystems.
- `tools/alias/README.md` documents the alias-analysis drivers used by some
  checker pipelines.
