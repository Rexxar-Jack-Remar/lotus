# Solver tools

This directory contains standalone frontends for SMT- and SAT-related solver
experiments in `lib/Solvers/`.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

`lotus-solver-datalog` is built by default. `lotus-solver-owl` is built only when
`-DLOTUS_ENABLE_OWL=ON`, and `lotus-solver-smt-stabilizer` only when
`-DLOTUS_ENABLE_SMT_STABILIZER=ON`. The `lotus-solver-staub.cpp` source remains
in the tree as an experimental tool and is not wired into the default build yet.

## Tools

| Tool | Status | Purpose |
| --- | --- | --- |
| `lotus-solver-datalog` | built by default | Solves Datalog/lattice programs via the native, JSON, and Z3 fixedpoint frontends. |
| `lotus-solver-owl` | built only when `LOTUS_ENABLE_OWL=ON` | Solves CNF and SMT-LIB2 inputs via the LIBSMT-based frontend. |
| `lotus-solver-smt-stabilizer` | built only when `LOTUS_ENABLE_SMT_STABILIZER=ON` | Normalizes SMT-LIB2 inputs to reduce syntactic-mutation variance. |
| `lotus-solver-staub` | source present, not built by default | Rewrites SMT formulas with abstract-interpretation-guided integer or floating-point widths. |

## `lotus-solver-owl`

`lotus-solver-owl` is the SMT/SAT command-line entry in this directory.

```bash
# Solve a CNF file
build/bin/lotus-solver-owl --cnf input.cnf

# Solve an SMT-LIB2 file with extra logging
build/bin/lotus-solver-owl --smt input.smt2 --verbose --stats
```

Exit codes follow solver conventions: `10` for SAT, `20` for UNSAT, and other
non-zero values for error or unsupported states.

## Experimental tools

- `lotus-solver-staub.cpp` provides a width-selection and rewriting workflow for
  integer and floating-point SMT formulas using options such as `-i`, `-r`, and
  `-l`.

These tools are useful as implementation references even when they are not part
of the default binary set.

## Related documentation

- `lib/Solvers/README.md` covers the solver libraries.
- `lib/Solvers/SMT/LIBSMT/README.md` and
  `lib/Solvers/SMT/STAUB/README.md` provide subsystem-level details.
- `lib/Solvers/SMT/TUNA/TUNA-Opt/README.md` documents the retained SMT↔LLVM
  translation and optimization toolkit.
