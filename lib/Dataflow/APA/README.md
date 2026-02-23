# Algebraic Program Analyis (the Elimination Method for Dataflow Analysis)

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

- Darren R. Cooper. "Concurrent Extensions to Dataflow Analysis". *Proceedings of the ACM SIGPLAN 1983 Symposium on Compiler Construction*, 1983.
  - Early work on elimination methods for dataflow.

### Algebraic Program Analysis (Reps & Kincaid)

- Thomas Reps and Zachary Kincaid. "Algebraic Path Problems, Revisited". 2014.
  - [PDF](https://pages.cs.wisc.edu/~zykincaid/papers/RepsKincaid14.pdf)
  - Introduces the algebraic framework for path problems with Union, Concat, and Star operators over semirings.

- Thomas Reps and Zachary Kincaid. "Algebraic Program Analysis". *International Conference on Computer Aided Verification (CAV)*, 2015.
  - [PDF](https://pages.cs.wisc.edu/~zykincaid/papers/RepsKincaid15.pdf)
  - Formalizes APA and shows how to compute MOP solutions via path expression evaluation.

- Zachary Kincaid. "Algebraic Program Analysis". PhD Thesis, University of Wisconsin-Madison, 2017.
  - [PDF](https://research.cs.wisc.edu/wpis/papers/thesis-kincaid.pdf)
  - Comprehensive treatment including ADT algorithms for reducible flowgraphs.

- Zachary Kincaid, Thomas Reps, and Samuel Stern. "Abstract Destination-Driven Distributive Analysis". *ACM Transactions on Programming Languages and Systems (TOPLAS)*, 2018.
  - [PDF](https://pages.cs.wisc.edu/~zykincaid/papers/KincaidRepsStern18.pdf)
  - The ADT (Abstract Destination-Driven Distributive) algorithms referenced in this implementation.

- Zachary Kincaid, John Cyphert, Jason Breck, and Thomas Reps. "Non-Volatile Memory: Analysis, Optimizations and Applications". *Programming Language Design and Implementation (PLDI)*, 2018.
  - [PDF](https://pages.cs.wisc.edu/~zykincaid/papers/KincaidEtAl18.pdf)
  - Additional applications of APA techniques.

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

