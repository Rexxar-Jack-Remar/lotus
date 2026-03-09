# Newtonian Program Analysis (NPA)

NPA implements Newton-style program analysis over ω-continuous semirings, following the algorithms in Esparza et al. (JACM) and Reps et al. (TOPLAS 2016).
The primary target is **idempotent** semirings; non-idempotent domains are supported when `subtract`/`choose_delta`
returns a valid Newton residual `delta` satisfying `combine(nu, delta) == f(nu)`.
For numeric semirings where exact equality is too strong, a domain may optionally provide `approx_equal(a,b)`; solvers will use it when present.

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
   - **Tensor product** (Reps et al., Alg. 3.4): Convert LCFL system to a tensorized left-linear system, solve there as a regular path problem, then apply tensor-side readout to project back to the base semiring. The implementation uses Tarjan path expressions when the tensorized system can be extracted into a left-linear labeled graph, and otherwise falls back to tensor-space worklist iteration.

## Implementation alignment with the papers

- **Kleene**: \( \kappa^{(i+1)} = f(\kappa^{(i)}) \) — implemented as `KleeneIter` (evaluate all equations under current \( \nu \)).
- **Newton**: \( \nu^{(i+1)} = \nu^{(i)} \sqcup \Delta^{(i)} \), \( \Delta^{(i)} \) = least solution of \( Df|_{\nu^{(i)}}(X) + \delta^{(i)} = X \) — implemented as `NewtonIter`: build RHS = \( \delta + Df|_\nu(X) \) (with \( \delta = f(\nu)-\nu \) or \( f(\nu) \) when idempotent), solve the linear system, then update with the solved correction. For idempotent domains this coincides with Proposition 7.1, so the next approximant is exactly the solved linear-system result.
- **Initial value**: The code uses \( \nu^{(0)} = f(\bot) \) (as in Esparza et al.).
- **Differential** (Esparza et al. Defn. 3.1, 3.5, plus TOPLAS 2016 Sec. 6.2): Term→0, Seq→c·d(t), Call→\( \nu(f)\cdot d(arg) + f(\nu(arg)) \), Cond/Ndet by linearity, Hole→X, Bound→0, **Concat**→\( D(t_1)\cdot \nu_X\cdot t_2 + t_1\cdot X\cdot t_2 + t_1\cdot \nu_X\cdot D(t_2) \), **InfClos**→\( g(\nu)^* \cdot D(g) \cdot g(\nu)^* \). Tensor mode uses the corresponding tensored rule from Sec. 6.2.
- **Tensor differential**: `TensorDiff.h` exposes a direct tensor-side differential builder, so tensor mode no longer needs to be expressed as ordinary differential plus post-hoc conversion.
- **Tensor product** (Reps et al. Alg. 3.4): `TensorSemiringTraits<D>` defines the tensor semiring, coefficient coupling, and tensor-side readout for a base domain. Domains must opt in with an explicit specialization; `paper_admissible()` distinguishes paper-faithful admissible semirings from utility tensorizations used for experiments and tests, and the high-level `TensorProduct` solver path only uses tensor regularization for paper-admissible traits.
- **Tarjan reuse**: the tensor solver caches the dependency-graph regex topology and reuses it across Newton rounds, which is the code-level analogue of Alg. 7.1's parameterized regular-expression reuse; coefficients are still re-evaluated from the current Newton iterate each round.
- **Linearized evaluation**: `Exp1` evaluation uses `extend_lin`, so domains can specialize linearized composition separately from full-summary composition if needed.
- **Tensor extension point**: domains can specialize `TensorSemiringTraits<D>` to provide a paper-faithful admissible tensor semiring, or another tensorized solver domain with explicitly understood tradeoffs.

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
│   ├── TensorDiff.h           # Tensor-side differential
│   ├── LCFLDetector.h         # LCFL structure (Concat/InfClos)
│   ├── LinearSolvers.h        # Worklist, SCC, tensor solvers
│   ├── TensorSemiring.h       # Tensor semiring/readout traits
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
    ├── BackwardInterproceduralEngine.h
    ├── InterproceduralEngine.h
    ├── Intraprocedural/
    │   ├── LiveVariables.h
    │   ├── ReachingDefinitions.h
    │   └── ReachableBlocks.h
    └── Interprocedural/
        ├── InterproceduralAffineEqualities.h
        ├── InterproceduralConstantPropagation.h
        ├── InterproceduralIntervalAnalysis.h
        ├── InterproceduralLiveVariables.h
        ├── InterproceduralMaybeUninitialized.h
        ├── InterproceduralRD.h
        └── InterproceduralTaint.h
```

## Usage

- **Intraprocedural**: local clients such as `BitVectorSolver` can use conventional worklist/Kleene-style solving.
- **Interprocedural (forward)**: `InterproceduralEngine<Domain, Analysis>` builds recursive procedure-summary equations and reports merged valid-path facts at blocks.
- **Interprocedural (backward)**: `BackwardInterproceduralEngine<Domain, Analysis>` provides the analogous backward summary-based engine for clients such as liveness.
- **Linear strategy** (Newton only): `LinearStrategy::Worklist`, `SCC`, or `TensorProduct` (when LCFL).
- **Bounded explicit summaries**: clients built on `ProgramTransferDomain` use an explicit set of paths with bounded size/length. When those bounds are exceeded, summaries conservatively fall back to an overflow approximation instead of remaining exact.
- **Solver status**: direct solver APIs return `Stat`. Interprocedural engines/results expose `AnalysisStatus`, which separates summary solving from later propagation (`summary_solve`, propagation limit flags, and `approximated`) so callers do not mistake phase-1 convergence for an exact whole-analysis result.

## Domain hooks (optional)

- `static constexpr bool commutative_extend`: If declared and true, `NewtonSolver` will cap the default outer iteration bound to `n` (number of equations), matching the JACM termination bound for idempotent commutative semirings.
- If that automatic `n`-step bound does not actually converge, the solver now falls back to an uncapped run instead of silently returning a truncated result.
- `static bool approx_equal(value_type a, value_type b)`: If declared, used instead of `equal()` for convergence/stability checks.
- `static value_type choose_delta(value_type f_nu, value_type nu)`: If declared, used to pick the Newton δ(i) term for **non-idempotent** domains (instead of requiring `subtract`). The solver validates the required invariant `combine(nu, delta) == f_nu` using exact domain equality (`equal`, not `approx_equal`) and throws `InvalidNewtonDeltaError` if the domain violates it.
- `static constexpr int max_fixpoint_iters`: If declared (≥0), caps generic fixpoint iteration (used by `InfClos` and the naive linear solver). This intentionally turns the result into a bounded approximation; `Stat::converged` / `Stat::hit_limit` report that loss of exactness.
- `static constexpr long max_linear_steps`: If declared (≥0), caps worklist/SCC steps when solving the linearized system (useful for numeric semirings). This also yields a bounded approximation rather than the paper's least solution; `Stat::converged` / `Stat::hit_limit` make that visible to callers.
- `ProgramTransferDomain` has separate built-in bounds `max_paths` / `max_path_length`; those affect only explicit-path summary clients such as constant propagation and interval analysis.
