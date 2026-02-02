# Elimination Dataflow (State Elimination / "Gaussian-style")

This directory provides an **elimination-based** intraprocedural solver that computes dataflow solutions by progressively summarizing paths, conceptually similar to state elimination in automata / Gaussian elimination over path expressions.

## What it computes

The solver constructs **path expressions** (regular-expression-like ASTs) over edge transfer functions (`Atom`, `Union`, `Concat`, `Star`) and then evaluates those expressions over your lattice using your `meet` and `applyTransfer`.

This corresponds to a **meet-over-all-paths (MOP)** computation. For classic distributive frameworks, MOP equals the standard maximal fixed point (MFP) solution.

## Solver methods

Two elimination-style solvers are exposed via `elimination::EliminationOptions`:

- `StateElimination` (default): generic **O(n³)** state-elimination over all nodes (Floyd–Warshall-style).
- `ADTDelayed`: a **paper-style ADT-based** path-expression construction for **reducible** flowgraphs. To use it, implement `elimination::IntraReducibleEliminationProblem` (dominators + topological order + edge list). If the problem does not provide this interface (or assumptions fail), the solver automatically falls back to `StateElimination`.

## Main headers

- `include/Dataflow/Elimination/EliminationFramework.h` — problem interface
- `include/Dataflow/Elimination/PathExpression.h` — path-expression AST
- `include/Dataflow/Elimination/Solver/IntraEliminationSolver.h` — solver
- `include/Dataflow/Elimination/DataFlowResult.h` — result container
