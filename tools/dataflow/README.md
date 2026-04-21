# Dataflow tools

This directory contains command-line frontends for Lotus dataflow engines in
`lib/Dataflow/`, including APA/elimination-style analyses, Mono analyses, IFDS
analyses, NPA analyses, and a differential-testing driver.

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
| `lotus-dfa-npa` | NPA analysis driver | Runs NPA intraprocedural and selected interprocedural analyses on LLVM bitcode, with module-level function scheduling and eligible NPA parallel execution via `-nworkers`. |

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
lotus-dfa-apa --analysis=liveness --elim-method=adt-simple --stdout /path/to/file.bc

# Run only one engine (for debugging)
lotus-dfa --analysis=liveness --engine=elim --out-dir=/tmp/dfa /path/to/file.bc
```

By default these tools stay quiet unless you provide `--out-dir`. Pass
`--stdout` to print analysis results to the terminal explicitly.

## Engine-specific tools

```bash
# APA / elimination-based driver
lotus-dfa-apa --analysis=liveness --elim-method=adt-simple --stdout /path/to/file.bc

# Mono-based driver
lotus-dfa-mono --analysis=liveness --stdout /path/to/file.bc

# IFDS driver
lotus-dfa-ifds --analysis=taint --stdout /path/to/file.bc

# NPA driver
lotus-dfa-npa --analysis=liveness --solver=newton --stdout /path/to/file.bc

# NPA interprocedural constant propagation
lotus-dfa-npa --analysis=constant_prop --stdout /path/to/file.bc

# NPA driver with module-level and NPA-internal parallel execution
lotus-dfa-npa --analysis=liveness --solver=newton --linear-solver=scc -nworkers=8 --stdout /path/to/file.bc
```

`lotus-dfa-npa` inherits the global `ThreadPool` flag `-nworkers=<N>`.
For intraprocedural analyses, the frontend schedules independent functions
across the module in parallel and each NPA solve may additionally use the
engine's internal parallel setup/SCC execution paths when the problem
structure is eligible.

For module-level interprocedural analyses, scheduling lives inside the
interprocedural engine rather than in the CLI driver. `-nworkers=0` and
`-nworkers=1` both stay on the sequential path.

Currently exposed NPA analyses:

- Intraprocedural: `liveness`, `reaching_defs`, `reachable`
- Interprocedural: `inter_liveness`, `inter_reaching_defs`,
  `inter_uninitialized`, `constant_prop`, `interval`, `affine_eqs`,
  `nullability`

`--solver={newton,kleene}` applies to the intraprocedural analyses. The
module-level interprocedural clients use Newton and currently reject
`--solver=kleene`. `--linear-solver={scc,adaptive_scc,tensor}` selects the
Newton linearized-system solver for both intraprocedural Newton runs and
module-level interprocedural runs.

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
