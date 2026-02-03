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
- **TensorProduct**: Rewrite LCFL terms into left-linear form \(Y \otimes_p (a,b)\), lift to the tensor-product semiring, solve there, project back (see `TensorProductDomain.h`). Falls back to worklist if regularization preconditions are not met (non-constant coefficients or `InfClos`).
- **LCFLDetector**: `has_lcfl_structure(E1)` detects Concat/InfClos in linear RHS (used to decide whether tensor is applicable).

Use `NewtonSolver<D>::solve(eqns, verbose, -1, LinearStrategy::SCC)` or `LinearStrategy::TensorProduct`; or pass `LinearStrategy` into `BitVectorSolver::run` (optional 5th parameter).

## Related Work

- Compositional Recurrence Analysis Revisited. PLDI 17.
- Newtonian Program Analysis via Tensor Product. POPL 16.
- Newtonian Program Analysis, JACM 10.
