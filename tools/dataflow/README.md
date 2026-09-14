# Dataflow tools

This directory contains command-line frontends for Lotus dataflow engines in
`lib/Dataflow/`, including APA/elimination-style analyses, Mono analyses, IFDS
analyses, NPA analyses, WPDS analyses, and a differential-testing driver.

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
| `lotus-dfa-npa` | NPA analysis driver | Runs serial NPA intraprocedural and selected interprocedural analyses on LLVM bitcode. |
| `lotus-dfa-wpds` | WPDS analysis driver | Selects the legacy, WALi FWPDS, or WALi SWPDS backend and reports stage statistics. |

## Diff testing (`lotus-dfa`)

`lotus-dfa` runs overlapping dataflow analyses on the same LLVM bitcode and
dumps results in a **canonical format** so they can be compared with tools such
as `diff`. This supports differential testing: generate random C, compile to
bitcode, run multiple engines, and compare their outputs to find discrepancies.

## Engines and overlap

| Analysis        | Elimination | Mono | WPDS |
|----------------|-------------|------|------|
| Reachable      | `-elim-reachable` | `runIntraMonoReachability` | — |
| Uninit vars    | `-elim-uninit` | `runIntraMonoUninitializedVariables` | `runUninitializedVariablesAnalysis` |
| Reaching defs  | `-elim-rd`  | —    | —    |
| Constant prop  | `-elim-constprop` | (Inter)Mono constant prop | WPDS constant prop |

The driver exposes the forward analyses implemented by APA and runs each
selected engine that supports the requested analysis.

## Usage

```bash
# Run available engines and write canonical results into OUT_DIR
lotus-dfa --analysis=uninitialized --engine=all --out-dir=/tmp/dfa /path/to/file.bc

# Run APA with a specific elimination backend
lotus-dfa-apa --analysis=reachable --elim-method=adt-simple --stdout /path/to/file.bc

# Run only one engine (for debugging)
lotus-dfa --analysis=reachable --engine=elim --out-dir=/tmp/dfa /path/to/file.bc
```

By default these tools stay quiet unless you provide `--out-dir`. Pass
`--stdout` to print analysis results to the terminal explicitly.

## Engine-specific tools

```bash
# APA / elimination-based driver
lotus-dfa-apa --analysis=reachable --elim-method=adt-simple --stdout /path/to/file.bc

# Mono-based driver
lotus-dfa-mono --analysis=liveness --stdout /path/to/file.bc

# IFDS driver
lotus-dfa-ifds --analysis=taint --stdout /path/to/file.bc

# NPA driver
lotus-dfa-npa --analysis=liveness --solver=newton --stdout /path/to/file.bc

# WPDS backend selection and cold-stage statistics
lotus-dfa-wpds --analysis=liveness --wpds-backend=wali-swpds \
  --wpds-stats /path/to/file.bc

# Reuse one prepared WPDS model for ten distinct liveness boundary seeds
lotus-dfa-wpds --analysis=liveness --wpds-backend=wali-swpds \
  --wpds-query-count=10 --wpds-stats --summary-only /path/to/file.bc

# Sparse fixed-seed Newton rounds (also: static, always_maybe, dense)
lotus-dfa-npa --analysis=liveness --solver=newton \
  --newton-round=sparse --stdout /path/to/file.bc

# Also print the potentially large per-block fact listing.
lotus-dfa-npa --analysis=liveness --solver=newton \
  --newton-round=sparse --stdout --print-block-results /path/to/file.bc

# NPA interprocedural constant propagation
lotus-dfa-npa --analysis=constant_prop --stdout /path/to/file.bc

```

`lotus-dfa-npa` executes all solver, function, and interprocedural scheduling
serially. A shared Lotus dependency may still register the process-wide
`-nworkers` option, but NPA does not consult it.

Currently exposed NPA analyses:

- Intraprocedural: `liveness`, `reaching_defs`, `reachable`
- Interprocedural: `inter_liveness`, `inter_reaching_defs`,
  `inter_uninitialized`, `constant_prop`, `interval`,
  `nullability`

`--solver={newton,kleene}` applies to the intraprocedural analyses. The
module-level interprocedural clients use Newton and currently reject
`--solver=kleene`. `--linear-solver={scc,adaptive_scc,tensor}` selects the
Newton linearized-system solver for both intraprocedural Newton runs and
module-level interprocedural runs.
`--newton-round={dense,static,always_maybe,sparse}` independently selects how
each Newton round is constructed. The non-dense choices require an idempotent
domain. The driver emits aggregate and per-round occurrence, active-coordinate,
materialization, discovery, and linear-solve profiles. Per-block facts are
omitted by default; pass `--print-block-results` to include them.

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
