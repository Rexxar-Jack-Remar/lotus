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

## Main headers

- `include/Dataflow/Elimination/EliminationFramework.h` — problem interface
- `include/Dataflow/Elimination/PathExpression.h` — path-expression AST
- `include/Dataflow/Elimination/Solver/IntraEliminationSolver.h` — solver
- `include/Dataflow/Elimination/DataFlowResult.h` — result container
- `include/Dataflow/Elimination/LLVM/LLVMEliminationProblem.h` — LLVM IR adapter
- `include/Dataflow/Elimination/Analyses/Intraprocedural/EliminationReachable.h`
- `include/Dataflow/Elimination/Analyses/Intraprocedural/EliminationConstantPropagation.h`
- `include/Dataflow/Elimination/Analyses/Intraprocedural/EliminationUninitVariables.h`

## Intraprocedural LLVM analyses

We provide a few concrete LLVM IR analyses implemented on top of the
elimination framework. These are intended as practical clients (as in the
paper), and serve as examples for adding additional analyses:

- Reachability (`runIntraElimReachable`)
- Constant propagation (`runIntraElimConstantPropagation`)
- Uninitialized variables (`runIntraElimUninitVariables`)
