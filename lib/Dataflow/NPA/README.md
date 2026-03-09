# Newtonian Program Analysis

We use the engine in `include/Dataflow/NPA/NPA.h`.

A generic method for solving *interprocedural dataflow equations* by *generalizing Newton’s method** to **ω-continuous semirings*.

The key insight is that Newton’s method can be reformulated **purely algebraically**, without division or limits, and applied to semirings.  
For idempotent semirings this avoids subtraction entirely; non-idempotent domains require a suitable `subtract`.

Analyses are expressed over an **ω-continuous semiring**: $⟨S, +, ·, 0, 1⟩$

- `+`: join / aggregation (may be non-idempotent)
- `·`: sequencing / composition
- supports infinite sums and a natural order `⊑`

This generalizes:
- lattices (classical dataflow analysis),
- language semirings,
- counting semirings,
- probabilistic and cost semirings.



## TOPLAS 2016 / LCFL support

The engine supports **TOPLAS 2016**-style algorithms for LCFL (linear context-free) linear sub-problems:

- **LinearStrategy**: `Naive`, `Worklist`, `SCC`, `TensorProduct`
- **SCC**: Solve in topological order of strongly connected components; fixpoint per SCC.
- **TensorProduct**: Rewrite LCFL terms into a tensorized left-linear system, solve there via Tarjan path expressions when extractable to a left-linear graph, and otherwise fall back to tensor-space worklist iteration.
- **TensorDiff**: Direct tensor-side differential builder used by the Newton tensor path.
- **TensorSemiringTraits**: Optional specialization point for domains that want to supply a custom tensor semiring/readout instead of the default exact-correlated tensor domain.
- **LCFLDetector**: `has_lcfl_structure(E1)` detects Concat/Star in linear RHS (used to decide whether tensor is applicable).

`Star` is the paper-faithful Newton/tensor construct. `Mu` is evaluable as a
generic least fixpoint, but NPA rejects it on Newton/tensor paths.

Domains that expose `project()` must additionally opt into `project_newton_safe`
before projection is accepted on Newton/tensor paths.

Use `NewtonSolver<D>::solve(eqns, verbose, -1, LinearStrategy::SCC)` or `LinearStrategy::TensorProduct`; or pass `LinearStrategy` into `BitVectorSolver::run` (optional 5th parameter).

## Related Work

- Compositional Recurrence Analysis Revisited. PLDI 17.
- Newtonian Program Analysis via Tensor Product. POPL 16.
- Newtonian Program Analysis, JACM 10.
