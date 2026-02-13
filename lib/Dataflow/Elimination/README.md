# Elimination Dataflow (State Elimination / "Gaussian-style")

This directory provides an **elimination-based** intraprocedural solver that computes dataflow solutions by progressively summarizing paths, conceptually similar to state elimination in automata / Gaussian elimination over path expressions.

## What it computes

The solver constructs **path expressions** (regular-expression-like ASTs) over edge transfer functions (`Atom`, `Union`, `Concat`, `Star`) and then evaluates those expressions over your lattice using your `meet` and `applyTransfer`.

This corresponds to a **meet-over-all-paths (MOP)** computation. For classic distributive frameworks, MOP equals the standard maximal fixed point (MFP) solution.

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
