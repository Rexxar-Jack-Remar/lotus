# Solver tools

This directory contains standalone frontends for SMT- and SAT-related solver
experiments in `lib/Solvers/`.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

At the moment, `tools/solver/CMakeLists.txt` builds `owl` by default. The
`slot.cpp` and `staub.cpp` sources remain in the tree as experimental tools and
are not wired into the default build yet.

## Tools

| Tool | Status | Purpose |
| --- | --- | --- |
| `owl` | built by default | Solves CNF and SMT-LIB2 inputs via the LIBSMT-based frontend. |
| `slot` | source present, not built by default | Translates SMT-LIB2 formulas to LLVM IR and optionally runs optimization passes. |
| `staub` | source present, not built by default | Rewrites SMT formulas with abstract-interpretation-guided integer or floating-point widths. |

## `owl`

`owl` is the supported command-line entry in this directory.

```bash
# Solve a CNF file
build/bin/owl --cnf input.cnf

# Solve an SMT-LIB2 file with extra logging
build/bin/owl --smt input.smt2 --verbose --stats
```

Exit codes follow solver conventions: `10` for SAT, `20` for UNSAT, and other
non-zero values for error or unsupported states.

## Experimental tools

- `slot.cpp` exposes a lower-level workflow for converting SMT-LIB2 formulas to
  LLVM IR, saving pre/post-optimization IR, and selectively enabling passes such
  as `-instcombine`, `-sccp`, `-gvn`, or `-pall`.
- `staub.cpp` provides a width-selection and rewriting workflow for integer and
  floating-point SMT formulas using options such as `-i`, `-r`, and `-l`.

These tools are useful as implementation references even when they are not part
of the default binary set.

## Related documentation

- `lib/Solvers/README.md` covers the solver libraries.
- `lib/Solvers/SMT/LIBSMT/README.md`, `lib/Solvers/SMT/SLOT/README.md`, and
  `lib/Solvers/SMT/STAUB/README.md` provide subsystem-level details.
