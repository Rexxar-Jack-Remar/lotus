# Algebraic Program Analysis (the Elimination Method for Dataflow Analysis)

This directory provides an **elimination-based** intraprocedural solver that computes dataflow solutions by progressively summarizing paths, conceptually similar to state elimination in automata / Gaussian elimination over path expressions.

## Positioning: what "APA" means here

The directory is named `APA` because it implements an **algebraic, path-expression-based**
style of analysis, including paper-style ADT elimination variants on reducible flowgraphs.

However, this is **not** a full general-purpose implementation of all "Algebraic Program
Analysis" machinery. In Lotus today, this component should be read as:

- an **intraprocedural** APA-inspired elimination engine,
- specialized to LLVM function CFGs,
- aimed at MOP-style dataflow clients (reachable, const-prop, RD, liveness, etc.).

For broader algebraic/interprocedural formulations, see other frameworks in this repository
(e.g., IFDS/IDE, WPDS, and NPA modules).

## References

### Classical Elimination-Based Dataflow Analysis

- Alfred V. Aho, Ravi Sethi, Jeffrey D. Ullman. *Compilers: Principles, Techniques, and Tools* (The "Dragon Book"). Addison-Wesley, 1986.
  - Chapter on data flow analysis covers classical iterative algorithms and elimination methods.

- Steven S. Muchnik. *Advanced Compiler Design and Implementation*. Morgan Kaufmann, 1997.
  - Covers elimination-based dataflow analysis in detail.

### Algebraic Program Analysis (Reps & Kincaid)

- Zachary Kincaid, Thomas Reps, John Cyphert. Algebraic Program Analysis (Invited Tutorial). In *Computer Aided Verification (CAV)*, 2021.
  - [PDF](https://www.cs.princeton.edu/~zkincaid/_static-pub/apa.pdf)

- Zachary Kincaid, Jason Breck, Ashkan Boroujeni Forouhi, Thomas Reps. Compositional Recurrence Analysis Revisited. In *Programming Language Design and Implementation (PLDI)*, 2017.
  - [PDF](https://www.cs.princeton.edu/~zkincaid/_static/pub/pldi17.pdf)

- Azadeh Farzan, Zachary Kincaid. Compositional Recurrence Analysis. In *Formal Methods in Computer-Aided Design (FMCAD)*, 2015.
  - [PDF](https://www.cs.princeton.edu/~zkincaid/_static/pub/fmcad15.pdf)

- Azadeh Farzan, Zachary Kincaid. An Algebraic Framework for Compositional Program Analysis. *ArXiv*, 2013.
  - [Link](https://arxiv.org/abs/1310.3481)

- Zachary Kincaid, Thomas Reps. *Tutorial on Algebraic Program Analysis*. CAV 2021.
  - [Link](https://ucl-pplv.github.io/CAV21/poster_P_k2/)


## What it computes

The solver constructs **path expressions** (regular-expression-like ASTs) over edge transfer functions (`Atom`, `Union`, `Concat`, `Star`) and then evaluates those expressions over your lattice using your `meet` and `applyTransfer`.

This corresponds to a **meet-over-all-paths (MOP)** computation. For classic distributive frameworks, MOP equals the standard maximal fixed point (MFP) solution.

## Current gaps / non-goals

- **Not interprocedural**: this solver works within a single function CFG.
- **Not a universal semiring-equation engine**: it uses path-expression elimination with
  problem-defined `meet`/`applyTransfer`, rather than exposing the full range of algebraic
  solver variants used across APA literature.
- **ADT methods are conditional**: `ADTSimple` / `ADTDelayed` require reducible-graph
  assumptions; the solver falls back to `StateElimination` when assumptions do not hold.
- **No claim of complete APA feature parity**: this module does not attempt to cover all
  formulations (e.g., every interprocedural or Newtonian/tensor-product variant).
- **Engineering tradeoff**: path-expression growth can still be substantial on large CFGs;
  this module focuses on practical intraprocedural analyses rather than full APA scalability
  research coverage.

## Solver methods

Three elimination-style solvers are exposed via `elimination::EliminationOptions`:

- `StateElimination` (default): generic **O(n³)** state-elimination over all nodes (Floyd–Warshall-style).
- `ADTSimple`: **paper-style ADT "simple" algorithm** for **reducible** flowgraphs (O(n²) updates).
- `ADTDelayed`: **paper-style ADT "delayed" algorithm** for **reducible** flowgraphs.

For ADT-based methods, you can optionally implement `elimination::IntraReducibleEliminationProblem`
(dominators + topological order + edge list). If not provided, the solver computes reducible
flowgraph metadata internally and falls back to `StateElimination` when reducibility assumptions fail.


## Intraprocedural LLVM analyses

We provide a few concrete LLVM IR analyses implemented on top of the
elimination framework. These are intended as practical clients (as in the
paper), and serve as examples for adding additional analyses:

- Reachability (`runIntraElimReachable`)
- Constant propagation (`runIntraElimConstantPropagation`)
- Uninitialized variables (`runIntraElimUninitVariables`)
- Reaching definitions (`runIntraElimReachingDefinitions`)
- Available expressions (`runIntraElimAvailableExpressions`)
- Live variables (`runIntraElimLiveVariables`)
- Very busy expressions (`runIntraElimVeryBusyExpressions`)
- Non-null propagation (`runIntraElimNonNull`)

## LLVM pass wrappers

For convenient use under LLVM's legacy pass manager, three function passes are
provided:

- `-elim-reachable` (reachability)
- `-elim-constprop` (constant propagation)
- `-elim-rd` (reaching definitions)
- `-elim-available` (available expressions)
- `-elim-uninit` (uninitialized variables)
- `-elim-live` (live variables)
- `-elim-busy` (very busy expressions)
- `-elim-nonnull` (nonnull propagation)

Use `-elim-method=state|adt-simple|adt-delayed` to select the solver.
Printing is optional via:

- `-elim-reachable-print`
- `-elim-constprop-print`
- `-elim-rd-print`
- `-elim-available-print`
- `-elim-uninit-print`
- `-elim-live-print`
- `-elim-busy-print`
- `-elim-nonnull-print`

Memory modeling can be toggled with:

- `-elim-use-memssa` (default: true) — use MemorySSA to refine memory analyses

## Analysis coverage notes

Constant propagation now tracks full LLVM `Constant*` values (integers, floats,
vectors, aggregates), uses LLVM constant-folding and instruction-simplification
when operands are constant, and performs alias-aware memory updates when
`AAResults` are available. Uninitialized-variable tracking normalizes pointer
bases, uses ValueTracking for guaranteed-non-undef checks, and clears aliasing
locations via `AAResults` when available, plus basic mem intrinsics.

