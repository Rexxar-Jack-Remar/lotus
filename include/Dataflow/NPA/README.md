# Newtonian Program Analysis (NPA)

NPA implements Newton-style program analysis over ω-continuous semirings, following the algorithms in Esparza et al. (JACM) and Reps et al. (TOPLAS 2016).
The primary target is **idempotent** semirings; non-idempotent domains are supported when a suitable `subtract` is provided.

## Algorithm overview

1. **Equation system**: Interprocedural dataflow is formulated as \( \vec{X} = \vec{f}(\vec{X}) \) over a semiring (combine ⊕, extend ⊗, zero ⊥, one 1).

2. **Kleene iteration** (classical): \( \vec{\kappa}^{(0)} = \vec{\bot} \), \( \vec{\kappa}^{(i+1)} = \vec{f}(\vec{\kappa}^{(i)}) \).

3. **Newton iteration** (Esparza et al.): \( \vec{\nu}^{(i+1)} = \vec{\nu}^{(i)} \sqcup \Delta^{(i)} \), where \( \Delta^{(i)} \) is the *least solution* of the *linearized* system:
   \[
   D\vec{f}|_{\vec{\nu}^{(i)}}(\vec{X}) + \vec{\delta}^{(i)} = \vec{X}.
   \]
   The differential \( D\vec{f}|_{\vec{\nu}} \) linearizes \( \vec{f} \) at \( \vec{\nu} \); when ⊗ is non-commutative, this system is an **LCFL equation system** (coefficients on both sides of variables).

4. **Solving the linear system** (each Newton round):
   - **Worklist / SCC**: Chaotic iteration (or SCC-ordered fixpoint) on the linear RHS.
   - **Tensor product** (Reps et al., Alg. 3.4): Convert LCFL system to a *left-linear* system over a *paired* semiring \( (a,b) \otimes_p (a',b') = (a' \otimes a, b \otimes b') \), solve there (regular path problem), then *project* back via \( R((w_1,w_2)) = w_1 \otimes w_2 \). Used only when the system has LCFL structure; falls back to worklist if regularization preconditions are not met (e.g., non-constant coefficients or `InfClos`).

## Implementation alignment with the papers

- **Kleene**: \( \kappa^{(i+1)} = f(\kappa^{(i)}) \) — implemented as `KleeneIter` (evaluate all equations under current \( \nu \)).
- **Newton**: \( \nu^{(i+1)} = \nu^{(i)} \sqcup \Delta^{(i)} \), \( \Delta^{(i)} \) = least solution of \( Df|_{\nu^{(i)}}(X) + \delta^{(i)} = X \) — implemented as `NewtonIter`: build RHS = \( \delta + Df|_\nu(X) \) (with \( \delta = f(\nu)-\nu \) or \( f(\nu) \) when idempotent), solve linear system, then \( \nu' = \Delta \) (idempotent) or \( \nu' = \nu \oplus \Delta \).
- **Initial value**: The code uses \( \nu^{(0)} = f(\bot) \) (as in Esparza et al.).
- **Differential** (Esparza et al. Defn. 3.1, 3.5): Term→0, Seq→c·d(t), Call→\( \nu(f)\cdot d(arg) + f(\nu(arg)) \), Cond/Ndet by linearity, Hole→X, Bound→0, **Concat**→\( D(t_1)\cdot \nu_X\cdot t_2 + t_1\cdot X\cdot t_2 + t_1\cdot \nu_X\cdot D(t_2) \), InfClos→d(body). Implemented in `Diff.h` (with `SeqR` for “expr·constant”).
- **Tensor product** (Reps et al. Alg. 3.4): Paired semiring \( (a_1,b_1)\otimes_p (a_2,b_2) = (a_2\otimes a_1,\, b_1\otimes b_2) \), readout \( R((w_1,w_2)) = w_1\otimes w_2 \). Implemented in `TensorProductDomain.h` (`extend` order and `project` = \( R \)), with LCFL terms rewritten to left-linear form \( Y \otimes_p (a,b) \) when coefficients are constant.

## References

- **Esparza et al.**, "Newtonian Program Analysis", JACM.  
  Differential \( Df|_{\nu} \), Newton sequence, convergence to least fixed point.
- **Reps et al.**, "Newtonian Program Analysis via Tensor Product", TOPLAS 2016.  
  LCFL sub-problems, regularization via tensor product (Defn. 3.1, Alg. 3.4).

## Structure

```
include/Dataflow/NPA/
├── NPA.h                      # Umbrella header; Kleene/Newton, paper refs
├── Core/                      # Core algorithms (see NPA.h for file roles)
│   ├── NPACommon.h            # Domain concept, LinearStrategy
│   ├── Expressions.h          # Exp0 (polynomial) / Exp1 (linearized)
│   ├── Fixpoint.h             # fix / fix_vec
│   ├── Eval.h                 # I0 / I1 evaluators
│   ├── Diff.h                 # Differential Df|ν
│   ├── LCFLDetector.h         # LCFL structure (Concat/InfClos)
│   ├── LinearSolvers.h        # Worklist, SCC, tensor solvers
│   ├── TensorLinearSolve.h    # Tensor-product solver (Alg. 3.4)
│   └── Solver.h               # KleeneIter / NewtonIter
├── Domains/
│   ├── BitVectorDomain.h
│   ├── BitVectorInfo.h
│   ├── GenKillDomain.h
│   ├── TaintTransferDomain.h
│   └── TensorProductDomain.h  # Paired semiring (TOPLAS 2016)
└── Analyses/
    ├── BitVectorSolver.h
    ├── InterproceduralEngine.h
    ├── Intraprocedural/
    │   ├── ReachingDefinitions.h
    │   └── ReachableBlocks.h
    └── Interprocedural/
        ├── InterproceduralRD.h
        └── InterproceduralTaint.h
```

## Usage

- **Intraprocedural**: `BitVectorSolver` + `BitVectorInfo` implementation.
- **Interprocedural**: `InterproceduralEngine<Domain, Analysis>` + analysis policy.
- **Linear strategy** (Newton only): `LinearStrategy::Worklist`, `SCC`, or `TensorProduct` (when LCFL).
