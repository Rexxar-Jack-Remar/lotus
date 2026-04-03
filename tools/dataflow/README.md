# Dataflow tools

This directory contains command-line frontends for Lotus dataflow engines in
`lib/Dataflow/`, including APA/elimination-style analyses, Mono analyses, IFDS
analyses, and a differential-testing driver.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

The binaries are written to `build/bin/`.

## Tools

| Tool | Purpose | Notes |
| --- | --- | --- |
| `lotus-dfa` | Differential-testing driver | Compares overlapping analyses across engines and writes canonical output for `diff`. |
| `lotus-dfa-apa` | APA / elimination driver | Runs elimination-based dataflow analyses and can dump engine results. |
| `lotus-dfa-mono` | Mono analysis driver | Runs Mono-based analyses on LLVM bitcode. |
| `lotus-dfa-ifds` | IFDS analysis driver | Runs IFDS-based analyses with alias-analysis support when needed. |

## Diff testing (`lotus-dfa`)

`lotus-dfa` runs overlapping dataflow analyses on the same LLVM bitcode and
dumps results in a **canonical format** so they can be compared with tools such
as `diff`. This supports differential testing: generate random C, compile to
bitcode, run multiple engines, and compare their outputs to find discrepancies.

## Engines and overlap

| Analysis        | Elimination | Mono | WPDS |
|----------------|-------------|------|------|
| Liveness       | `-elim-live` | `runLiveVariablesAnalysis` | `runLivenessAnalysis` |
| Reachable      | `-elim-reachable` | `runReachableAnalysis` | — |
| Uninit vars    | `-elim-uninit` | `runUninitVariablesAnalysis` | `runUninitializedVariablesAnalysis` |
| Reaching defs  | `-elim-rd`  | —    | —    |
| Constant prop  | `-elim-constprop` | (Inter)Mono constant prop | WPDS constant prop |

Currently only **liveness** is wired for diff (Elimination vs Mono, both
intraprocedural). Other analyses can be added by extending the tool and the
canonical dump format.

## Usage

```bash
# Run both engines and write elim.txt / mono.txt into OUT_DIR
lotus-dfa --analysis=liveness --engine=both --out-dir=/tmp/dfa /path/to/file.bc

# Run APA with a specific elimination backend
lotus-dfa-apa --analysis=liveness --elim-method=adt-simple /path/to/file.bc

# Run only one engine (for debugging)
lotus-dfa --analysis=liveness --engine=elim --out-dir=/tmp/dfa /path/to/file.bc
```

## Engine-specific tools

```bash
# APA / elimination-based driver
lotus-dfa-apa --analysis=liveness --elim-method=adt-simple /path/to/file.bc

# Mono-based driver
lotus-dfa-mono --analysis=liveness /path/to/file.bc

# IFDS driver
lotus-dfa-ifds --analysis=taint /path/to/file.bc
```

## Canonical format

Per function, one line per instruction:

```
FUNC <function_name>
 inst_<id> IN: <sorted,comma-separated value ids>
```

Value ids are stable: `arg0`, `arg1`, … for arguments, then `i0`, `i1`, … for instructions in BB order. This allows a direct `diff elim.txt mono.txt` for the same bitcode.

## Fuzz script

Use the fuzz script to generate random C and run the diff:

```bash
# From repo root
./fuzz/diff_dfa.sh              # CSmith random C → compile → diff (needs CSmith)
./fuzz/diff_dfa.sh foo.c        # compile foo.c → diff
./fuzz/diff_dfa.sh foo.bc       # diff on existing bitcode
```

Bitcode must be readable by the same LLVM version used to build Lotus (for
example LLVM 14). If your system `clang` emits opaque-pointer bitcode, set
`CLANG` to the clang from that LLVM build when compiling C files.
